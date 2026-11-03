// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include <cgltf.h>

#include <cmath>
#include <set>
#include <string>
#include <tuple>

#include "pose_atlas/test_rig.hpp"

using namespace pose_atlas;

namespace {

struct Parsed {
    cgltf_data* data = nullptr;
    std::vector<std::uint8_t> bytes;
    explicit Parsed(std::vector<std::uint8_t> glb) : bytes(std::move(glb)) {
        cgltf_options options{};
        REQUIRE(cgltf_parse(&options, bytes.data(), bytes.size(), &data) == cgltf_result_success);
        REQUIRE(cgltf_load_buffers(&options, data, nullptr) == cgltf_result_success);
    }
    ~Parsed() { cgltf_free(data); }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
};

Vec3 read_vec3(const cgltf_accessor* accessor, cgltf_size index) {
    float v[3] = {0, 0, 0};
    REQUIRE(cgltf_accessor_read_float(accessor, index, v, 3));
    return {v[0], v[1], v[2]};
}

const cgltf_accessor* attribute(const cgltf_primitive& primitive, cgltf_attribute_type type) {
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        if (primitive.attributes[i].type == type && primitive.attributes[i].index == 0) {
            return primitive.attributes[i].data;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("test rig: generated GLB is valid glTF with the described content") {
    Parsed glb(test_rig::make_glb());
    cgltf_data* data = glb.data;
    CHECK(cgltf_validate(data) == cgltf_result_success);
    CHECK(data->file_type == cgltf_file_type_glb);
    const auto& d = test_rig::description();

    REQUIRE(data->asset.copyright != nullptr);
    CHECK(std::string(data->asset.copyright).find("Apache License, Version 2.0") != std::string::npos);

    REQUIRE(data->skins_count == 1);
    CHECK(data->skins[0].joints_count == d.joints.size());
    for (std::size_t j = 0; j < d.joints.size(); ++j) {
        CHECK(std::string(data->skins[0].joints[j]->name) == d.joints[j].name);
    }

    REQUIRE(data->animations_count == 2);
    CHECK(std::string(data->animations[0].name) == "idle");
    CHECK(std::string(data->animations[1].name) == "run");
    for (cgltf_size a = 0; a < 2; ++a) {
        float last = 0.0f;
        for (cgltf_size s = 0; s < data->animations[a].samplers_count; ++s) {
            last = std::max(last, data->animations[a].samplers[s].input->max[0]);
        }
        CHECK(last == static_cast<float>(d.animations[a].duration));
    }
    CHECK(d.animations[0].duration != d.animations[1].duration);

    // One primitive and a distinct colour per part.
    REQUIRE(data->meshes_count == 1);
    REQUIRE(data->meshes[0].primitives_count == d.parts.size());
    std::set<std::tuple<float, float, float>> colours;
    for (cgltf_size p = 0; p < data->materials_count; ++p) {
        const float* c = data->materials[p].pbr_metallic_roughness.base_color_factor;
        colours.insert({c[0], c[1], c[2]});
    }
    // The pivot marker and its forward tick share one colour.
    CHECK(colours.size() == d.parts.size() - 1);
}

TEST_CASE("test rig: boxes are closed, outward-facing and the pivot marker sits on the origin") {
    Parsed glb(test_rig::make_glb());
    const cgltf_mesh& mesh = glb.data->meshes[0];
    for (cgltf_size p = 0; p < mesh.primitives_count; ++p) {
        const cgltf_primitive& primitive = mesh.primitives[p];
        const cgltf_accessor* positions = attribute(primitive, cgltf_attribute_type_position);
        const cgltf_accessor* normals = attribute(primitive, cgltf_attribute_type_normal);
        REQUIRE(positions != nullptr);
        REQUIRE(normals != nullptr);
        REQUIRE(primitive.indices->count == 36);
        for (cgltf_size t = 0; t < 36; t += 3) {
            const cgltf_size i0 = cgltf_accessor_read_index(primitive.indices, t);
            const cgltf_size i1 = cgltf_accessor_read_index(primitive.indices, t + 1);
            const cgltf_size i2 = cgltf_accessor_read_index(primitive.indices, t + 2);
            const Vec3 a = read_vec3(positions, i0);
            const Vec3 face = cross(read_vec3(positions, i1) - a, read_vec3(positions, i2) - a);
            CHECK(dot(face, read_vec3(normals, i0)) > 0.0);
        }
    }
    const cgltf_accessor* marker = attribute(mesh.primitives[0], cgltf_attribute_type_position);
    CHECK(marker->min[1] == 0.0f);
    CHECK(marker->max[1] == 0.01f);
    CHECK(marker->min[0] == -marker->max[0]);
    CHECK(marker->min[2] == -marker->max[2]);
    // The feet rest on the ground.
    const cgltf_accessor* foot = attribute(mesh.primitives[11], cgltf_attribute_type_position);
    CHECK(std::fabs(foot->min[1]) < 1e-6f);
}

TEST_CASE("test rig: output is deterministic") {
    CHECK(test_rig::make_glb() == test_rig::make_glb());
}
