// SPDX-License-Identifier: Apache-2.0
// glTF 2.0 loading with cgltf.

#include <cgltf.h>

#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <string>

#include "pose_atlas/model.hpp"

namespace pose_atlas {

namespace {

const char* result_name(cgltf_result result) {
    switch (result) {
        case cgltf_result_success: return "success";
        case cgltf_result_data_too_short: return "data too short";
        case cgltf_result_unknown_format: return "unknown format (not glTF or GLB)";
        case cgltf_result_invalid_json: return "invalid JSON";
        case cgltf_result_invalid_gltf: return "invalid glTF";
        case cgltf_result_invalid_options: return "invalid options";
        case cgltf_result_file_not_found: return "file not found";
        case cgltf_result_io_error: return "I/O error";
        case cgltf_result_out_of_memory: return "out of memory";
        case cgltf_result_legacy_gltf: return "glTF 1.0 is not supported";
        default: return "unknown error";
    }
}

struct DataDeleter {
    void operator()(cgltf_data* data) const { cgltf_free(data); }
};
using DataPtr = std::unique_ptr<cgltf_data, DataDeleter>;

std::atomic<std::uint64_t> next_texture_id{1};

// RFC 4648 base64 (data: URIs). Returns false on malformed input.
bool decode_base64(const char* text, std::size_t size, std::vector<std::uint8_t>& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    std::uint32_t buffer = 0;
    int bits = 0;
    std::size_t i = 0;
    for (; i < size && text[i] != '='; ++i) {
        const int v = value(text[i]);
        if (v < 0) return false;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFFu));
        }
    }
    for (; i < size; ++i) {
        if (text[i] != '=') return false;
    }
    return true;
}

Wrap wrap_mode(cgltf_wrap_mode mode) {
    switch (mode) {
        case cgltf_wrap_mode_clamp_to_edge: return Wrap::ClampToEdge;
        case cgltf_wrap_mode_mirrored_repeat: return Wrap::MirroredRepeat;
        default: return Wrap::Repeat;
    }
}

class Converter {
public:
    Converter(const cgltf_data* data, std::string source, std::string base_path)
        : data_(data), source_(std::move(source)), base_path_(std::move(base_path)) {}

    Model convert() {
        Model model;
        model.source = source_;
        convert_nodes(model);
        convert_meshes(model);
        convert_skins(model);
        convert_animations(model);
        return model;
    }

private:
    [[noreturn]] void fail(const std::string& what) const { throw ModelError(source_ + ": " + what); }

    int node_index(const cgltf_node* node) const {
        return static_cast<int>(cgltf_node_index(data_, node));
    }

    static std::string name_or(const char* name, const std::string& fallback) {
        return name != nullptr ? std::string(name) : fallback;
    }

    std::vector<double> floats(const cgltf_accessor* accessor, cgltf_size components,
                               const std::string& what) const {
        if (accessor == nullptr) fail(what + ": missing accessor");
        if (cgltf_num_components(accessor->type) != components) {
            fail(what + ": expected " + std::to_string(components) + " components per element");
        }
        std::vector<float> raw(accessor->count * components);
        if (cgltf_accessor_unpack_floats(accessor, raw.data(), raw.size()) != raw.size()) {
            fail(what + ": cannot read accessor data");
        }
        return {raw.begin(), raw.end()};
    }

    void convert_nodes(Model& model) const {
        model.nodes.resize(data_->nodes_count);
        for (cgltf_size i = 0; i < data_->nodes_count; ++i) {
            const cgltf_node& in = data_->nodes[i];
            Node& out = model.nodes[i];
            out.name = name_or(in.name, "node" + std::to_string(i));
            out.parent = in.parent != nullptr ? node_index(in.parent) : -1;
            for (cgltf_size c = 0; c < in.children_count; ++c) out.children.push_back(node_index(in.children[c]));
            if (in.has_matrix) {
                out.has_matrix = true;
                for (std::size_t k = 0; k < 16; ++k) out.matrix.m[k] = in.matrix[k];
            }
            if (in.has_translation) out.translation = {in.translation[0], in.translation[1], in.translation[2]};
            if (in.has_rotation) {
                out.rotation = {in.rotation[0], in.rotation[1], in.rotation[2], in.rotation[3]};
            }
            if (in.has_scale) out.scale = {in.scale[0], in.scale[1], in.scale[2]};
            out.mesh = in.mesh != nullptr ? static_cast<int>(cgltf_mesh_index(data_, in.mesh)) : -1;
            out.skin = in.skin != nullptr ? static_cast<int>(cgltf_skin_index(data_, in.skin)) : -1;
        }
    }

    void convert_meshes(Model& model) {
        for (cgltf_size m = 0; m < data_->meshes_count; ++m) {
            const cgltf_mesh& in = data_->meshes[m];
            Mesh mesh;
            mesh.name = name_or(in.name, "mesh" + std::to_string(m));
            for (cgltf_size p = 0; p < in.primitives_count; ++p) {
                const std::string what = "mesh \"" + mesh.name + "\" primitive " + std::to_string(p);
                mesh.primitives.push_back(convert_primitive(model, in.primitives[p], what));
            }
            model.meshes.push_back(std::move(mesh));
        }
    }

    Primitive convert_primitive(Model& model, const cgltf_primitive& in, const std::string& what) {
        if (in.type != cgltf_primitive_type_triangles) fail(what + ": only triangle lists are supported");
        Primitive out;
        const cgltf_accessor* joints = nullptr;
        const cgltf_accessor* weights = nullptr;
        std::map<cgltf_int, const cgltf_accessor*> texcoord_sets;
        for (cgltf_size a = 0; a < in.attributes_count; ++a) {
            const cgltf_attribute& attribute = in.attributes[a];
            if (attribute.type == cgltf_attribute_type_position) {
                const auto v = floats(attribute.data, 3, what + " POSITION");
                for (std::size_t i = 0; i + 2 < v.size(); i += 3) out.positions.push_back({v[i], v[i + 1], v[i + 2]});
            } else if (attribute.type == cgltf_attribute_type_normal) {
                const auto v = floats(attribute.data, 3, what + " NORMAL");
                for (std::size_t i = 0; i + 2 < v.size(); i += 3) out.normals.push_back({v[i], v[i + 1], v[i + 2]});
            } else if (attribute.type == cgltf_attribute_type_joints && attribute.index == 0) {
                joints = attribute.data;
            } else if (attribute.type == cgltf_attribute_type_weights && attribute.index == 0) {
                weights = attribute.data;
            } else if (attribute.type == cgltf_attribute_type_texcoord) {
                texcoord_sets[attribute.index] = attribute.data;
            }
        }
        if (out.positions.empty()) fail(what + ": no POSITION attribute");
        if (!out.normals.empty() && out.normals.size() != out.positions.size()) {
            fail(what + ": NORMAL count differs from POSITION count");
        }
        if ((joints == nullptr) != (weights == nullptr)) fail(what + ": JOINTS_0 and WEIGHTS_0 must come together");
        if (joints != nullptr) {
            if (joints->count != out.positions.size() || weights->count != out.positions.size()) {
                fail(what + ": JOINTS_0/WEIGHTS_0 count differs from POSITION count");
            }
            for (cgltf_size i = 0; i < joints->count; ++i) {
                cgltf_uint j[4] = {0, 0, 0, 0};
                if (!cgltf_accessor_read_uint(joints, i, j, 4)) fail(what + ": cannot read JOINTS_0");
                std::array<std::uint16_t, 4> joint{};
                for (std::size_t k = 0; k < 4; ++k) {
                    if (j[k] > 0xFFFFu) fail(what + ": joint index out of range");
                    joint[k] = static_cast<std::uint16_t>(j[k]);
                }
                out.joints.push_back(joint);
            }
            const auto w = floats(weights, 4, what + " WEIGHTS_0");
            for (std::size_t i = 0; i + 3 < w.size(); i += 4) {
                out.weights.push_back({static_cast<float>(w[i]), static_cast<float>(w[i + 1]),
                                       static_cast<float>(w[i + 2]), static_cast<float>(w[i + 3])});
            }
        }
        if (in.indices != nullptr) {
            for (cgltf_size i = 0; i < in.indices->count; ++i) {
                const cgltf_size index = cgltf_accessor_read_index(in.indices, i);
                if (index >= out.positions.size()) fail(what + ": index out of range");
                out.indices.push_back(static_cast<std::uint32_t>(index));
            }
        } else {
            for (std::size_t i = 0; i < out.positions.size(); ++i) out.indices.push_back(static_cast<std::uint32_t>(i));
        }
        if (out.indices.size() % 3 != 0) fail(what + ": index count is not a multiple of 3");
        if (in.material != nullptr) {
            out.material = name_or(in.material->name, "material" + std::to_string(cgltf_material_index(data_, in.material)));
            if (in.material->has_pbr_metallic_roughness) {
                const cgltf_pbr_metallic_roughness& pbr = in.material->pbr_metallic_roughness;
                const float* c = pbr.base_color_factor;
                out.base_colour = {c[0], c[1], c[2], c[3]};
                if (pbr.base_color_texture.texture != nullptr) {
                    convert_texture_view(model, pbr.base_color_texture, texcoord_sets, out, what);
                }
            }
            if (in.material->alpha_mode == cgltf_alpha_mode_mask) {
                out.alpha_mode = AlphaMode::Mask;
                out.alpha_cutoff = in.material->alpha_cutoff;
            } else if (in.material->alpha_mode == cgltf_alpha_mode_blend) {
                out.alpha_mode = AlphaMode::Blend;
            }
        }
        return out;
    }

    // The base colour texture of a primitive: its texture coordinates (with
    // KHR_texture_transform applied) and the texture itself, decoded once.
    void convert_texture_view(Model& model, const cgltf_texture_view& view,
                              const std::map<cgltf_int, const cgltf_accessor*>& sets, Primitive& out,
                              const std::string& what) {
        cgltf_int set = view.texcoord;
        if (view.has_transform && view.transform.has_texcoord) set = view.transform.texcoord;
        const auto found = sets.find(set);
        if (found == sets.end()) {
            fail(what + ": baseColorTexture uses TEXCOORD_" + std::to_string(set) + ", which the primitive lacks");
        }
        const auto uv = floats(found->second, 2, what + " TEXCOORD_" + std::to_string(set));
        if (uv.size() != out.positions.size() * 2) fail(what + ": TEXCOORD count differs from POSITION count");
        // KHR_texture_transform: uv' = T(offset) * R(rotation) * S(scale) * uv.
        double ox = 0, oy = 0, rotation = 0, sx = 1, sy = 1;
        if (view.has_transform) {
            ox = view.transform.offset[0];
            oy = view.transform.offset[1];
            rotation = view.transform.rotation;
            sx = view.transform.scale[0];
            sy = view.transform.scale[1];
        }
        const double c = std::cos(rotation), s = std::sin(rotation);
        for (std::size_t i = 0; i + 1 < uv.size(); i += 2) {
            const double u = uv[i] * sx, v = uv[i + 1] * sy;
            out.texcoords.push_back({static_cast<float>(c * u + s * v + ox), static_cast<float>(-s * u + c * v + oy)});
        }
        const cgltf_size index = cgltf_texture_index(data_, view.texture);
        const auto known = texture_map_.find(index);
        if (known != texture_map_.end()) {
            out.base_colour_texture = known->second;
            return;
        }
        const cgltf_texture& texture = *view.texture;
        Texture t;
        t.name = name_or(texture.name, "texture" + std::to_string(index));
        if (texture.image == nullptr) {
            fail(what + ": texture \"" + t.name + "\" has no PNG or JPEG image (KTX2 and WebP are not supported)");
        }
        try {
            t.image = decode_image(image_bytes(*texture.image, what), "texture \"" + t.name + "\"");
        } catch (const ImageError& e) {
            fail(what + ": " + e.what());
        }
        if (texture.sampler != nullptr) {
            t.wrap_s = wrap_mode(texture.sampler->wrap_s);
            t.wrap_t = wrap_mode(texture.sampler->wrap_t);
        }
        t.id = next_texture_id.fetch_add(1);
        model.textures.push_back(std::move(t));
        out.base_colour_texture = static_cast<int>(model.textures.size() - 1);
        texture_map_[index] = out.base_colour_texture;
    }

    // Encoded bytes of an image: from a buffer view, a data: URI or a file
    // next to the glTF file.
    std::vector<std::uint8_t> image_bytes(const cgltf_image& image, const std::string& what) const {
        if (image.buffer_view != nullptr) {
            const cgltf_buffer_view& view = *image.buffer_view;
            const auto* data = static_cast<const std::uint8_t*>(view.buffer->data);
            if (data == nullptr) fail(what + ": image buffer is not loaded");
            return {data + view.offset, data + view.offset + view.size};
        }
        if (image.uri == nullptr) fail(what + ": image has neither a bufferView nor a uri");
        const std::string uri = image.uri;
        if (uri.rfind("data:", 0) == 0) {
            const auto marker = uri.find(";base64,");
            std::vector<std::uint8_t> bytes;
            if (marker == std::string::npos ||
                !decode_base64(uri.data() + marker + 8, uri.size() - marker - 8, bytes)) {
                fail(what + ": image data URI is not base64");
            }
            return bytes;
        }
        if (base_path_.empty()) fail(what + ": image \"" + uri + "\" is an external file, but the model was not loaded from a file");
        std::string decoded = uri;
        decoded.resize(cgltf_decode_uri(&decoded[0]));
        const auto slash = base_path_.find_last_of("/\\");
        const std::string path = (slash == std::string::npos ? std::string() : base_path_.substr(0, slash + 1)) + decoded;
        try {
            return read_file(path);
        } catch (const std::exception& e) {
            fail(what + ": " + e.what());
        }
    }

    void convert_skins(Model& model) const {
        for (cgltf_size s = 0; s < data_->skins_count; ++s) {
            const cgltf_skin& in = data_->skins[s];
            Skin skin;
            skin.name = name_or(in.name, "skin" + std::to_string(s));
            for (cgltf_size j = 0; j < in.joints_count; ++j) skin.joints.push_back(node_index(in.joints[j]));
            skin.skeleton = in.skeleton != nullptr ? node_index(in.skeleton) : -1;
            if (in.inverse_bind_matrices != nullptr) {
                const auto v = floats(in.inverse_bind_matrices, 16, "skin \"" + skin.name + "\" inverseBindMatrices");
                if (v.size() != in.joints_count * 16) fail("skin \"" + skin.name + "\": one inverse bind matrix per joint expected");
                for (std::size_t j = 0; j < in.joints_count; ++j) {
                    Mat4 m;
                    for (std::size_t k = 0; k < 16; ++k) m.m[k] = v[j * 16 + k];
                    skin.inverse_bind.push_back(m);
                }
            } else {
                skin.inverse_bind.assign(in.joints_count, Mat4{});
            }
            model.skins.push_back(std::move(skin));
        }
        // Every skinned primitive's joint indices must exist in its skin.
        for (const Node& node : model.nodes) {
            if (node.mesh < 0 || node.skin < 0) continue;
            const std::size_t joint_count = model.skins[static_cast<std::size_t>(node.skin)].joints.size();
            for (const Primitive& primitive : model.meshes[static_cast<std::size_t>(node.mesh)].primitives) {
                for (const auto& joints : primitive.joints) {
                    for (auto j : joints) {
                        if (j >= joint_count) fail("node \"" + node.name + "\": joint index " + std::to_string(j) + " outside its skin");
                    }
                }
            }
        }
    }

    void convert_animations(Model& model) const {
        for (cgltf_size a = 0; a < data_->animations_count; ++a) {
            const cgltf_animation& in = data_->animations[a];
            Animation animation;
            animation.name = name_or(in.name, "animation" + std::to_string(a));
            bool first = true;
            for (cgltf_size c = 0; c < in.channels_count; ++c) {
                const cgltf_animation_channel& channel = in.channels[c];
                const std::string what = "animation \"" + animation.name + "\" channel " + std::to_string(c);
                if (channel.target_node == nullptr) continue;
                Channel out;
                out.node = node_index(channel.target_node);
                cgltf_size components = 3;
                switch (channel.target_path) {
                    case cgltf_animation_path_type_translation: out.path = ChannelPath::Translation; break;
                    case cgltf_animation_path_type_rotation:
                        out.path = ChannelPath::Rotation;
                        components = 4;
                        break;
                    case cgltf_animation_path_type_scale: out.path = ChannelPath::Scale; break;
                    default:
                        ++animation.ignored_channels;
                        continue;
                }
                if (model.nodes[static_cast<std::size_t>(out.node)].has_matrix) {
                    fail(what + ": animates node \"" + model.nodes[static_cast<std::size_t>(out.node)].name +
                         "\", which has a matrix (not allowed by glTF)");
                }
                const cgltf_animation_sampler* sampler = channel.sampler;
                switch (sampler->interpolation) {
                    case cgltf_interpolation_type_linear: out.interpolation = Interpolation::Linear; break;
                    case cgltf_interpolation_type_step: out.interpolation = Interpolation::Step; break;
                    case cgltf_interpolation_type_cubic_spline: out.interpolation = Interpolation::CubicSpline; break;
                    default: fail(what + ": unknown interpolation");
                }
                out.times = floats(sampler->input, 1, what + " input");
                out.values = floats(sampler->output, components, what + " output");
                if (out.times.empty()) fail(what + ": no keys");
                for (std::size_t k = 1; k < out.times.size(); ++k) {
                    if (!(out.times[k] > out.times[k - 1])) fail(what + ": key times must increase strictly");
                }
                const std::size_t per_key = out.interpolation == Interpolation::CubicSpline ? 3 * components : components;
                if (out.values.size() != out.times.size() * per_key) fail(what + ": output count does not match the keys");
                if (first || out.times.front() < animation.start_s) animation.start_s = out.times.front();
                if (first || out.times.back() > animation.end_s) animation.end_s = out.times.back();
                first = false;
                animation.channels.push_back(std::move(out));
            }
            model.animations.push_back(std::move(animation));
        }
    }

    const cgltf_data* data_;
    std::string source_;
    std::string base_path_;  // the glTF file, for external images; empty for bytes
    std::map<cgltf_size, int> texture_map_;  // glTF texture -> Model::textures
};

Model finish(cgltf_data* raw, cgltf_result parsed, const std::string& source, const char* buffer_path) {
    DataPtr data(raw);
    if (parsed != cgltf_result_success) throw ModelError(source + ": cannot parse: " + result_name(parsed));
    cgltf_options options{};
    const cgltf_result loaded = cgltf_load_buffers(&options, data.get(), buffer_path);
    if (loaded != cgltf_result_success) {
        throw ModelError(source + ": cannot load buffers: " + result_name(loaded));
    }
    const cgltf_result valid = cgltf_validate(data.get());
    if (valid != cgltf_result_success) throw ModelError(source + ": failed validation: " + result_name(valid));
    return Converter(data.get(), source, buffer_path != nullptr ? buffer_path : "").convert();
}

}  // namespace

Model load_gltf(const std::string& path) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    const cgltf_result parsed = cgltf_parse_file(&options, path.c_str(), &data);
    return finish(data, parsed, path, path.c_str());
}

Model load_gltf(const std::vector<std::uint8_t>& bytes, const std::string& source) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    const cgltf_result parsed = cgltf_parse(&options, bytes.data(), bytes.size(), &data);
    return finish(data, parsed, source, nullptr);
}

}  // namespace pose_atlas
