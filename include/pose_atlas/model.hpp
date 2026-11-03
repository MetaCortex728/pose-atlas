// SPDX-License-Identifier: Apache-2.0
// A rigged, animated character loaded from glTF 2.0 (.gltf or .glb), and its
// evaluation at any source animation time.
#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "pose_atlas/math.hpp"

namespace pose_atlas {

/// Triangle list in the mesh's own coordinates (the bind pose for skinned
/// meshes). joints/weights are empty for meshes without a skin.
struct Primitive {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;  // empty when the file has none
    std::vector<std::array<std::uint16_t, 4>> joints;
    std::vector<std::array<float, 4>> weights;
    std::vector<std::uint32_t> indices;  // three per triangle
    std::string material;
    std::array<float, 4> base_colour = {1.0f, 1.0f, 1.0f, 1.0f};  // linear RGBA
};

struct Mesh {
    std::string name;
    std::vector<Primitive> primitives;
};

struct Node {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    // Rest (non-animated) local transform: a matrix, or translation, rotation
    // and scale.
    bool has_matrix = false;
    Mat4 matrix;
    Vec3 translation;
    Quat rotation;
    Vec3 scale{1.0, 1.0, 1.0};
    int mesh = -1;
    int skin = -1;
};

struct Skin {
    std::string name;
    std::vector<int> joints;          // node indices
    std::vector<Mat4> inverse_bind;   // one per joint
    int skeleton = -1;                // node index, or -1
};

enum class Interpolation { Linear, Step, CubicSpline };
enum class ChannelPath { Translation, Rotation, Scale };

struct Channel {
    int node = 0;
    ChannelPath path = ChannelPath::Translation;
    Interpolation interpolation = Interpolation::Linear;
    std::vector<double> times;   // strictly increasing, seconds
    // Per key: 3 (translation, scale) or 4 (rotation x, y, z, w) values; for
    // CUBICSPLINE three such groups per key: in-tangent, value, out-tangent.
    std::vector<double> values;
};

/// One action. The source clock runs from `start_s` to `end_s`: the first
/// and last key over all channels.
struct Animation {
    std::string name;
    std::vector<Channel> channels;
    double start_s = 0.0;
    double end_s = 0.0;
    int ignored_channels = 0;  // morph-target weight channels (not supported)

    double duration_s() const { return end_s - start_s; }
};

struct Model {
    std::string source;
    std::vector<Node> nodes;
    std::vector<Mesh> meshes;
    std::vector<Skin> skins;
    std::vector<Animation> animations;

    /// Index of a node or animation by name; -1 when absent.
    int find_node(const std::string& name) const;
    int find_animation(const std::string& name) const;
    /// Like find_animation, but throws ModelError naming the available ones.
    const Animation& animation(const std::string& name) const;
};

class ModelError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Load a .gltf (with external or embedded buffers) or .glb file.
Model load_gltf(const std::string& path);
/// Load from GLB or self-contained glTF bytes; `source` names it in errors.
Model load_gltf(const std::vector<std::uint8_t>& bytes, const std::string& source);

/// What to do with source times outside the animation's range.
enum class TimeMode {
    Clamp,  // hold the first or last key
    Loop,   // wrap into [start, end]; times inside the range are used as is
};

/// Node transforms at one instant.
struct Pose {
    std::vector<Mat4> local;
    std::vector<Mat4> global;  // model space
};

/// Pose of every node at source time `time_s` (seconds on the animation's own
/// clock). With `animation == nullptr`, the rest pose.
Pose evaluate(const Model& model, const Animation* animation, double time_s,
              TimeMode mode = TimeMode::Clamp);

/// Skinning matrices of a skin: global(joint) * inverse_bind(joint).
std::vector<Mat4> joint_matrices(const Model& model, int skin, const Pose& pose);

/// Positions and normals of a primitive of the mesh on `node` in model space:
/// linear-blend skinned when the node has a skin, otherwise moved by the
/// node's global transform.
struct PosedPrimitive {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
};
PosedPrimitive pose_primitive(const Model& model, int node, int primitive, const Pose& pose);

}  // namespace pose_atlas
