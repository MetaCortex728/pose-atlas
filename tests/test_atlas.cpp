// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "aseprite_parser.hpp"
#include "pose_atlas/atlas.hpp"
#include "pose_atlas/camera.hpp"
#include "pose_atlas/test_rig.hpp"
#include "render_support.hpp"

using namespace pose_atlas;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string fresh_dir(const std::string& name) {
    const fs::path dir = fs::path(POSE_ATLAS_TEST_OUTPUT_DIR) / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir.string();
}

std::string rig_path() {
    static const std::string path = [] {
        const std::string p = std::string(POSE_ATLAS_TEST_OUTPUT_DIR) + "/atlas_rig.glb";
        test_rig::write_glb(p);
        return p;
    }();
    return path;
}

AtlasJob rig_job(int poses = 8, bool small = false) {
    AtlasJob job;
    job.model_path = rig_path();
    job.profile = small ? pose_atlas_test::small_profile() : pose_atlas_test::gltf_profile();
    for (const char* action : {"idle", "run"}) {
        SamplingSpec spec;
        spec.action = action;
        spec.pose_count = poses;
        job.actions.push_back(spec);
    }
    return job;
}

const Atlas& rig_atlas() {
    static const Atlas atlas = render_atlas(pose_atlas_test::renderer(), rig_job());
    return atlas;
}

const Atlas& small_rig_atlas() {
    static const Atlas atlas = render_atlas(pose_atlas_test::renderer(), rig_job(8, true));
    return atlas;
}

json read_json(const std::string& path) { return json::parse(read_text_file(path)); }

bool has_problem(const ValidationReport& report, const std::string& text) {
    for (const auto& p : report.problems) {
        if (p.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("atlas: frame durations round cumulatively") {
    SamplingPlan plan;
    plan.action = "a";
    for (int i = 0; i < 3; ++i) {
        Sample s;
        s.runtime_start_ms = i * 100.0 / 3.0;
        s.runtime_duration_ms = 100.0 / 3.0;
        plan.samples.push_back(s);
    }
    CHECK(frame_durations_ms(plan) == std::vector<int>{33, 34, 33});
    plan.samples[0].runtime_duration_ms.reset();
    CHECK_THROWS_AS(frame_durations_ms(plan), SamplingError);
}

TEST_CASE("atlas: the rig's two actions in eight views export and validate") {
    const Atlas& atlas = rig_atlas();
    CHECK(atlas.columns == 16);
    CHECK(atlas.cells.size() == 128);
    CHECK(atlas.renders.size() == 16);
    CHECK(atlas.pose_names.front() == "idle0");
    CHECK(atlas.pose_names.back() == "run7");
    CHECK(atlas.durations_ms[0] == std::vector<int>(8, 250));  // idle: 2 s / 8
    CHECK(atlas.durations_ms[1] == std::vector<int>(8, 100));  // run: 0.8 s / 8

    const std::string dir = fresh_dir("atlas_export");
    export_atlas(atlas, dir);
    const ValidationReport report = validate_atlas(dir);
    for (const auto& p : report.problems) MESSAGE(p);
    CHECK(report.passed());
    CHECK(report.frames.size() == 128);

    const Image image = decode_png(read_file(dir + "/atlas.png"));
    CHECK(image.width == 16 * 256);
    CHECK(image.height == 8 * 256);
    const json metadata = read_json(dir + "/atlas.json");
    CHECK(metadata["frames"][17]["filename"] == "idle1.ne");
    CHECK(metadata["frames"][17]["frame"] == json({{"x", 256}, {"y", 256}, {"w", 256}, {"h", 256}}));
    CHECK(metadata["frames"][17]["duration"] == 250);
    CHECK(metadata["meta"]["frameTags"].size() == 128);
    CHECK(metadata["meta"]["slices"][0]["keys"][0]["pivot"] == json({{"x", 128}, {"y", 208}}));
    const json contract = read_json(dir + "/pose-contract.json");
    CHECK(contract["sources"]["model"]["sha256"] == sha256_hex(read_file(rig_path())));
    CHECK(contract["sources"]["profile"]["id"] == "quarter-view-30-gltf");
    CHECK_FALSE(contract["sources"].contains("palette"));
    CHECK_FALSE(contract.contains("palette_report"));
    CHECK(contract["actions"][1]["clocks"]["runtime"]["frame_cycle_ms"] == 800);
    CHECK(contract["frames"][8]["source_time_s"] == doctest::Approx(0.0));
    CHECK(contract["frames"][9]["source_time_s"] == doctest::Approx(0.1));

    // The .aseprite file, read with the tests' own parser: one frame per
    // pose and view with its duration, two layers, one tag per state.
    const auto raw = pose_atlas_test::parse_raw_aseprite(read_file(dir + "/atlas.aseprite"));
    CHECK(raw.width == 256);
    CHECK(raw.frames == 128);
    CHECK(raw.frame_list[0].duration == 250);
    CHECK(raw.frame_list[8].duration == 100);
    const AsepriteDocument doc = read_aseprite(read_file(dir + "/atlas.aseprite"));
    REQUIRE(doc.tags.size() == 16);
    CHECK(doc.tags[3].name == "run.ne");
    CHECK(doc.tags[3].from == 24);
    CHECK(doc.tags[3].to == 31);
    CHECK(doc.frames[24].cels[0].image == atlas.cell(1, 8));

    // Exporting again gives the same bytes.
    const std::string again = fresh_dir("atlas_export_again");
    export_atlas(atlas, again);
    CHECK(read_file(again + "/atlas.png") == read_file(dir + "/atlas.png"));
    CHECK(read_file(again + "/atlas.aseprite") == read_file(dir + "/atlas.aseprite"));
    CHECK(read_file(again + "/pose-contract.json") == read_file(dir + "/pose-contract.json"));
}

TEST_CASE("atlas: the validator finds broken exports") {
    const Atlas& atlas = small_rig_atlas();
    const std::string dir = fresh_dir("atlas_broken");

    SUBCASE("changed metadata") {
        export_atlas(atlas, dir);
        json metadata = read_json(dir + "/atlas.json");
        metadata["frames"][3]["duration"] = 999;
        metadata["meta"]["frameTags"][5]["to"] = 6;
        metadata["meta"]["slices"][0]["keys"][0]["pivot"]["x"] = 127;
        write_text_file(dir + "/atlas.json", metadata.dump());
        const auto report = validate_atlas(dir);
        CHECK(has_problem(report, "atlas.json: SHA-256 differs"));
        CHECK(has_problem(report, "frame 3 (idle3.e): duration differs from the contract"));
        CHECK(has_problem(report, "frame 5 (idle5.e): Tag was extended"));
        CHECK(has_problem(report, "foot slice: expected one key on frame 0 with the profile's pivot"));
    }
    SUBCASE("clipped, blank and duplicate cells") {
        Atlas broken = atlas;
        broken.cells[0].pixel(0, 40)[3] = 255;                  // idle0.e touches the edge
        broken.cells[1] = Image(96, 96);                        // idle1.e blank
        broken.cells[16 + 2] = broken.cells[2];                 // idle2.ne repeats idle2.e
        for (int c = 9; c < 16; ++c) broken.cells[static_cast<std::size_t>(32 + c)] = broken.cells[32 + 8];  // run in n
        export_atlas(broken, dir);
        const auto report = validate_atlas(dir);
        CHECK(has_problem(report, "frame 0 (idle0.e): Clipping"));
        CHECK(has_problem(report, "frame 1 (idle1.e): blank pose"));
        CHECK(has_problem(report, "Directions did not change: pose idle2 has identical views"));
        CHECK(has_problem(report, "Poses did not change: action run shows one image in view n"));
        CHECK_FALSE(has_problem(report, "action idle shows one image"));
        CHECK_FALSE(has_problem(report, "SHA-256"));
    }
    SUBCASE("changed .aseprite") {
        export_atlas(atlas, dir);
        AsepriteDocument doc = aseprite_document(atlas);
        doc.frames[7].cels[0].image.pixel(48, 40)[0] ^= 1;
        doc.frames[2].cels.push_back({1, 0, 0, Image(2, 2)});
        doc.tags.pop_back();
        write_file(dir + "/atlas.aseprite", write_aseprite(doc));
        const auto report = validate_atlas(dir);
        CHECK(has_problem(report, "atlas.aseprite: SHA-256 differs"));
        CHECK(has_problem(report, "frame 7 (idle7.e): Import/export changed raw RGBA pixels"));
        CHECK(has_problem(report, "frame 2 has a cel on the corrections layer"));
        CHECK(has_problem(report, "tags are not one per state"));
    }
    SUBCASE("contract inconsistent with its profile") {
        export_atlas(atlas, dir);
        json contract = read_json(dir + "/pose-contract.json");
        contract["pivot"] = {40, 78};
        contract["cell_span_m"] = 2.0;
        contract["frames"][9]["projected_heading"] = {0.0, 1.0};
        write_text_file(dir + "/pose-contract.json", contract.dump());
        const auto report = validate_atlas(dir);
        CHECK(has_problem(report, "contract: pivot does not match"));
        CHECK(has_problem(report, "Physical framing changed"));
        CHECK(has_problem(report, "frame 9 (run1.e): Facing does not project"));
    }
    SUBCASE("missing or foreign files") {
        CHECK(has_problem(validate_atlas(dir), "cannot continue"));
        write_text_file(dir + "/pose-contract.json", "{\"format\": \"other\"}");
        CHECK(has_problem(validate_atlas(dir), "not a pose-atlas-contract"));
    }
}

TEST_CASE("atlas: the content-hash cache re-renders only what changed") {
    const std::string cache = fresh_dir("atlas_cache");
    auto& renderer = pose_atlas_test::renderer();
    auto reused = [](const Atlas& atlas, const std::string& action) {
        int n = 0;
        for (const auto& r : atlas.renders) n += (r.action == action && r.reused) ? 1 : 0;
        return n;
    };

    AtlasJob job = rig_job(4, true);
    const Atlas first = render_atlas(renderer, job, cache);
    CHECK(reused(first, "idle") == 0);
    CHECK(reused(first, "run") == 0);
    CHECK(std::distance(fs::directory_iterator(cache), fs::directory_iterator()) == 16);

    // Same inputs: everything comes from the cache, with the same pixels.
    const Atlas second = render_atlas(renderer, job, cache);
    CHECK(reused(second, "idle") == 8);
    CHECK(reused(second, "run") == 8);
    CHECK(second.cells == first.cells);

    // Change one action (its pose count): only that action is rendered again.
    job.actions[1].pose_count = 6;
    const Atlas third = render_atlas(renderer, job, cache);
    CHECK(reused(third, "idle") == 8);
    CHECK(reused(third, "run") == 0);
    CHECK(third.cell(3, 1) == first.cell(3, 1));
    // ... and it matches an uncached render.
    const Atlas uncached = render_atlas(renderer, job);
    CHECK(uncached.cells == third.cells);
    for (std::size_t i = 0; i < third.renders.size(); ++i) CHECK(third.renders[i].key == uncached.renders[i].key);

    // A different runtime cadence does not change the pixels: all reused.
    job.actions[0].cadence.kind = Cadence::Kind::FixedFrame;
    job.actions[0].cadence.frame_ms = 120;
    const Atlas fourth = render_atlas(renderer, job, cache);
    CHECK(reused(fourth, "idle") == 8);
    CHECK(reused(fourth, "run") == 8);
    CHECK(fourth.durations_ms[0] == std::vector<int>(4, 120));

    // Render settings and the profile key every render.
    job.settings.style.outline = true;
    CHECK(reused(render_atlas(renderer, job, cache), "idle") == 0);
    // So does the palette's content hash; the contract names the palette.
    job.settings = {};
    job.settings.palette = {{0x1a, 0x14, 0x1e}, {0xff, 0xff, 0xff}};
    job.settings.palette_stage = PaletteStage::BeforeAfter;
    job.palette_path = "two.json";
    const Atlas with_palette = render_atlas(renderer, job, cache);
    CHECK(reused(with_palette, "idle") == 0);
    CHECK(with_palette.renders[0].key != fourth.renders[0].key);
    const std::string palette_dir = fresh_dir("atlas_palette_contract");
    export_atlas(with_palette, palette_dir);
    const json contract = read_json(palette_dir + "/pose-contract.json");
    CHECK(contract["sources"]["palette"]["path"] == "two.json");
    CHECK(contract["sources"]["palette"]["sha256"] == palette_sha256(job.settings.palette));
    CHECK(contract["sources"]["palette"]["colors"] == json({"#1a141e", "#ffffff"}));
    CHECK(contract["sources"]["render_settings"]["values"]["palette_sha256"] == palette_sha256(job.settings.palette));
    job.palette_path.clear();
    job.settings = {};
    job.profile.resampling_filter = ResampleFilter::Box;
    CHECK(reused(render_atlas(renderer, job, cache), "idle") == 0);

    // A damaged entry is rendered again rather than trusted.
    job = rig_job(4, true);
    write_file(cache + "/" + first.renders[0].key + ".png", {1, 2, 3});
    const Atlas repaired = render_atlas(renderer, job, cache);
    CHECK(reused(repaired, "idle") == 7);
    CHECK(repaired.cells == first.cells);
}

TEST_CASE("atlas: palette report, before versus before_after") {
    // The open question: is snapping the supersampled render to the palette
    // (before) enough, or is a final snap after the Mitchell downsample also
    // needed (before_after)? Rendered with the rig's own colours.
    auto& renderer = pose_atlas_test::renderer();
    AtlasJob job = rig_job(4, true);
    job.settings.palette = test_rig::palette();
    job.palette_path = "palettes/test-rig.json";
    PaletteMatcher matcher(job.settings.palette);

    job.settings.palette_stage = PaletteStage::Before;
    const std::string cache = fresh_dir("atlas_palette_cache");
    const Atlas before = render_atlas(renderer, job, cache);
    job.settings.palette_stage = PaletteStage::BeforeAfter;
    const Atlas both = render_atlas(renderer, job);

    const PaletteStats b = before.palette_total(), ba = both.palette_total();
    MESSAGE("before:       " << palette_summary(before));
    MESSAGE("before_after: " << palette_summary(both));
    CHECK(b.opaque > 10000);
    // Before alone leaves off-palette pixels (the downsample blends regions);
    // with no final snap they reach the output.
    CHECK(b.off_palette_before_snap > 0);
    CHECK(b.off_palette_output == b.off_palette_before_snap);
    // The same pipeline up to the final snap, then none are left.
    CHECK(ba.off_palette_before_snap == b.off_palette_before_snap);
    CHECK(ba.partial_alpha == b.partial_alpha);
    CHECK(ba.off_palette_output == 0);
    CHECK(b.partial_alpha > 0);

    // Every off-palette pixel of `before` is a Mitchell blend at a region
    // border: the filter's footprint in the snapped supersampled render
    // (output pixel centre +- 2 px) holds more than one colour or coverage.
    // Pixels whose footprint is uniform are on the palette. Checked on the
    // first idle pose in all eight views.
    {
        const Profile& profile = job.profile;
        const int ss = profile.supersampling;
        const Model& model = pose_atlas_test::rig();
        const Pose pose = evaluate(model, &model.animation("idle"), 0.0);
        RenderSettings settings = job.settings;
        settings.palette_stage = PaletteStage::Before;
        int off = 0, off_uniform = 0, uniform_opaque = 0;
        for (int view = 0; view < 8; ++view) {
            const CameraGuide guide = camera_guide(profile, view);
            LinearImage snapped = renderer.render_linear(model, pose, profile, guide, settings);
            snap_linear(snapped, matcher);
            const Image cell = renderer.render(model, pose, profile, guide, settings);
            CHECK(cell == before.cell(view, 0));
            auto at = [&](int sx, int sy) {
                return snapped.rgba.data() +
                       (static_cast<std::size_t>(sy) * static_cast<std::size_t>(snapped.width) + static_cast<std::size_t>(sx)) * 4;
            };
            auto footprint = [&](int centre) {
                std::vector<int> taps;
                for (int k = (centre - 2) * ss; k < (centre + 3) * ss; ++k) {
                    if (std::fabs((k + 0.5) / ss - (centre + 0.5)) < 2.0 && k >= 0 && k < snapped.width) taps.push_back(k);
                }
                return taps;
            };
            for (int y = 0; y < cell.height; ++y) {
                const auto ys = footprint(y);
                for (int x = 0; x < cell.width; ++x) {
                    const std::uint8_t* p = cell.pixel(x, y);
                    if (p[3] == 0) continue;
                    const auto xs = footprint(x);
                    bool uniform = true;
                    const float* first = at(xs[0], ys[0]);
                    for (int sy : ys) {
                        for (int sx : xs) {
                            const float* q = at(sx, sy);
                            uniform = uniform && std::equal(first, first + 4, q);
                        }
                    }
                    const bool on_palette = matcher.contains({p[0], p[1], p[2]});
                    off += on_palette ? 0 : 1;
                    off_uniform += (!on_palette && uniform) ? 1 : 0;
                    uniform_opaque += uniform ? 1 : 0;
                }
            }
        }
        MESSAGE("idle0, 8 views: " << off << " off-palette pixels, " << off_uniform
                                   << " of them with a uniform footprint; " << uniform_opaque
                                   << " opaque pixels with a uniform footprint");
        CHECK(off > 0);
        CHECK(uniform_opaque > 0);
        CHECK(off_uniform == 0);
    }
    for (const Image& cell : both.cells) {
        for (int y = 0; y < cell.height; ++y) {
            for (int x = 0; x < cell.width; ++x) {
                const std::uint8_t* p = cell.pixel(x, y);
                CHECK((p[3] == 0 || p[3] == 255));
                if (p[3] == 255) CHECK(matcher.contains({p[0], p[1], p[2]}));
            }
        }
    }

    // Per (action, view) numbers add up; the contract carries them; the
    // cache keeps them.
    PaletteStats sum;
    for (const RenderRecord& r : before.renders) sum += r.palette;
    CHECK(sum == b);
    const std::string dir = fresh_dir("atlas_palette_report");
    export_atlas(before, dir);
    const json report = read_json(dir + "/pose-contract.json")["palette_report"];
    CHECK(report["stage"] == "before");
    CHECK(report["alpha_threshold"] == 0.5);
    CHECK(report["palette_sha256"] == palette_sha256(job.settings.palette));
    CHECK(report["outline_colour"].is_null());
    REQUIRE(report["renders"].size() == 16);
    CHECK(report["renders"][9]["action"] == "run");
    CHECK(report["renders"][9]["view"] == "ne");
    CHECK(report["renders"][9]["opaque"] == before.renders[9].palette.opaque);
    CHECK(report["total"]["off_palette_before_snap"] == b.off_palette_before_snap);
    CHECK(report["total"]["partial_alpha"] == b.partial_alpha);
    CHECK(report["total"]["off_palette_output"] == b.off_palette_output);
    job.settings.palette_stage = PaletteStage::Before;
    const Atlas cached = render_atlas(renderer, job, cache);
    for (const RenderRecord& r : cached.renders) CHECK(r.reused);
    CHECK(cached.palette_total() == b);
    CHECK(cached.cells == before.cells);
    // An entry that lost its statistics is rendered again.
    fs::remove(cache + "/" + before.renders[0].key + ".palette.json");
    const Atlas repaired = render_atlas(renderer, job, cache);
    CHECK_FALSE(repaired.renders[0].reused);
    CHECK(repaired.renders[1].reused);
    CHECK(repaired.palette_total() == b);
}
