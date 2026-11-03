// SPDX-License-Identifier: Apache-2.0
// pose-atlas command-line tool.

#include <cmath>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pose_atlas/camera.hpp"
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
    if (command == "make-test-rig") {
        if (args.size() != 2) throw UsageError("make-test-rig: expected 'make-test-rig <out.glb>'");
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
