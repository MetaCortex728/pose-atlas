// SPDX-License-Identifier: Apache-2.0
#include "pose_atlas/camera.hpp"

#include <cmath>
#include <stdexcept>

namespace pose_atlas {

namespace {

Vec3 rotate_z(const Vec3& v, double angle) {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return {c * v.x - s * v.y, s * v.x + c * v.y, v.z};
}

}  // namespace

AxisFrame axis_frame(const Profile& profile) {
    const Vec3 y = -axis_vector(profile.character_forward);
    const Vec3 z = axis_vector(profile.world_up);
    // Right-handed: canonical X = Y x Z.
    return {cross(y, z), y, z};
}

int heading_index(const std::string& direction) {
    for (std::size_t i = 0; i < kScreenHeadings.size(); ++i) {
        if (direction == kScreenHeadings[i]) return static_cast<int>(i);
    }
    throw std::invalid_argument("unknown screen heading \"" + direction +
                                "\" (expected e, ne, n, nw, w, sw, s or se)");
}

CameraGuide camera_guide(const Profile& profile, const std::string& direction) {
    return camera_guide(profile, heading_index(direction));
}

CameraGuide camera_guide(const Profile& profile, int index) {
    if (index < 0 || index >= static_cast<int>(kScreenHeadings.size())) {
        throw std::invalid_argument("heading index must be 0..7, got " + std::to_string(index));
    }
    // Same operations in the same order as the prototype, in the canonical
    // frame (forward -Y, up +Z).
    const double pitch = radians(profile.elevation_degrees);
    const double az = radians(profile.base_azimuth_degrees);
    const double cp = std::cos(pitch);
    const double sp = std::sin(pitch);
    const double ca = std::cos(az);
    const double sa = std::sin(az);
    Vec3 toward{cp * ca, cp * sa, sp};
    Vec3 right{-sa, ca, 0.0};
    Vec3 up{-sp * ca, -sp * sa, cp};
    const double angle = index * kPi / 4;
    // Invert the ground-plane foreshortening before choosing the model heading.
    const double hx = right.x * std::cos(angle) + up.x * std::sin(angle) / (sp * sp);
    const double hy = right.y * std::cos(angle) + up.y * std::sin(angle) / (sp * sp);
    const double rotation = -(std::atan2(hy, hx) + kPi / 2);
    toward = rotate_z(toward, rotation);
    right = rotate_z(right, rotation);
    up = rotate_z(up, rotation);
    const double cell = profile.cell_pixels;
    const double mpp = profile.meters_per_pixel;
    const double px = profile.ground_pivot_pixels[0];
    const double py = profile.ground_pivot_pixels[1];
    const double offset_x = (cell / 2 - px) * mpp;
    const double offset_y = (py - cell / 2) * mpp;
    Vec3 position;
    position.x = toward.x * profile.camera_distance_m + right.x * offset_x + up.x * offset_y;
    position.y = toward.y * profile.camera_distance_m + right.y * offset_x + up.y * offset_y;
    position.z = toward.z * profile.camera_distance_m + right.z * offset_x + up.z * offset_y;

    const AxisFrame frame = axis_frame(profile);
    CameraGuide guide;
    guide.direction = kScreenHeadings[static_cast<std::size_t>(index)];
    guide.heading_index = index;
    guide.toward = frame.to_model(toward);
    guide.right = frame.to_model(right);
    guide.up = frame.to_model(up);
    guide.position = frame.to_model(position);
    guide.ortho_scale_m = cell * mpp;
    guide.camera_rotation_degrees = degrees(rotation);
    guide.vertical_projection_factor = cp;
    guide.ground_depth_factor = sp;
    return guide;
}

Vec2 project(const Profile& profile, const CameraGuide& guide, const Vec3& point) {
    const double px = profile.ground_pivot_pixels[0];
    const double py = profile.ground_pivot_pixels[1];
    const double mpp = profile.meters_per_pixel;
    return {px + dot(point, guide.right) / mpp, py - dot(point, guide.up) / mpp};
}

Vec2 projected_forward(const Profile& profile, const CameraGuide& guide) {
    const Vec3 forward = axis_vector(profile.character_forward);
    return {dot(forward, guide.right), dot(forward, guide.up)};
}

}  // namespace pose_atlas
