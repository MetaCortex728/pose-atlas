// SPDX-License-Identifier: Apache-2.0
// Animation sampling, node hierarchy evaluation and linear-blend skinning.

#include <algorithm>
#include <cmath>

#include "pose_atlas/model.hpp"

namespace pose_atlas {

namespace {

struct LocalTrs {
    Vec3 translation;
    Quat rotation;
    Vec3 scale{1.0, 1.0, 1.0};
};

// Value of a channel at time t: `out` receives `components` doubles.
void sample_channel(const Channel& channel, double t, std::size_t components, double* out) {
    const std::vector<double>& times = channel.times;
    const bool cubic = channel.interpolation == Interpolation::CubicSpline;
    const std::size_t stride = cubic ? 3 * components : components;
    const std::size_t value_offset = cubic ? components : 0;  // skip the in-tangent
    auto key_value = [&](std::size_t key) { return channel.values.data() + key * stride + value_offset; };

    if (t <= times.front() || times.size() == 1) {
        std::copy(key_value(0), key_value(0) + components, out);
        return;
    }
    if (t >= times.back()) {
        const std::size_t last = times.size() - 1;
        std::copy(key_value(last), key_value(last) + components, out);
        return;
    }
    // times[k0] <= t < times[k0 + 1]
    const auto upper = std::upper_bound(times.begin(), times.end(), t);
    const auto k1 = static_cast<std::size_t>(upper - times.begin());
    const std::size_t k0 = k1 - 1;
    const double dt = times[k1] - times[k0];
    const double s = (t - times[k0]) / dt;
    const double* v0 = key_value(k0);
    const double* v1 = key_value(k1);

    switch (channel.interpolation) {
        case Interpolation::Step: std::copy(v0, v0 + components, out); return;
        case Interpolation::Linear:
            if (channel.path == ChannelPath::Rotation) {
                const Quat q = slerp({v0[0], v0[1], v0[2], v0[3]}, {v1[0], v1[1], v1[2], v1[3]}, s);
                out[0] = q.x;
                out[1] = q.y;
                out[2] = q.z;
                out[3] = q.w;
            } else {
                for (std::size_t i = 0; i < components; ++i) out[i] = v0[i] + (v1[i] - v0[i]) * s;
            }
            return;
        case Interpolation::CubicSpline: {
            // glTF Appendix C: Hermite spline with tangents scaled by dt.
            const double* out_tangent0 = channel.values.data() + k0 * stride + 2 * components;
            const double* in_tangent1 = channel.values.data() + k1 * stride;
            const double s2 = s * s;
            const double s3 = s2 * s;
            const double h00 = 2 * s3 - 3 * s2 + 1;
            const double h10 = s3 - 2 * s2 + s;
            const double h01 = -2 * s3 + 3 * s2;
            const double h11 = s3 - s2;
            for (std::size_t i = 0; i < components; ++i) {
                out[i] = h00 * v0[i] + h10 * dt * out_tangent0[i] + h01 * v1[i] + h11 * dt * in_tangent1[i];
            }
            if (channel.path == ChannelPath::Rotation) {
                const Quat q = normalized({out[0], out[1], out[2], out[3]});
                out[0] = q.x;
                out[1] = q.y;
                out[2] = q.z;
                out[3] = q.w;
            }
            return;
        }
    }
}

double wrap_time(const Animation& animation, double t, TimeMode mode) {
    if (mode == TimeMode::Clamp || (t >= animation.start_s && t <= animation.end_s)) return t;
    const double duration = animation.duration_s();
    if (!(duration > 0.0)) return animation.start_s;
    double phase = std::fmod(t - animation.start_s, duration);
    if (phase < 0.0) phase += duration;
    return animation.start_s + phase;
}

}  // namespace

Pose evaluate(const Model& model, const Animation* animation, double time_s, TimeMode mode) {
    const std::size_t count = model.nodes.size();
    std::vector<LocalTrs> trs(count);
    for (std::size_t i = 0; i < count; ++i) {
        trs[i] = {model.nodes[i].translation, model.nodes[i].rotation, model.nodes[i].scale};
    }
    std::vector<bool> animated(count, false);
    if (animation != nullptr) {
        const double t = wrap_time(*animation, time_s, mode);
        for (const Channel& channel : animation->channels) {
            const auto node = static_cast<std::size_t>(channel.node);
            double v[4] = {0, 0, 0, 0};
            switch (channel.path) {
                case ChannelPath::Translation:
                    sample_channel(channel, t, 3, v);
                    trs[node].translation = {v[0], v[1], v[2]};
                    break;
                case ChannelPath::Rotation:
                    sample_channel(channel, t, 4, v);
                    trs[node].rotation = normalized({v[0], v[1], v[2], v[3]});
                    break;
                case ChannelPath::Scale:
                    sample_channel(channel, t, 3, v);
                    trs[node].scale = {v[0], v[1], v[2]};
                    break;
            }
            animated[node] = true;
        }
    }

    Pose pose;
    pose.local.resize(count);
    pose.global.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Node& node = model.nodes[i];
        pose.local[i] = (node.has_matrix && !animated[i]) ? node.matrix
                                                          : compose(trs[i].translation, trs[i].rotation, trs[i].scale);
    }
    // Parents before children: walk down from each root.
    std::vector<std::size_t> stack;
    for (std::size_t i = 0; i < count; ++i) {
        if (model.nodes[i].parent < 0) stack.push_back(i);
    }
    while (!stack.empty()) {
        const std::size_t i = stack.back();
        stack.pop_back();
        const int parent = model.nodes[i].parent;
        pose.global[i] = parent < 0 ? pose.local[i] : pose.global[static_cast<std::size_t>(parent)] * pose.local[i];
        for (int child : model.nodes[i].children) stack.push_back(static_cast<std::size_t>(child));
    }
    return pose;
}

std::vector<Mat4> joint_matrices(const Model& model, int skin_index, const Pose& pose) {
    const Skin& skin = model.skins.at(static_cast<std::size_t>(skin_index));
    std::vector<Mat4> matrices;
    matrices.reserve(skin.joints.size());
    for (std::size_t j = 0; j < skin.joints.size(); ++j) {
        matrices.push_back(pose.global[static_cast<std::size_t>(skin.joints[j])] * skin.inverse_bind[j]);
    }
    return matrices;
}

PosedPrimitive pose_primitive(const Model& model, int node_index, int primitive_index, const Pose& pose) {
    const Node& node = model.nodes.at(static_cast<std::size_t>(node_index));
    if (node.mesh < 0) throw ModelError(model.source + ": node \"" + node.name + "\" has no mesh");
    const Primitive& primitive =
        model.meshes[static_cast<std::size_t>(node.mesh)].primitives.at(static_cast<std::size_t>(primitive_index));
    PosedPrimitive out;
    out.positions.reserve(primitive.positions.size());
    out.normals.reserve(primitive.normals.size());

    if (node.skin < 0 || primitive.joints.empty()) {
        const Mat4& m = pose.global[static_cast<std::size_t>(node_index)];
        for (const Vec3& p : primitive.positions) out.positions.push_back(transform_point(m, p));
        for (const Vec3& n : primitive.normals) {
            const Vec3 d = transform_direction(m, n);
            out.normals.push_back(d * (1.0 / norm(d)));
        }
        return out;
    }
    // glTF: the skinned mesh node's own transform is ignored; joint matrices
    // carry the vertices into model space.
    const std::vector<Mat4> joints = joint_matrices(model, node.skin, pose);
    for (std::size_t v = 0; v < primitive.positions.size(); ++v) {
        Mat4 blend;
        blend.m.fill(0.0);
        double total = 0.0;
        for (std::size_t k = 0; k < 4; ++k) {
            const double w = primitive.weights[v][k];
            if (w == 0.0) continue;
            const Mat4& j = joints[primitive.joints[v][k]];
            for (std::size_t e = 0; e < 16; ++e) blend.m[e] += w * j.m[e];
            total += w;
        }
        if (total == 0.0) blend = Mat4{};
        out.positions.push_back(transform_point(blend, primitive.positions[v]));
        if (!primitive.normals.empty()) {
            const Vec3 d = transform_direction(blend, primitive.normals[v]);
            const double n = norm(d);
            out.normals.push_back(n > 0.0 ? d * (1.0 / n) : d);
        }
    }
    return out;
}

int Model::find_node(const std::string& name) const {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int Model::find_animation(const std::string& name) const {
    for (std::size_t i = 0; i < animations.size(); ++i) {
        if (animations[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

const Animation& Model::animation(const std::string& name) const {
    const int index = find_animation(name);
    if (index >= 0) return animations[static_cast<std::size_t>(index)];
    std::string available;
    for (const Animation& a : animations) available += (available.empty() ? "" : ", ") + ("\"" + a.name + "\"");
    throw ModelError(source + ": no animation \"" + name + "\" (available: " +
                     (available.empty() ? "none" : available) + ")");
}

}  // namespace pose_atlas
