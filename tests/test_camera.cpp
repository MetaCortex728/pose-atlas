// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "pose_atlas/camera.hpp"

using namespace pose_atlas;
using nlohmann::json;

namespace {

const std::string kSource = POSE_ATLAS_SOURCE_DIR;

json read_json(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return json::parse(text.str());
}

Vec3 vec3(const json& v) { return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()}; }

void check_vec(const Vec3& actual, const json& expected, double tolerance) {
    CHECK(std::fabs(actual.x - expected[0].get<double>()) <= tolerance);
    CHECK(std::fabs(actual.y - expected[1].get<double>()) <= tolerance);
    CHECK(std::fabs(actual.z - expected[2].get<double>()) <= tolerance);
}

}  // namespace

TEST_CASE("camera: guides and projections match prototype/dan_projection.py") {
    // Expected values: tests/fixtures/make_camera_fixture.py, which runs the
    // prototype's Python on prototype/quarter-view-profile.json.
    const json fixture = read_json(kSource + "/tests/fixtures/camera_guides_quarter_view.json");
    const Profile profile = load_profile(kSource + "/profiles/quarter-view.json");
    REQUIRE(fixture["guides"].size() == 8);
    constexpr double kTol = 1e-12;
    for (std::size_t i = 0; i < 8; ++i) {
        const json& expected = fixture["guides"][i];
        const CameraGuide guide = camera_guide(profile, expected["direction"].get<std::string>());
        CAPTURE(guide.direction);
        CHECK(guide.heading_index == static_cast<int>(i));
        check_vec(guide.toward, expected["toward"], kTol);
        check_vec(guide.right, expected["right"], kTol);
        check_vec(guide.up, expected["up"], kTol);
        check_vec(guide.position, expected["position"], kTol);
        CHECK(std::fabs(guide.ortho_scale_m - expected["ortho_scale_m"].get<double>()) <= kTol);
        CHECK(std::fabs(guide.camera_rotation_degrees -
                        expected["camera_rotation_z_degrees"].get<double>()) <= 1e-10);
        CHECK(std::fabs(guide.vertical_projection_factor -
                        expected["vertical_projection_factor"].get<double>()) <= kTol);
        CHECK(std::fabs(guide.ground_depth_factor - expected["ground_depth_factor"].get<double>()) <=
              kTol);
        for (std::size_t k = 0; k < fixture["points"].size(); ++k) {
            const Vec2 pixel = project(profile, guide, vec3(fixture["points"][k]));
            CHECK(std::fabs(pixel.x - expected["projected"][k][0].get<double>()) <= 1e-9);
            CHECK(std::fabs(pixel.y - expected["projected"][k][1].get<double>()) <= 1e-9);
        }
    }
}

TEST_CASE("camera: the character's forward axis projects onto its screen heading") {
    for (const char* name : {"quarter-view.json", "quarter-view-gltf.json"}) {
        const Profile profile = load_profile(kSource + "/profiles/" + name);
        CAPTURE(name);
        for (int i = 0; i < 8; ++i) {
            const CameraGuide guide = camera_guide(profile, i);
            const Vec2 forward = projected_forward(profile, guide);
            const double angle = i * kPi / 4;
            const double length = std::hypot(forward.x, forward.y);
            CHECK(std::hypot(forward.x / length - std::cos(angle), forward.y / length - std::sin(angle)) <
                  1e-12);
            // Foreshortening: full length across the screen, sin(elevation)
            // straight into it.
            const double sp = std::sin(radians(profile.elevation_degrees));
            if (i == 0 || i == 4) CHECK(std::fabs(length - 1) < 1e-12);
            if (i == 2 || i == 6) CHECK(std::fabs(length - sp) < 1e-12);
            // The model origin always lands on the ground pivot.
            const Vec2 origin = project(profile, guide, {0, 0, 0});
            CHECK(origin.x == profile.ground_pivot_pixels[0]);
            CHECK(origin.y == profile.ground_pivot_pixels[1]);
            // An orthonormal camera basis.
            CHECK(std::fabs(norm(guide.right) - 1) < 1e-12);
            CHECK(std::fabs(norm(guide.up) - 1) < 1e-12);
            CHECK(std::fabs(norm(guide.toward) - 1) < 1e-12);
            CHECK(std::fabs(dot(guide.right, guide.up)) < 1e-12);
            CHECK(std::fabs(dot(guide.right, guide.toward)) < 1e-12);
            CHECK(std::fabs(dot(guide.up, guide.toward)) < 1e-12);
        }
    }
}

TEST_CASE("camera: other model axes give the same image") {
    // The same character described in Blender axes (forward -Y, up +Z) and in
    // glTF axes (forward +Z, up +Y) projects to the same pixels.
    const Profile blender = load_profile(kSource + "/profiles/quarter-view.json");
    const Profile gltf = load_profile(kSource + "/profiles/quarter-view-gltf.json");
    const AxisFrame frame = axis_frame(gltf);
    // Blender (x, y, z) is glTF (x, z, -y).
    const Vec3 p_blender{0.3, -0.2, 1.0};
    const Vec3 p_gltf = frame.to_model(p_blender);
    CHECK(p_gltf.x == 0.3);
    CHECK(p_gltf.y == 1.0);
    CHECK(p_gltf.z == 0.2);
    CHECK(frame.to_canonical(p_gltf).y == -0.2);
    for (int i = 0; i < 8; ++i) {
        const Vec2 a = project(blender, camera_guide(blender, i), p_blender);
        const Vec2 b = project(gltf, camera_guide(gltf, i), p_gltf);
        CHECK(std::fabs(a.x - b.x) < 1e-9);
        CHECK(std::fabs(a.y - b.y) < 1e-9);
    }
}

TEST_CASE("camera: heading names") {
    CHECK(heading_index("e") == 0);
    CHECK(heading_index("se") == 7);
    CHECK_THROWS_AS(heading_index("north"), std::invalid_argument);
    const Profile profile = load_profile(kSource + "/profiles/quarter-view.json");
    CHECK_THROWS_AS(camera_guide(profile, 8), std::invalid_argument);
}
