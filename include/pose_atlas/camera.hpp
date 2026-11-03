// SPDX-License-Identifier: Apache-2.0
// Camera guides: where the orthographic camera sits for each of the eight
// screen headings, and how model points project into the cell.
//
// Port of the prototype's camera_guide and project
// (prototype/dan_projection.py). The model never rotates; the camera orbits
// the world up axis so that the character's forward axis appears on screen
// along the requested heading. The heading is chosen on screen, so the
// ground-plane foreshortening of the elevated camera is inverted first.
#pragma once

#include <string>

#include "pose_atlas/math.hpp"
#include "pose_atlas/profile.hpp"

namespace pose_atlas {

/// Rotation from the canonical frame (forward -Y, up +Z, the frame the
/// prototype's maths is written in) to the profile's model frame.
struct AxisFrame {
    Vec3 x;  // canonical +X in model coordinates
    Vec3 y;  // canonical +Y in model coordinates (= -character_forward)
    Vec3 z;  // canonical +Z in model coordinates (= world_up)

    Vec3 to_model(const Vec3& canonical) const {
        return x * canonical.x + y * canonical.y + z * canonical.z;
    }
    Vec3 to_canonical(const Vec3& model) const { return {dot(model, x), dot(model, y), dot(model, z)}; }
};

AxisFrame axis_frame(const Profile& profile);

/// Camera placement for one screen heading. Vectors and the position are in
/// the model frame; `toward` points from the target to the camera.
struct CameraGuide {
    std::string direction;  // "e", "ne", ...
    int heading_index = 0;  // 0..7, heading angle = index * 45 degrees
    Vec3 toward;
    Vec3 right;
    Vec3 up;
    Vec3 position;
    double ortho_scale_m = 0.0;
    /// Camera rotation about world up, relative to the base azimuth.
    double camera_rotation_degrees = 0.0;
    double vertical_projection_factor = 0.0;  // cos(elevation)
    double ground_depth_factor = 0.0;         // sin(elevation)
};

/// Index of a heading name; throws std::invalid_argument for unknown names.
int heading_index(const std::string& direction);

CameraGuide camera_guide(const Profile& profile, int heading_index);
CameraGuide camera_guide(const Profile& profile, const std::string& direction);

/// Pixel coordinates of a model-frame point in the cell, Y pointing down.
/// The model origin lands on the ground pivot.
Vec2 project(const Profile& profile, const CameraGuide& guide, const Vec3& point);

/// Screen projection (X right, Y up, in metres per metre) of the character's
/// unit forward axis. Its direction is (cos, sin) of the heading angle; its
/// length is below 1 where the ground plane is foreshortened.
Vec2 projected_forward(const Profile& profile, const CameraGuide& guide);

}  // namespace pose_atlas
