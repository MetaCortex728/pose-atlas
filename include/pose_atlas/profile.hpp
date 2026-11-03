// SPDX-License-Identifier: Apache-2.0
// The versioned camera profile: one orthographic camera, eight screen
// headings, the sprite cell and its physical scale, and the resampling and
// zoom settings every output of a run shares.
#pragma once

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#include "pose_atlas/math.hpp"

namespace pose_atlas {

/// Format name and the only profile version this build reads.
inline constexpr const char* kProfileFormat = "pose-atlas-profile";
inline constexpr int kProfileVersion = 1;

/// Screen headings in order; heading i points i * 45 degrees counter-clockwise
/// from screen right ("e").
inline constexpr std::array<const char*, 8> kScreenHeadings = {"e", "ne", "n", "nw",
                                                               "w", "sw", "s", "se"};

/// A signed coordinate axis of the model, written "+X", "-Y" and so on.
enum class Axis { PosX, NegX, PosY, NegY, PosZ, NegZ };

/// Filter used when downsampling the supersampled render to the cell.
enum class ResampleFilter { Box, Triangle, Lanczos3 };

struct Profile {
    std::string id;
    std::string description;
    // Camera. Only orthographic projection is supported.
    double elevation_degrees = 30.0;
    double base_azimuth_degrees = 45.0;
    double camera_distance_m = 8.0;
    // Model axes: the direction the character faces in its rest pose and the
    // world up direction, both in the model's own coordinates.
    Axis character_forward = Axis::NegY;
    Axis world_up = Axis::PosZ;
    // Cell and scale. Pixel coordinates have Y pointing down.
    int cell_pixels = 256;
    std::array<int, 2> ground_pivot_pixels = {128, 208};
    double meters_per_pixel = 0.0;
    // Rendering and output.
    int supersampling = 4;
    ResampleFilter resampling_filter = ResampleFilter::Box;
    std::vector<int> zoom_set = {1};

    /// Side of the cell in metres (the orthographic scale).
    double cell_span_m() const { return cell_pixels * meters_per_pixel; }
};

/// Raised for unreadable or invalid profiles. what() lists every problem,
/// one per line, each prefixed by the source name and the field.
class ProfileError : public std::runtime_error {
public:
    ProfileError(const std::string& source, std::vector<std::string> problems);
    const std::vector<std::string>& problems() const { return problems_; }

private:
    std::vector<std::string> problems_;
};

/// Parse and validate a profile from JSON text. `source` names it in errors.
Profile parse_profile(const std::string& json_text, const std::string& source = "profile");

/// Read, parse and validate a profile file.
Profile load_profile(const std::string& path);

/// Problems with a profile's values; empty when it is valid.
std::vector<std::string> validate_profile(const Profile& profile);

/// Profile as JSON text in the file format (round-trips through parse_profile).
std::string profile_to_json(const Profile& profile);

std::string to_string(Axis axis);
std::string to_string(ResampleFilter filter);
/// Unit vector of an axis.
Vec3 axis_vector(Axis axis);

}  // namespace pose_atlas
