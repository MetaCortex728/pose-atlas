// SPDX-License-Identifier: Apache-2.0
#include "pose_atlas/test_rig.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "pose_atlas/resample.hpp"
#include "pose_atlas/version.hpp"

namespace pose_atlas::test_rig {

namespace {

using nlohmann::ordered_json;

enum JointIndex { kPivot, kHips, kTorso, kHead, kArmL, kArmR, kLegL, kLegR };

Description build_description() {
    Description d;
    d.joints = {
        {"pivot", -1, {0.0, 0.0, 0.0}},
        {"hips", kPivot, {0.0, 0.95, 0.0}},
        {"torso", kHips, {0.0, 0.10, 0.0}},
        {"head", kTorso, {0.0, 0.60, 0.0}},
        {"arm_l", kTorso, {0.25, 0.52, 0.0}},
        {"arm_r", kTorso, {-0.25, 0.52, 0.0}},
        {"leg_l", kHips, {0.11, 0.0, 0.0}},
        {"leg_r", kHips, {-0.11, 0.0, 0.0}},
    };
    // Front (+Z): nose and toes. Back (-Z): backpack. Left (+X): shield.
    d.parts = {
        {"pivot_marker", kPivot, {-0.06, 0.0, -0.06}, {0.06, 0.01, 0.06}, {1.0f, 0.0f, 1.0f}, -1},
        {"pivot_forward_tick", kPivot, {-0.015, 0.0, 0.06}, {0.015, 0.01, 0.22}, {1.0f, 0.0f, 1.0f}, -1},
        {"pelvis", kHips, {-0.17, -0.08, -0.10}, {0.17, 0.10, 0.10}, {0.35f, 0.35f, 0.38f}, -1},
        {"torso", kTorso, {-0.20, 0.0, -0.12}, {0.20, 0.55, 0.12}, {1.0f, 0.8f, 1.0f}, kHips, 0},
        {"backpack", kTorso, {-0.14, 0.15, -0.24}, {0.14, 0.50, -0.12}, {0.40f, 0.22f, 0.08f}, -1},
        {"head", kHead, {-0.11, 0.0, -0.11}, {0.11, 0.26, 0.11}, {0.90f, 0.70f, 0.55f}, -1},
        {"nose", kHead, {-0.025, 0.10, 0.11}, {0.025, 0.15, 0.16}, {0.85f, 0.05f, 0.05f}, -1},
        {"arm_l", kArmL, {-0.045, -0.60, -0.045}, {0.045, 0.0, 0.045}, {0.10f, 0.70f, 0.20f}, -1},
        {"shield", kArmL, {0.045, -0.50, -0.12}, {0.075, -0.20, 0.12}, {0.95f, 0.85f, 0.10f}, -1},
        {"arm_r", kArmR, {-0.045, -0.60, -0.045}, {0.045, 0.0, 0.045}, {0.95f, 0.50f, 0.05f}, -1},
        {"leg_l", kLegL, {-0.06, -0.95, -0.07}, {0.06, 0.0, 0.07}, {0.05f, 0.40f, 0.35f}, -1},
        {"foot_l", kLegL, {-0.06, -0.95, 0.07}, {0.06, -0.88, 0.18}, {0.05f, 0.20f, 0.15f}, -1},
        {"leg_r", kLegR, {-0.06, -0.95, -0.07}, {0.06, 0.0, 0.07}, {0.50f, 0.15f, 0.60f}, -1},
        {"foot_r", kLegR, {-0.06, -0.95, 0.07}, {0.06, -0.88, 0.18}, {0.25f, 0.05f, 0.30f}, -1},
    };

    // A 2x2 checker in four distinct colours, so that orientation shows:
    // blue, cream / red, navy (rows top to bottom).
    Image checker(2, 2);
    const std::uint8_t texels[4][4] = {
        {0x20, 0x50, 0xc0, 0xff}, {0xe8, 0xe0, 0xc8, 0xff}, {0xc0, 0x30, 0x30, 0xff}, {0x20, 0x30, 0x40, 0xff}};
    for (std::size_t i = 0; i < 4; ++i) std::memcpy(checker.rgba.data() + i * 4, texels[i], 4);
    d.textures = {checker};

    const Vec3 x{1, 0, 0};
    const Vec3 y{0, 1, 0};

    Animation idle;
    idle.name = "idle";
    idle.duration = 2.0;
    idle.translations.push_back({kHips, {0.0, 1.0, 2.0}, {{0, 0.95, 0}, {0, 0.93, 0}, {0, 0.95, 0}}});
    idle.rotations.push_back({kHead, y, {0.0, 0.5, 1.5, 2.0}, {0.0, 15.0, -15.0, 0.0}});
    idle.rotations.push_back({kArmL, x, {0.0, 1.0, 2.0}, {0.0, -5.0, 0.0}});

    Animation run;
    run.name = "run";
    run.duration = 0.8;
    const std::vector<double> t = {0.0, 0.2, 0.4, 0.6, 0.8};
    // Positive rotation about +X swings a hanging limb backwards (-Z).
    run.rotations.push_back({kLegL, x, t, {-35.0, 0.0, 35.0, 0.0, -35.0}});
    run.rotations.push_back({kLegR, x, t, {35.0, 0.0, -35.0, 0.0, 35.0}});
    run.rotations.push_back({kArmL, x, t, {30.0, 0.0, -30.0, 0.0, 30.0}});
    run.rotations.push_back({kArmR, x, t, {-30.0, 0.0, 30.0, 0.0, -30.0}});
    run.rotations.push_back({kTorso, y, t, {10.0, 0.0, -10.0, 0.0, 10.0}});
    run.translations.push_back(
        {kHips, t, {{0, 0.95, 0}, {0, 0.90, 0}, {0, 0.95, 0}, {0, 0.90, 0}, {0, 0.95, 0}}});

    d.animations = {idle, run};
    return d;
}

Vec3 rest_global(const Description& d, int joint) {
    Vec3 p;
    for (int j = joint; j >= 0; j = d.joints[static_cast<std::size_t>(j)].parent) {
        p = p + d.joints[static_cast<std::size_t>(j)].translation;
    }
    return p;
}

// Builds the binary chunk and the accessors that describe it.
class BufferBuilder {
public:
    explicit BufferBuilder(ordered_json& root) : root_(root) {
        root_["bufferViews"] = ordered_json::array();
        root_["accessors"] = ordered_json::array();
    }

    int floats(const std::vector<float>& values, int components, const char* type, bool min_max,
               int target = 0) {
        const int view = add_view(values.data(), values.size() * sizeof(float), target);
        ordered_json accessor = {{"bufferView", view},
                                 {"componentType", 5126},
                                 {"count", values.size() / static_cast<std::size_t>(components)},
                                 {"type", type}};
        if (min_max) {
            std::vector<float> lo(static_cast<std::size_t>(components), std::numeric_limits<float>::max());
            std::vector<float> hi(static_cast<std::size_t>(components), -std::numeric_limits<float>::max());
            for (std::size_t i = 0; i < values.size(); ++i) {
                const std::size_t c = i % static_cast<std::size_t>(components);
                lo[c] = std::min(lo[c], values[i]);
                hi[c] = std::max(hi[c], values[i]);
            }
            accessor["min"] = lo;
            accessor["max"] = hi;
        }
        return add_accessor(std::move(accessor));
    }

    int u16(const std::vector<std::uint16_t>& values, int components, const char* type, int target) {
        const int view = add_view(values.data(), values.size() * sizeof(std::uint16_t), target);
        return add_accessor({{"bufferView", view},
                             {"componentType", 5123},
                             {"count", values.size() / static_cast<std::size_t>(components)},
                             {"type", type}});
    }

    /// A buffer view over raw bytes (an embedded image).
    int bytes_view(const std::vector<std::uint8_t>& data) { return add_view(data.data(), data.size(), 0); }

    const std::vector<std::uint8_t>& bytes() const { return bin_; }

private:
    int add_view(const void* data, std::size_t size, int target) {
        while (bin_.size() % 4 != 0) bin_.push_back(0);
        // Little-endian hosts only write their memory as is; glTF is
        // little-endian.
        const auto* p = static_cast<const std::uint8_t*>(data);
        ordered_json view = {{"buffer", 0}, {"byteOffset", bin_.size()}, {"byteLength", size}};
        if (target != 0) view["target"] = target;
        bin_.insert(bin_.end(), p, p + size);
        root_["bufferViews"].push_back(view);
        return static_cast<int>(root_["bufferViews"].size() - 1);
    }

    int add_accessor(ordered_json accessor) {
        root_["accessors"].push_back(std::move(accessor));
        return static_cast<int>(root_["accessors"].size() - 1);
    }

    ordered_json& root_;
    std::vector<std::uint8_t> bin_;
};

constexpr int kArrayBuffer = 34962;
constexpr int kElementArrayBuffer = 34963;

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}

void push(std::vector<float>& out, const Vec3& v) {
    out.push_back(static_cast<float>(v.x));
    out.push_back(static_cast<float>(v.y));
    out.push_back(static_cast<float>(v.z));
}

}  // namespace

const Description& description() {
    static const Description d = build_description();
    return d;
}

std::vector<std::array<std::uint8_t, 3>> palette() {
    const Description& d = description();
    std::vector<std::array<std::uint8_t, 3>> out;
    auto add = [&](const std::array<double, 3>& linear) {
        std::array<std::uint8_t, 3> c{};
        for (std::size_t k = 0; k < 3; ++k) {
            c[k] = static_cast<std::uint8_t>(std::lround(std::clamp(linear_to_srgb(linear[k]), 0.0, 1.0) * 255.0));
        }
        if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    };
    for (const Part& part : d.parts) {
        if (part.texture < 0) {
            add({part.colour[0], part.colour[1], part.colour[2]});
            continue;
        }
        const Image& texture = d.textures[static_cast<std::size_t>(part.texture)];
        for (std::size_t i = 0; i + 3 < texture.rgba.size(); i += 4) {
            std::array<double, 3> linear{};
            for (std::size_t k = 0; k < 3; ++k) linear[k] = srgb_to_linear(texture.rgba[i + k] / 255.0) * part.colour[k];
            add(linear);
        }
    }
    return out;
}

std::vector<std::uint8_t> make_glb() {
    static_assert(std::numeric_limits<float>::is_iec559, "glTF needs IEEE 754 floats");
    {
        const std::uint16_t probe = 1;
        std::uint8_t first = 0;
        std::memcpy(&first, &probe, 1);
        if (first != 1) throw std::runtime_error("make_glb: big-endian hosts are not supported");
    }
    const Description& d = description();
    ordered_json root;
    root["asset"] = {
        {"version", "2.0"},
        {"generator", "pose-atlas " + version() + " test_rig::make_glb"},
        {"copyright",
         "Copyright 2026 the pose-atlas authors. Licensed under the Apache License, Version 2.0."}};
    root["extras"] = {{"license", "Apache-2.0"},
                      {"description",
                       "Procedurally generated pose-atlas test figure: boxes on a skin of eight "
                       "joints, faces +Z, +Y up, metres; animations idle and run."}};
    root["scene"] = 0;
    BufferBuilder buffer(root);

    // Nodes 0..7 are the joints; node 8 holds the skinned mesh.
    ordered_json nodes = ordered_json::array();
    for (std::size_t j = 0; j < d.joints.size(); ++j) {
        const Joint& joint = d.joints[j];
        ordered_json node = {{"name", joint.name},
                             {"translation", {joint.translation.x, joint.translation.y, joint.translation.z}}};
        ordered_json children = ordered_json::array();
        for (std::size_t c = 0; c < d.joints.size(); ++c) {
            if (d.joints[c].parent == static_cast<int>(j)) children.push_back(c);
        }
        if (!children.empty()) node["children"] = children;
        nodes.push_back(node);
    }
    const std::size_t mesh_node = d.joints.size();
    nodes.push_back({{"name", "figure"}, {"mesh", 0}, {"skin", 0}});
    root["nodes"] = nodes;
    root["scenes"] = {{{"name", "test_rig"}, {"nodes", {0, mesh_node}}}};

    // Skin: rest joints have no rotation, so the inverse bind matrices are
    // pure translations (column-major).
    std::vector<float> inverse_bind;
    ordered_json joint_nodes = ordered_json::array();
    for (std::size_t j = 0; j < d.joints.size(); ++j) {
        const Vec3 g = rest_global(d, static_cast<int>(j));
        const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, static_cast<float>(-g.x),
                             static_cast<float>(-g.y), static_cast<float>(-g.z), 1};
        inverse_bind.insert(inverse_bind.end(), m, m + 16);
        joint_nodes.push_back(j);
    }
    const int ibm = buffer.floats(inverse_bind, 16, "MAT4", false);
    root["skins"] = {{{"name", "test_rig_skin"},
                      {"inverseBindMatrices", ibm},
                      {"skeleton", 0},
                      {"joints", joint_nodes}}};

    // One primitive and one material per part, so every part has its own
    // flat colour.
    ordered_json primitives = ordered_json::array();
    ordered_json materials = ordered_json::array();
    for (std::size_t p = 0; p < d.parts.size(); ++p) {
        const Part& part = d.parts[p];
        const Vec3 origin = rest_global(d, part.joint);
        std::vector<float> positions, normals, weights, texcoords;
        std::vector<std::uint16_t> joints, indices;
        for (int axis = 0; axis < 3; ++axis) {
            for (int sign : {1, -1}) {
                const auto base = static_cast<std::uint16_t>(positions.size() / 3);
                const int ua = (axis + 1) % 3;
                const int va = (axis + 2) % 3;
                // Counter-clockwise seen from outside.
                const int corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                for (int k = 0; k < 4; ++k) {
                    const int cu = corners[sign > 0 ? k : (4 - k) % 4][0];
                    const int cv = corners[sign > 0 ? k : (4 - k) % 4][1];
                    double local[3];
                    local[axis] = sign > 0 ? part.max[axis] : part.min[axis];
                    local[ua] = cu ? part.max[ua] : part.min[ua];
                    local[va] = cv ? part.max[va] : part.min[va];
                    push(positions, origin + Vec3{local[0], local[1], local[2]});
                    double n[3] = {0, 0, 0};
                    n[axis] = sign;
                    push(normals, {n[0], n[1], n[2]});
                    texcoords.push_back(static_cast<float>(cu));
                    texcoords.push_back(static_cast<float>(1 - cv));
                    const bool blended = part.blend_joint >= 0 && local[1] == part.min.y;
                    joints.insert(joints.end(),
                                  {static_cast<std::uint16_t>(part.joint),
                                   static_cast<std::uint16_t>(blended ? part.blend_joint : 0), 0, 0});
                    weights.insert(weights.end(), {blended ? 0.5f : 1.0f, blended ? 0.5f : 0.0f, 0.0f, 0.0f});
                }
                for (int k : {0, 1, 2, 0, 2, 3}) indices.push_back(static_cast<std::uint16_t>(base + k));
            }
        }
        ordered_json attributes;
        attributes["POSITION"] = buffer.floats(positions, 3, "VEC3", true, kArrayBuffer);
        attributes["NORMAL"] = buffer.floats(normals, 3, "VEC3", false, kArrayBuffer);
        if (part.texture >= 0) attributes["TEXCOORD_0"] = buffer.floats(texcoords, 2, "VEC2", false, kArrayBuffer);
        attributes["JOINTS_0"] = buffer.u16(joints, 4, "VEC4", kArrayBuffer);
        attributes["WEIGHTS_0"] = buffer.floats(weights, 4, "VEC4", false, kArrayBuffer);
        const int index_accessor = buffer.u16(indices, 1, "SCALAR", kElementArrayBuffer);
        primitives.push_back({{"attributes", attributes},
                              {"indices", index_accessor},
                              {"material", p},
                              {"mode", 4},
                              {"extras", {{"part", part.name}}}});
        ordered_json pbr = {{"baseColorFactor", {part.colour[0], part.colour[1], part.colour[2], 1.0f}},
                            {"metallicFactor", 0.0},
                            {"roughnessFactor", 1.0}};
        if (part.texture >= 0) pbr["baseColorTexture"] = {{"index", part.texture}};
        materials.push_back({{"name", part.name}, {"pbrMetallicRoughness", pbr}});
    }
    root["meshes"] = {{{"name", "figure"}, {"primitives", primitives}}};
    root["materials"] = materials;
    if (!d.textures.empty()) {
        // Nearest filtering (9728), clamp to edge (33071).
        root["samplers"] = {{{"magFilter", 9728}, {"minFilter", 9728}, {"wrapS", 33071}, {"wrapT", 33071}}};
        ordered_json images = ordered_json::array();
        ordered_json textures = ordered_json::array();
        for (std::size_t t = 0; t < d.textures.size(); ++t) {
            const std::vector<std::uint8_t> png = encode_png(d.textures[t]);
            images.push_back({{"name", "texture" + std::to_string(t)},
                              {"bufferView", buffer.bytes_view(png)},
                              {"mimeType", "image/png"}});
            textures.push_back({{"sampler", 0}, {"source", t}});
        }
        root["images"] = images;
        root["textures"] = textures;
    }

    ordered_json animations = ordered_json::array();
    for (const Animation& animation : d.animations) {
        ordered_json samplers = ordered_json::array();
        ordered_json channels = ordered_json::array();
        auto add = [&](int joint, const std::vector<double>& times, const std::vector<float>& values,
                       int components, const char* type, const char* path) {
            std::vector<float> input;
            for (double time : times) input.push_back(static_cast<float>(time));
            const int in = buffer.floats(input, 1, "SCALAR", true);
            const int out = buffer.floats(values, components, type, false);
            samplers.push_back({{"input", in}, {"output", out}, {"interpolation", "LINEAR"}});
            channels.push_back(
                {{"sampler", samplers.size() - 1}, {"target", {{"node", joint}, {"path", path}}}});
        };
        for (const RotationTrack& track : animation.rotations) {
            std::vector<float> quaternions;
            for (double angle : track.angles_degrees) {
                const double half = radians(angle) / 2;
                const double s = std::sin(half);
                quaternions.insert(quaternions.end(),
                                   {static_cast<float>(track.axis.x * s), static_cast<float>(track.axis.y * s),
                                    static_cast<float>(track.axis.z * s), static_cast<float>(std::cos(half))});
            }
            add(track.joint, track.times, quaternions, 4, "VEC4", "rotation");
        }
        for (const TranslationTrack& track : animation.translations) {
            std::vector<float> values;
            for (const Vec3& v : track.values) push(values, v);
            add(track.joint, track.times, values, 3, "VEC3", "translation");
        }
        animations.push_back({{"name", animation.name}, {"channels", channels}, {"samplers", samplers}});
    }
    root["animations"] = animations;

    std::vector<std::uint8_t> bin = buffer.bytes();
    while (bin.size() % 4 != 0) bin.push_back(0);
    root["buffers"] = {{{"byteLength", bin.size()}}};

    std::string text = root.dump();
    while (text.size() % 4 != 0) text.push_back(' ');

    std::vector<std::uint8_t> glb;
    const std::size_t total = 12 + 8 + text.size() + 8 + bin.size();
    append_u32(glb, 0x46546C67u);  // "glTF"
    append_u32(glb, 2);
    append_u32(glb, static_cast<std::uint32_t>(total));
    append_u32(glb, static_cast<std::uint32_t>(text.size()));
    append_u32(glb, 0x4E4F534Au);  // "JSON"
    glb.insert(glb.end(), text.begin(), text.end());
    append_u32(glb, static_cast<std::uint32_t>(bin.size()));
    append_u32(glb, 0x004E4942u);  // "BIN\0"
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

void write_glb(const std::string& path) {
    const std::vector<std::uint8_t> glb = make_glb();
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error(path + ": cannot open for writing");
    file.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    if (!file) throw std::runtime_error(path + ": write failed");
}

}  // namespace pose_atlas::test_rig
