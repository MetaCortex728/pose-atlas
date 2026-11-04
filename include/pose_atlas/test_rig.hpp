// SPDX-License-Identifier: Apache-2.0
// Procedurally generated test character: a small, deliberately asymmetric
// articulated figure made of boxes, with a skin and two animations, written
// as a glTF 2.0 binary (GLB). It exists so that tests and CI exercise the
// whole pipeline without any third-party or game assets.
//
// glTF conventions: metres, +Y up, the figure faces +Z, its left is +X.
// The model origin is on the ground between the feet, marked by a flat pivot
// marker with a tick pointing forward.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "pose_atlas/image.hpp"
#include "pose_atlas/math.hpp"

namespace pose_atlas::test_rig {

/// Rest pose: joints carry translations only (no rest rotation or scale).
struct Joint {
    std::string name;
    int parent = -1;   // index into Description::joints, -1 for the root
    Vec3 translation;  // relative to the parent
};

/// An axis-aligned box in the local frame of `joint`, rigidly bound to it.
/// When `blend_joint` is set, the vertices on the box's lowest face are
/// shared half and half with that joint instead.
struct Part {
    std::string name;
    int joint = 0;
    Vec3 min;
    Vec3 max;
    std::array<float, 3> colour{};  // linear RGB base colour factor
    int blend_joint = -1;
    /// Index into Description::textures for the base colour texture, or -1.
    /// Each face of a textured box maps the whole texture. For a face normal
    /// to axis k, u runs from 0 to 1 along axis (k + 1) mod 3 and v from 1 to
    /// 0 along axis (k + 2) mod 3, so on the +Z face u = 0 is at -X (the
    /// figure's right) and v = 0 at the top.
    int texture = -1;
};

/// Rotation about a fixed axis through the joint, linear in angle between
/// keys (what glTF's spherical interpolation gives for a single axis).
struct RotationTrack {
    int joint = 0;
    Vec3 axis;
    std::vector<double> times;
    std::vector<double> angles_degrees;
};

struct TranslationTrack {
    int joint = 0;
    std::vector<double> times;
    std::vector<Vec3> values;
};

struct Animation {
    std::string name;
    double duration = 0.0;  // the last key time of every track
    std::vector<RotationTrack> rotations;
    std::vector<TranslationTrack> translations;
};

struct Description {
    std::vector<Joint> joints;
    std::vector<Part> parts;
    /// Generated base colour textures (sRGB, opaque), embedded as PNG and
    /// sampled with nearest filtering and clamp-to-edge wrapping.
    std::vector<Image> textures;
    std::vector<Animation> animations;
};

/// The figure. Animations: "idle" (2 s) and "run" (0.8 s), both loops whose
/// last key equals the first. The torso carries a 2x2 checker texture.
const Description& description();

/// The figure's rendered colours as 8-bit sRGB, in part order without
/// repeats: each untextured part's base colour, and for textured parts each
/// texel times the part's colour factor. palettes/test-rig.json holds
/// the same colours.
std::vector<std::array<std::uint8_t, 3>> palette();

/// The figure as GLB bytes. Deterministic: the same bytes on every call and
/// build.
std::vector<std::uint8_t> make_glb();

/// Write make_glb() to a file; throws std::runtime_error on I/O failure.
void write_glb(const std::string& path);

}  // namespace pose_atlas::test_rig
