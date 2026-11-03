// SPDX-License-Identifier: Apache-2.0
// pose-atlas command-line tool.

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pose_atlas/camera.hpp"
#include "pose_atlas/profile.hpp"
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
