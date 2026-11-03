// SPDX-License-Identifier: Apache-2.0
#include "pose_atlas/atlas.hpp"

#include <cmath>
#include <cstdio>
#include <set>

#include <nlohmann/json.hpp>

#include "pose_atlas/camera.hpp"
#include "pose_atlas/version.hpp"

namespace pose_atlas {

namespace {

using nlohmann::ordered_json;

std::string join_path(const std::string& directory, const std::string& name) {
    if (directory.empty() || directory.back() == '/') return directory + name;
    return directory + "/" + name;
}

std::string exact(double value) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.17g", value);
    return buffer;
}

ordered_json rect(int x, int y, int w, int h) { return {{"x", x}, {"y", y}, {"w", w}, {"h", h}}; }

ordered_json optional_number(const std::optional<double>& value) {
    return value ? ordered_json(*value) : ordered_json(nullptr);
}

}  // namespace

std::vector<int> frame_durations_ms(const SamplingPlan& plan) {
    std::vector<int> out;
    for (const Sample& s : plan.samples) {
        if (!s.runtime_start_ms || !s.runtime_duration_ms) {
            throw SamplingError("action \"" + plan.action +
                                "\": the cadence gives no frame durations (a distance cadence needs a reference "
                                "speed)");
        }
        const double start = std::round(*s.runtime_start_ms);
        const double end = std::round(*s.runtime_start_ms + *s.runtime_duration_ms);
        const double d = end - start;
        if (!(d >= 1.0) || d > 65535.0) {
            throw SamplingError("action \"" + plan.action + "\": pose " + std::to_string(s.pose_index) +
                                " lasts " + exact(*s.runtime_duration_ms) +
                                " ms, outside the 1 to 65535 ms an atlas frame can hold");
        }
        out.push_back(static_cast<int>(d));
    }
    return out;
}

std::string render_key(const Atlas& atlas, const SamplingPlan& plan, int view) {
    std::string text = "pose-atlas render key 1\n";
    text += "model " + atlas.model_sha256 + "\n";
    text += "profile " + atlas.profile_sha256 + "\n";
    text += "settings " + atlas.settings_sha256 + "\n";
    text += "renderer " + atlas.renderer + "\n";
    text += "action " + plan.action + "\n";
    for (const Sample& s : plan.samples) text += "time " + exact(s.source_time_s) + "\n";
    text += std::string("view ") + kScreenHeadings[static_cast<std::size_t>(view)] + "\n";
    return sha256_hex(text);
}

Atlas render_atlas(Renderer& renderer, const AtlasJob& job) {
    if (job.actions.empty()) throw std::invalid_argument("render_atlas: no actions");
    const auto problems = validate_profile(job.profile);
    if (!problems.empty()) throw ProfileError("profile", problems);
    std::set<std::string> seen;
    for (const auto& spec : job.actions) {
        if (!seen.insert(spec.action).second) {
            throw std::invalid_argument("action \"" + spec.action + "\" is listed twice");
        }
    }
    const std::vector<std::uint8_t> model_bytes = read_file(job.model_path);
    const Model model = load_gltf(model_bytes, job.model_path);

    Atlas atlas;
    atlas.model_path = job.model_path;
    atlas.model_sha256 = sha256_hex(model_bytes);
    atlas.profile = job.profile;
    atlas.profile_sha256 = sha256_hex(profile_to_json(job.profile));
    atlas.settings = job.settings;
    atlas.settings_sha256 = sha256_hex(render_settings_json(job.settings));
    atlas.renderer = renderer.description();
    for (const auto& spec : job.actions) {
        atlas.plans.push_back(plan_sampling(model, spec));
        atlas.durations_ms.push_back(frame_durations_ms(atlas.plans.back()));
        for (const Sample& s : atlas.plans.back().samples) {
            atlas.pose_names.push_back(spec.action + std::to_string(s.pose_index));
        }
    }
    atlas.columns = static_cast<int>(atlas.pose_names.size());
    atlas.cells.resize(static_cast<std::size_t>(atlas.columns) * kScreenHeadings.size());

    int first_column = 0;
    for (const SamplingPlan& plan : atlas.plans) {
        const Animation& animation = model.animation(plan.action);
        std::vector<Pose> poses;
        for (const Sample& s : plan.samples) poses.push_back(evaluate(model, &animation, s.source_time_s));
        for (int view = 0; view < static_cast<int>(kScreenHeadings.size()); ++view) {
            const CameraGuide guide = camera_guide(job.profile, view);
            for (std::size_t i = 0; i < poses.size(); ++i) {
                atlas.cells[static_cast<std::size_t>(view * atlas.columns + first_column) + i] =
                    renderer.render(model, poses[i], job.profile, guide, job.settings);
            }
            atlas.renders.push_back(
                {plan.action, kScreenHeadings[static_cast<std::size_t>(view)], render_key(atlas, plan, view)});
        }
        first_column += plan.pose_count;
    }
    return atlas;
}

Image atlas_image(const Atlas& atlas) {
    const int cell = atlas.profile.cell_pixels;
    const int views = static_cast<int>(kScreenHeadings.size());
    Image image(cell * atlas.columns, cell * views);
    for (int view = 0; view < views; ++view) {
        for (int column = 0; column < atlas.columns; ++column) {
            blit(image, atlas.cell(view, column), column * cell, view * cell);
        }
    }
    return image;
}

namespace {

// Per-frame bookkeeping shared by the metadata and the contract.
struct FrameInfo {
    std::string name;
    std::size_t action = 0;
    int pose = 0;
    int view = 0;
    int column = 0;
};

std::vector<FrameInfo> frame_infos(const Atlas& atlas) {
    std::vector<FrameInfo> out;
    for (int view = 0; view < static_cast<int>(kScreenHeadings.size()); ++view) {
        int column = 0;
        for (std::size_t a = 0; a < atlas.plans.size(); ++a) {
            for (int pose = 0; pose < atlas.plans[a].pose_count; ++pose, ++column) {
                out.push_back({atlas.pose_names[static_cast<std::size_t>(column)] + "." +
                                   kScreenHeadings[static_cast<std::size_t>(view)],
                               a, pose, view, column});
            }
        }
    }
    return out;
}

}  // namespace

std::string atlas_metadata_json(const Atlas& atlas, const std::string& image_file) {
    const int cell = atlas.profile.cell_pixels;
    ordered_json frames = ordered_json::array();
    ordered_json tags = ordered_json::array();
    const auto infos = frame_infos(atlas);
    for (std::size_t i = 0; i < infos.size(); ++i) {
        const FrameInfo& f = infos[i];
        frames.push_back({{"filename", f.name},
                          {"frame", rect(f.column * cell, f.view * cell, cell, cell)},
                          {"rotated", false},
                          {"trimmed", false},
                          {"spriteSourceSize", rect(0, 0, cell, cell)},
                          {"sourceSize", {{"w", cell}, {"h", cell}}},
                          {"duration", atlas.durations_ms[f.action][static_cast<std::size_t>(f.pose)]}});
        tags.push_back({{"name", f.name}, {"from", i}, {"to", i}, {"direction", "forward"}, {"color", "#000000ff"}});
    }
    ordered_json meta;
    meta["app"] = "pose-atlas";
    meta["version"] = version();
    meta["image"] = image_file;
    meta["format"] = "RGBA8888";
    meta["size"] = {{"w", cell * atlas.columns}, {"h", cell * static_cast<int>(kScreenHeadings.size())}};
    meta["scale"] = "1";
    meta["frameTags"] = tags;
    meta["layers"] = ordered_json::array({{{"name", "Render"}, {"opacity", 255}, {"blendMode", "normal"}}});
    meta["slices"] = ordered_json::array(
        {{{"name", "foot"},
          {"color", "#0000ffff"},
          {"keys",
           ordered_json::array({{{"frame", 0},
                                 {"bounds", rect(0, 0, cell, cell)},
                                 {"pivot",
                                  {{"x", atlas.profile.ground_pivot_pixels[0]},
                                   {"y", atlas.profile.ground_pivot_pixels[1]}}}}})}}});
    ordered_json root;
    root["frames"] = frames;
    root["meta"] = meta;
    return root.dump(1) + "\n";
}

AsepriteDocument aseprite_document(const Atlas& atlas) {
    const int cell = atlas.profile.cell_pixels;
    AsepriteDocument doc;
    doc.width = cell;
    doc.height = cell;
    doc.layers = {kAsepriteBaseLayer, kAsepriteCorrectionsLayer};
    for (const FrameInfo& f : frame_infos(atlas)) {
        AsepriteFrame frame;
        frame.duration_ms = atlas.durations_ms[f.action][static_cast<std::size_t>(f.pose)];
        frame.cels.push_back({0, 0, 0, atlas.cell(f.view, f.column)});
        doc.frames.push_back(std::move(frame));
    }
    for (int view = 0; view < static_cast<int>(kScreenHeadings.size()); ++view) {
        int first = 0;
        for (const SamplingPlan& plan : atlas.plans) {
            const int from = view * atlas.columns + first;
            doc.tags.push_back({plan.action + "." + kScreenHeadings[static_cast<std::size_t>(view)], from,
                                from + plan.pose_count - 1});
            first += plan.pose_count;
        }
    }
    AsepriteSliceKey key;
    key.width = cell;
    key.height = cell;
    key.pivot = std::array<int, 2>{atlas.profile.ground_pivot_pixels[0], atlas.profile.ground_pivot_pixels[1]};
    doc.slices.push_back({"foot", {key}});
    return doc;
}

std::vector<std::string> export_atlas(const Atlas& atlas, const std::string& directory) {
    const auto png = encode_png(atlas_image(atlas));
    const std::string metadata = atlas_metadata_json(atlas, kAtlasPng);
    write_file(join_path(directory, kAtlasPng), png);
    write_text_file(join_path(directory, kAtlasJson), metadata);
    const auto aseprite = write_aseprite(aseprite_document(atlas));
    write_file(join_path(directory, kAtlasAseprite), aseprite);

    const Profile& p = atlas.profile;
    ordered_json c;
    c["format"] = kContractFormat;
    c["version"] = kContractVersion;
    c["tool"] = "pose-atlas " + version();
    c["renderer"] = atlas.renderer;
    c["sources"] = {
        {"model", {{"path", atlas.model_path}, {"sha256", atlas.model_sha256}}},
        {"profile", {{"id", p.id}, {"sha256", atlas.profile_sha256}, {"values", ordered_json::parse(profile_to_json(p))}}},
        {"render_settings",
         {{"sha256", atlas.settings_sha256}, {"values", ordered_json::parse(render_settings_json(atlas.settings))}}}};
    c["cell"] = {p.cell_pixels, p.cell_pixels};
    c["pivot"] = {p.ground_pivot_pixels[0], p.ground_pivot_pixels[1]};
    c["meters_per_pixel"] = p.meters_per_pixel;
    c["cell_span_m"] = p.cell_span_m();
    c["elevation_degrees"] = p.elevation_degrees;
    c["supersampling"] = p.supersampling;
    c["resampling_filter"] = to_string(p.resampling_filter);
    c["view_count"] = kScreenHeadings.size();
    c["views"] = kScreenHeadings;
    c["columns"] = atlas.columns;
    c["pose_names"] = atlas.pose_names;

    ordered_json actions = ordered_json::array();
    int first_column = 0;
    for (std::size_t a = 0; a < atlas.plans.size(); ++a) {
        const SamplingPlan& plan = atlas.plans[a];
        ordered_json renders = ordered_json::object();
        for (const RenderRecord& r : atlas.renders) {
            if (r.action == plan.action) renders[r.view] = r.key;
        }
        int cycle_ms = 0;
        for (int d : atlas.durations_ms[a]) cycle_ms += d;
        actions.push_back(
            {{"name", plan.action},
             {"first_column", first_column},
             {"pose_count", plan.pose_count},
             {"end_point", to_string(plan.endpoint)},
             {"clocks",
              {{"source", {{"start_s", plan.source_start_s}, {"end_s", plan.source_end_s}, {"step_s", plan.source_step_s}}},
               {"pose", {{"count", plan.pose_count}, {"intervals", plan.intervals}}},
               {"runtime",
                {{"cadence", to_string(plan.cadence.kind)},
                 {"playback_rate", optional_number(plan.playback_rate)},
                 {"cycle_ms", optional_number(plan.runtime_cycle_ms)},
                 {"cycle_m", optional_number(plan.runtime_cycle_m)},
                 {"frame_cycle_ms", cycle_ms}}}}},
             {"loop_closure",
              {{"max_position_delta_m", plan.loop_closure.max_position_delta_m},
               {"max_rotation_delta_degrees", plan.loop_closure.max_rotation_delta_degrees}}},
             {"warnings", plan.warnings},
             {"render_keys", renders}});
        first_column += plan.pose_count;
    }
    c["actions"] = actions;

    ordered_json frames = ordered_json::array();
    for (const FrameInfo& f : frame_infos(atlas)) {
        const SamplingPlan& plan = atlas.plans[f.action];
        const Sample& s = plan.samples[static_cast<std::size_t>(f.pose)];
        const Vec2 forward = projected_forward(p, camera_guide(p, f.view));
        const double length = std::hypot(forward.x, forward.y);
        frames.push_back({{"name", f.name},
                          {"action", plan.action},
                          {"pose_index", f.pose},
                          {"view", kScreenHeadings[static_cast<std::size_t>(f.view)]},
                          {"source_time_s", s.source_time_s},
                          {"runtime_start_ms", optional_number(s.runtime_start_ms)},
                          {"duration_ms", atlas.durations_ms[f.action][static_cast<std::size_t>(f.pose)]},
                          {"projected_heading", {forward.x / length, forward.y / length}}});
    }
    c["frames"] = frames;
    c["files"] = {{"atlas", {{"file", kAtlasPng}, {"sha256", sha256_hex(png)}}},
                  {"metadata", {{"file", kAtlasJson}, {"sha256", sha256_hex(metadata)}}},
                  {"aseprite", {{"file", kAtlasAseprite}, {"sha256", sha256_hex(aseprite)}}}};
    write_text_file(join_path(directory, kContractJson), c.dump(2) + "\n");
    return {kAtlasPng, kAtlasJson, kAtlasAseprite, kContractJson};
}

}  // namespace pose_atlas
