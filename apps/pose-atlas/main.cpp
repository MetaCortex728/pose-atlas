// SPDX-License-Identifier: Apache-2.0
// pose-atlas command-line tool.

#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pose_atlas/atlas.hpp"
#include "pose_atlas/camera.hpp"
#include "pose_atlas/corrections.hpp"
#include "pose_atlas/model.hpp"
#include "pose_atlas/profile.hpp"
#include "pose_atlas/sampling.hpp"
#include "pose_atlas/test_rig.hpp"
#include "pose_atlas/version.hpp"

namespace {

using nlohmann::ordered_json;
using Args = std::vector<std::string>;

constexpr const char* kUsage =
    "usage: pose-atlas <command> [arguments]\n"
    "\n"
    "commands:\n"
    "  profile check <profile.json>    validate a profile and print a summary\n"
    "  profile guides <profile.json>   print the eight camera guides as JSON\n"
    "  make-test-rig <out.glb>         write the generated test character\n"
    "  inspect <model.glb>             print nodes, skins and animations as JSON\n"
    "  plan <model.glb> --action NAME --poses N [options]\n"
    "                                  print a sampling plan with its three clocks:\n"
    "      --include-end               sample the end of the range (one-shots)\n"
    "      --start S --end S           source range in seconds (default: whole clip)\n"
    "      --rate R                    runtime = source time / R (default 1)\n"
    "      --frame-ms MS               fixed runtime duration per pose\n"
    "      --cycle-m M [--speed-mps V] distance-driven cycle, optional reference speed\n"
    "  render <model.glb> --profile <profile.json> --actions A,B --out <dir> [options]\n"
    "                                  render every action in eight views and export\n"
    "                                  atlas.png, atlas.json, atlas.aseprite and\n"
    "                                  pose-contract.json, then validate them:\n"
    "      --poses N | A=N,B=M         poses per action (default 8)\n"
    "      --rate R | --frame-ms MS    runtime cadence (default: source rate 1)\n"
    "      --outline                   one-pixel outline, hard edges\n"
    "      --texture-filter F          nearest (default) or bilinear base colour textures\n"
    "      --hide-material NAME        do not draw this material (repeatable)\n"
    "      --palette FILE              palette: JSON {\"colors\": [\"#rrggbb\", ...]} or a PNG\n"
    "                                  whose distinct opaque pixels are the colours (2 to 256)\n"
    "      --palette-stage S           where to snap to the palette: before (supersampled\n"
    "                                  render), after (cell) or before_after (default)\n"
    "      --alpha-threshold A         with a palette, coverage at which a pixel becomes\n"
    "                                  opaque; the rest becomes transparent (default 0.5)\n"
    "      --cache DIR | --no-cache    render cache (default <out>/cache)\n"
    "      --erase-color #RRGGBB       corrections pixels of this colour make the atlas pixel\n"
    "                                  transparent (default #ff00ff)\n"
    "      --discard-corrections       do not carry the manual corrections of an existing\n"
    "                                  atlas.aseprite into the new one (it is still backed\n"
    "                                  up as atlas.before-<hash>.aseprite)\n"
    "  validate <dir> [--report FILE]  check an exported directory against its contract\n"
    "  version                         print the version\n"
    "  help                            print this help\n";

class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

ordered_json vec_json(const pose_atlas::Vec3& v) { return {v.x, v.y, v.z}; }

int profile_command(const Args& args) {
    if (args.size() != 3) throw UsageError("profile: expected 'profile check|guides <profile.json>'");
    const std::string& action = args[1];
    if (action != "check" && action != "guides") {
        throw UsageError("profile: unknown action '" + action + "'");
    }
    const pose_atlas::Profile profile = pose_atlas::load_profile(args[2]);
    if (action == "check") {
        std::cout << args[2] << ": valid profile \"" << profile.id << "\" (version "
                  << pose_atlas::kProfileVersion << ")\n"
                  << "  orthographic, elevation " << profile.elevation_degrees << " deg, azimuth "
                  << profile.base_azimuth_degrees << " deg, distance " << profile.camera_distance_m
                  << " m\n"
                  << "  cell " << profile.cell_pixels << " px = " << profile.cell_span_m()
                  << " m, pivot (" << profile.ground_pivot_pixels[0] << ", "
                  << profile.ground_pivot_pixels[1] << ")\n"
                  << "  forward " << pose_atlas::to_string(profile.character_forward) << ", up "
                  << pose_atlas::to_string(profile.world_up) << ", supersampling "
                  << profile.supersampling << "x "
                  << pose_atlas::to_string(profile.resampling_filter) << "\n";
        return 0;
    }
    ordered_json out;
    out["profile"] = profile.id;
    out["frame"] = "model";
    out["guides"] = ordered_json::array();
    for (int i = 0; i < 8; ++i) {
        const auto guide = pose_atlas::camera_guide(profile, i);
        const auto forward = pose_atlas::projected_forward(profile, guide);
        out["guides"].push_back({{"direction", guide.direction},
                                 {"heading_degrees", i * 45},
                                 {"toward", vec_json(guide.toward)},
                                 {"right", vec_json(guide.right)},
                                 {"up", vec_json(guide.up)},
                                 {"position", vec_json(guide.position)},
                                 {"ortho_scale_m", guide.ortho_scale_m},
                                 {"camera_rotation_degrees", guide.camera_rotation_degrees},
                                 {"vertical_projection_factor", guide.vertical_projection_factor},
                                 {"ground_depth_factor", guide.ground_depth_factor},
                                 {"projected_forward", {forward.x, forward.y}}});
    }
    std::cout << out.dump(2) << "\n";
    return 0;
}

double parse_number(const std::string& option, const std::string& text) {
    std::size_t used = 0;
    double value = 0.0;
    try {
        value = std::stod(text, &used);
    } catch (const std::exception&) {
        used = 0;
    }
    if (used != text.size() || text.empty() || !std::isfinite(value)) {
        throw UsageError(option + ": expected a number, got '" + text + "'");
    }
    return value;
}

int inspect_command(const Args& args) {
    if (args.size() != 2) throw UsageError("inspect: expected 'inspect <model.glb>'");
    const pose_atlas::Model model = pose_atlas::load_gltf(args[1]);
    ordered_json out;
    out["source"] = model.source;
    out["nodes"] = ordered_json::array();
    for (const auto& node : model.nodes) {
        ordered_json n = {{"name", node.name}, {"parent", node.parent}};
        if (node.mesh >= 0) n["mesh"] = model.meshes[static_cast<std::size_t>(node.mesh)].name;
        if (node.skin >= 0) n["skin"] = model.skins[static_cast<std::size_t>(node.skin)].name;
        out["nodes"].push_back(n);
    }
    out["meshes"] = ordered_json::array();
    for (const auto& mesh : model.meshes) {
        std::size_t vertices = 0, triangles = 0;
        for (const auto& primitive : mesh.primitives) {
            vertices += primitive.positions.size();
            triangles += primitive.indices.size() / 3;
        }
        out["meshes"].push_back({{"name", mesh.name},
                                 {"primitives", mesh.primitives.size()},
                                 {"vertices", vertices},
                                 {"triangles", triangles}});
    }
    out["skins"] = ordered_json::array();
    for (const auto& skin : model.skins) {
        ordered_json joints = ordered_json::array();
        for (int j : skin.joints) joints.push_back(model.nodes[static_cast<std::size_t>(j)].name);
        out["skins"].push_back({{"name", skin.name}, {"joints", joints}});
    }
    out["animations"] = ordered_json::array();
    for (const auto& animation : model.animations) {
        const auto closure = pose_atlas::loop_closure(model, animation, animation.start_s, animation.end_s);
        out["animations"].push_back({{"name", animation.name},
                                     {"start_s", animation.start_s},
                                     {"end_s", animation.end_s},
                                     {"duration_s", animation.duration_s()},
                                     {"channels", animation.channels.size()},
                                     {"ignored_channels", animation.ignored_channels},
                                     {"loop_closure",
                                      {{"max_position_delta_m", closure.max_position_delta_m},
                                       {"max_rotation_delta_degrees", closure.max_rotation_delta_degrees}}}});
    }
    std::cout << out.dump(2) << "\n";
    return 0;
}

int plan_command(const Args& args) {
    if (args.size() < 2) throw UsageError("plan: expected 'plan <model.glb> --action NAME --poses N'");
    pose_atlas::SamplingSpec spec;
    bool have_action = false, have_poses = false, have_rate = false, have_frame = false, have_cycle = false,
         have_speed = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string& option = args[i];
        if (option == "--include-end") {
            spec.endpoint = pose_atlas::EndPoint::Include;
            continue;
        }
        if (i + 1 >= args.size()) throw UsageError(option + ": missing value");
        const std::string& value = args[++i];
        if (option == "--action") {
            spec.action = value;
            have_action = true;
        } else if (option == "--poses") {
            const double n = parse_number(option, value);
            if (n != std::floor(n) || n < 1 || n > 100000) throw UsageError("--poses: expected a positive integer");
            spec.pose_count = static_cast<int>(n);
            have_poses = true;
        } else if (option == "--start") {
            spec.start_s = parse_number(option, value);
        } else if (option == "--end") {
            spec.end_s = parse_number(option, value);
        } else if (option == "--rate") {
            spec.cadence.playback_rate = parse_number(option, value);
            have_rate = true;
        } else if (option == "--frame-ms") {
            spec.cadence.kind = pose_atlas::Cadence::Kind::FixedFrame;
            spec.cadence.frame_ms = parse_number(option, value);
            have_frame = true;
        } else if (option == "--cycle-m") {
            spec.cadence.kind = pose_atlas::Cadence::Kind::Distance;
            spec.cadence.cycle_length_m = parse_number(option, value);
            have_cycle = true;
        } else if (option == "--speed-mps") {
            spec.cadence.reference_speed_mps = parse_number(option, value);
            have_speed = true;
        } else {
            throw UsageError("plan: unknown option '" + option + "'");
        }
    }
    if (!have_action || !have_poses) throw UsageError("plan: --action and --poses are required");
    if (int(have_rate) + int(have_frame) + int(have_cycle) > 1) {
        throw UsageError("plan: choose one cadence: --rate, --frame-ms or --cycle-m");
    }
    if (have_speed && !have_cycle) throw UsageError("plan: --speed-mps needs --cycle-m");
    const pose_atlas::Model model = pose_atlas::load_gltf(args[1]);
    std::cout << pose_atlas::plan_to_json(pose_atlas::plan_sampling(model, spec));
    return 0;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> out;
    std::string item;
    for (char c : text) {
        if (c == separator) {
            out.push_back(item);
            item.clear();
        } else {
            item += c;
        }
    }
    out.push_back(item);
    return out;
}

int parse_count(const std::string& option, const std::string& text) {
    const double n = parse_number(option, text);
    if (n != std::floor(n) || n < 1 || n > 4096) throw UsageError(option + ": expected a pose count from 1 to 4096");
    return static_cast<int>(n);
}

int render_command(const Args& args) {
    if (args.size() < 2) throw UsageError("render: expected 'render <model.glb> --profile FILE --actions A,B --out DIR'");
    pose_atlas::AtlasJob job;
    job.model_path = args[1];
    std::string profile_path, actions, out, cache, poses = "8";
    std::string palette_stage, alpha_threshold;
    bool no_cache = false, have_rate = false, have_frame = false, discard_corrections = false;
    pose_atlas::Cadence cadence;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string& option = args[i];
        if (option == "--outline") {
            job.settings.style.outline = true;
            continue;
        }
        if (option == "--no-cache") {
            no_cache = true;
            continue;
        }
        if (option == "--discard-corrections") {
            discard_corrections = true;
            continue;
        }
        if (i + 1 >= args.size()) throw UsageError(option + ": missing value");
        const std::string& value = args[++i];
        if (option == "--profile") {
            profile_path = value;
        } else if (option == "--actions") {
            actions = value;
        } else if (option == "--out") {
            out = value;
        } else if (option == "--poses") {
            poses = value;
        } else if (option == "--rate") {
            cadence.playback_rate = parse_number(option, value);
            have_rate = true;
        } else if (option == "--frame-ms") {
            cadence.kind = pose_atlas::Cadence::Kind::FixedFrame;
            cadence.frame_ms = parse_number(option, value);
            have_frame = true;
        } else if (option == "--texture-filter") {
            try {
                job.settings.texture_filter = pose_atlas::parse_texture_filter(value);
            } catch (const std::invalid_argument& e) {
                throw UsageError(std::string("--texture-filter: ") + e.what());
            }
        } else if (option == "--palette") {
            job.palette_path = value;
        } else if (option == "--palette-stage") {
            palette_stage = value;
        } else if (option == "--alpha-threshold") {
            alpha_threshold = value;
        } else if (option == "--erase-color") {
            if (!pose_atlas::parse_hex_colour(value, job.erase_colour)) {
                throw UsageError("--erase-color: expected #rrggbb, got '" + value + "'");
            }
        } else if (option == "--hide-material") {
            job.settings.hidden_materials.push_back(value);
        } else if (option == "--cache") {
            cache = value;
        } else {
            throw UsageError("render: unknown option '" + option + "'");
        }
    }
    if (profile_path.empty() || actions.empty() || out.empty()) {
        throw UsageError("render: --profile, --actions and --out are required");
    }
    if (have_rate && have_frame) throw UsageError("render: choose one cadence: --rate or --frame-ms");
    if (no_cache && !cache.empty()) throw UsageError("render: --cache and --no-cache exclude each other");
    if (job.palette_path.empty() && (!palette_stage.empty() || !alpha_threshold.empty())) {
        throw UsageError("render: --palette-stage and --alpha-threshold need --palette");
    }
    if (!job.palette_path.empty()) {
        try {
            job.settings.palette_stage = pose_atlas::parse_palette_stage(palette_stage.empty() ? "before_after" : palette_stage);
        } catch (const std::invalid_argument& e) {
            throw UsageError(std::string("--palette-stage: ") + e.what());
        }
        if (job.settings.palette_stage == pose_atlas::PaletteStage::None) {
            throw UsageError("--palette-stage: none would ignore the palette; leave out --palette instead");
        }
        if (!alpha_threshold.empty()) {
            job.settings.alpha_threshold = parse_number("--alpha-threshold", alpha_threshold);
            if (!(job.settings.alpha_threshold > 0.0 && job.settings.alpha_threshold <= 1.0)) {
                throw UsageError("--alpha-threshold: expected a coverage in (0, 1]");
            }
        }
    }

    // Pose counts: one number for every action, or action=count pairs.
    std::map<std::string, int> counts;
    int default_count = 8;
    if (poses.find('=') == std::string::npos) {
        default_count = parse_count("--poses", poses);
    } else {
        for (const std::string& pair : split(poses, ',')) {
            const auto eq = pair.find('=');
            if (eq == std::string::npos) throw UsageError("--poses: expected N or ACTION=N,...");
            counts[pair.substr(0, eq)] = parse_count("--poses", pair.substr(eq + 1));
        }
    }
    for (const std::string& name : split(actions, ',')) {
        if (name.empty()) throw UsageError("--actions: empty action name");
        pose_atlas::SamplingSpec spec;
        spec.action = name;
        const auto it = counts.find(name);
        spec.pose_count = it != counts.end() ? it->second : default_count;
        if (it != counts.end()) counts.erase(it);
        spec.cadence = cadence;
        job.actions.push_back(spec);
    }
    if (!counts.empty()) throw UsageError("--poses: '" + counts.begin()->first + "' is not in --actions");

    job.profile = pose_atlas::load_profile(profile_path);
    if (!job.palette_path.empty()) job.settings.palette = pose_atlas::load_palette(job.palette_path).colours;
    for (const auto& colour : job.settings.palette) {
        if (colour == job.erase_colour) {
            std::cerr << "pose-atlas: warning: the erase colour " << pose_atlas::hex_colour(colour)
                      << " is also a palette colour; corrections cannot paint it (choose another with "
                         "--erase-color)\n";
        }
    }
    std::filesystem::create_directories(out);
    if (!no_cache && cache.empty()) cache = (std::filesystem::path(out) / "cache").string();

    // Manual corrections in an existing atlas.aseprite: backed up and read
    // before anything is rendered or overwritten.
    const pose_atlas::ExistingCorrections existing = pose_atlas::read_existing_corrections(out, discard_corrections);

    pose_atlas::Renderer renderer;
    pose_atlas::Atlas atlas = pose_atlas::render_atlas(renderer, job, no_cache ? "" : cache);
    const pose_atlas::CorrectionsReport corrections = pose_atlas::carry_corrections(existing, atlas);
    int reused = 0;
    std::string line;
    for (const auto& r : atlas.renders) {
        reused += r.reused ? 1 : 0;
        if (line.empty() || line.rfind(r.action + ":", 0) != 0) {
            if (!line.empty()) std::cout << line << "\n";
            line = r.action + ":";
        }
        line += " " + r.view + (r.reused ? "=reused" : "=rendered");
    }
    std::cout << line << "\n";
    std::cout << "renders: " << atlas.renders.size() - static_cast<std::size_t>(reused) << " rendered, " << reused
              << " reused from " << (no_cache ? std::string("(no cache)") : cache) << "\n";
    if (pose_atlas::palette_active(atlas.settings)) std::cout << pose_atlas::palette_summary(atlas) << "\n";
    for (const std::string& file : pose_atlas::export_atlas(atlas, out)) {
        std::cout << "wrote " << (std::filesystem::path(out) / file).string() << "\n";
    }
    const std::string report_path = (std::filesystem::path(out) / pose_atlas::kCorrectionsReport).string();
    pose_atlas::write_text_file(report_path, corrections.to_json());
    for (const auto& orphan : corrections.orphaned) {
        std::cout << "orphaned correction: " << orphan.key << " (old frame " << orphan.old_frame << "): "
                  << orphan.reason << "\n";
    }
    std::cout << corrections.summary() << "; report in " << report_path << "\n";
    const pose_atlas::ValidationReport report = pose_atlas::validate_atlas(out);
    for (const auto& problem : report.problems) std::cerr << "validate: " << problem << "\n";
    if (!report.passed()) {
        std::cerr << "pose-atlas: the export did not pass validation\n";
        return 1;
    }
    std::cout << "validated " << report.frames.size() << " frames (" << atlas.columns << " poses x 8 views) in "
              << out << "\n";
    return 0;
}

int validate_command(const Args& args) {
    if (args.size() != 2 && !(args.size() == 4 && args[2] == "--report")) {
        throw UsageError("validate: expected 'validate <dir> [--report FILE]'");
    }
    const pose_atlas::ValidationReport report = pose_atlas::validate_atlas(args[1]);
    if (args.size() == 4) {
        std::ofstream file(args[3], std::ios::binary | std::ios::trunc);
        file << report.to_json();
        if (!file) throw std::runtime_error(args[3] + ": cannot write the report");
    }
    for (const auto& problem : report.problems) std::cout << args[1] << ": " << problem << "\n";
    if (!report.passed()) {
        std::cout << "POSE_VALIDATION_FAIL " << report.problems.size() << " problems\n";
        return 1;
    }
    std::cout << "POSE_VALIDATION_PASS " << report.frames.size() << " frames in " << args[1] << "\n";
    return 0;
}

int run(const Args& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        std::cout << kUsage;
        return args.empty() ? 2 : 0;
    }
    const std::string& command = args[0];
    if (command == "version" || command == "--version") {
        std::cout << "pose-atlas " << pose_atlas::version() << "\n";
        return 0;
    }
    if (command == "profile") return profile_command(args);
    if (command == "inspect") return inspect_command(args);
    if (command == "plan") return plan_command(args);
    if (command == "render") return render_command(args);
    if (command == "validate") return validate_command(args);
    if (command == "make-test-rig") {
        if (args.size() != 2) throw UsageError("make-test-rig: expected 'make-test-rig <out.glb>'");
        const std::filesystem::path parent = std::filesystem::path(args[1]).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        pose_atlas::test_rig::write_glb(args[1]);
        std::cout << "wrote " << args[1] << "\n";
        return 0;
    }
    throw UsageError("unknown command '" + command + "'");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(Args(argv + 1, argv + argc));
    } catch (const UsageError& error) {
        std::cerr << "pose-atlas: " << error.what() << "\n" << kUsage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "pose-atlas: " << error.what() << "\n";
        return 1;
    }
}
