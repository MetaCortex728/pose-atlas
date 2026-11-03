// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "pose_atlas/model.hpp"
#include "pose_atlas/test_rig.hpp"

using namespace pose_atlas;
namespace rig = pose_atlas::test_rig;

namespace {

const Model& rig_model() {
    static const Model model = load_gltf(rig::make_glb(), "test_rig.glb");
    return model;
}

Vec3 rotate(const Vec3& v, const Vec3& axis, double angle) {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1 - c));
}

double interpolate(const std::vector<double>& times, const std::vector<double>& values, double t) {
    if (t <= times.front()) return values.front();
    for (std::size_t k = 1; k < times.size(); ++k) {
        if (t <= times[k]) {
            const double s = (t - times[k - 1]) / (times[k] - times[k - 1]);
            return values[k - 1] + (values[k] - values[k - 1]) * s;
        }
    }
    return values.back();
}

// Independent forward kinematics straight from the rig description: a point
// given in the local frame of `joint`, carried to model space at time t.
Vec3 expected_point(const rig::Animation& animation, double t, int joint, Vec3 p) {
    const auto& d = rig::description();
    for (int j = joint; j >= 0; j = d.joints[static_cast<std::size_t>(j)].parent) {
        Vec3 translation = d.joints[static_cast<std::size_t>(j)].translation;
        for (const auto& track : animation.translations) {
            if (track.joint != j) continue;
            std::vector<double> xs, ys, zs;
            for (const Vec3& v : track.values) {
                xs.push_back(v.x);
                ys.push_back(v.y);
                zs.push_back(v.z);
            }
            translation = {interpolate(track.times, xs, t), interpolate(track.times, ys, t),
                           interpolate(track.times, zs, t)};
        }
        for (const auto& track : animation.rotations) {
            if (track.joint == j) p = rotate(p, track.axis, radians(interpolate(track.times, track.angles_degrees, t)));
        }
        p = translation + p;
    }
    return p;
}

bool near(const Vec3& a, const Vec3& b, double tolerance) { return norm(a - b) <= tolerance; }

}  // namespace

TEST_CASE("model: loads the synthetic rig's nodes, meshes, skin and animations") {
    const Model& model = rig_model();
    const auto& d = rig::description();
    REQUIRE(model.nodes.size() == d.joints.size() + 1);
    for (std::size_t j = 0; j < d.joints.size(); ++j) {
        CHECK(model.nodes[j].name == d.joints[j].name);
        CHECK(model.nodes[j].parent == d.joints[j].parent);
    }
    const int figure = model.find_node("figure");
    REQUIRE(figure == static_cast<int>(d.joints.size()));
    CHECK(model.nodes[static_cast<std::size_t>(figure)].skin == 0);
    REQUIRE(model.skins.size() == 1);
    CHECK(model.skins[0].joints.size() == 8);
    CHECK(model.skins[0].skeleton == 0);
    REQUIRE(model.meshes.size() == 1);
    REQUIRE(model.meshes[0].primitives.size() == d.parts.size());
    const Primitive& nose = model.meshes[0].primitives[6];
    CHECK(nose.material == "nose");
    CHECK(nose.base_colour[0] == 0.85f);
    CHECK(nose.positions.size() == 24);
    CHECK(nose.indices.size() == 36);
    CHECK(nose.joints.size() == 24);

    REQUIRE(model.animations.size() == 2);
    CHECK(model.animation("idle").start_s == 0.0);
    CHECK(model.animation("idle").end_s == doctest::Approx(2.0));
    CHECK(model.animation("run").end_s == doctest::Approx(0.8));
    CHECK(model.animation("run").channels.size() == 6);
    CHECK_THROWS_WITH_AS(model.animation("walk"), "test_rig.glb: no animation \"walk\" (available: \"idle\", \"run\")",
                         ModelError);
}

TEST_CASE("model: joint transforms at known times") {
    const Model& model = rig_model();
    const Animation& run = model.animation("run");
    const Pose pose = evaluate(model, &run, 0.1);
    // Halfway between the first two keys: hips at 0.925 m, left leg swung
    // -17.5 degrees about +X, torso turned 5 degrees about +Y.
    CHECK(near(pose.global[1].translation(), {0.0, 0.925, 0.0}, 1e-6));
    CHECK(near(transform_point(pose.global[6], {0.0, -0.95, 0.18}), {0.11, 0.07309594069995362, 0.45733956066374026},
               1e-6));
    CHECK(near(pose.global[4].translation(), {0.2490486745229364, 1.545, -0.02178893568691454}, 1e-6));

    const Pose idle = evaluate(model, &model.animation("idle"), 0.25);
    // Head turned 7.5 degrees: the nose tip moves towards +X.
    const Vec3 nose = transform_point(idle.global[3], {0.0, 0.125, 0.16});
    CHECK(nose.x == doctest::Approx(0.16 * std::sin(radians(7.5))).epsilon(1e-5));
}

TEST_CASE("model: every joint follows the rig description at many times") {
    const Model& model = rig_model();
    const auto& d = rig::description();
    for (const auto& described : d.animations) {
        const Animation& animation = model.animation(described.name);
        for (int step = 0; step <= 40; ++step) {
            const double t = described.duration * step / 40.0;
            const Pose pose = evaluate(model, &animation, t);
            for (std::size_t j = 0; j < d.joints.size(); ++j) {
                for (const Vec3& p : {Vec3{0, 0, 0}, Vec3{0.1, -0.3, 0.05}, Vec3{-0.2, 0.1, 0.3}}) {
                    const Vec3 expected = expected_point(described, t, static_cast<int>(j), p);
                    const Vec3 actual = transform_point(pose.global[j], p);
                    CAPTURE(described.name);
                    CAPTURE(t);
                    CAPTURE(j);
                    CHECK(norm(actual - expected) < 2e-6);
                }
            }
        }
    }
}

TEST_CASE("model: skinning") {
    const Model& model = rig_model();
    const auto& d = rig::description();
    const int figure = model.find_node("figure");

    // Rest pose: skinned vertices are the bind-pose vertices.
    const Pose rest = evaluate(model, nullptr, 0.0);
    for (int p = 0; p < static_cast<int>(d.parts.size()); ++p) {
        const PosedPrimitive posed = pose_primitive(model, figure, p, rest);
        const Primitive& bind = model.meshes[0].primitives[static_cast<std::size_t>(p)];
        for (std::size_t v = 0; v < bind.positions.size(); ++v) CHECK(near(posed.positions[v], bind.positions[v], 1e-6));  // float32 data
    }

    const rig::Animation& described = d.animations[1];  // run
    const Animation& run = model.animation("run");
    const double t = 0.05;
    const Pose pose = evaluate(model, &run, t);
    // Rigid part: every nose vertex is the head transform of its local point.
    const Primitive& nose_bind = model.meshes[0].primitives[6];
    const PosedPrimitive nose = pose_primitive(model, figure, 6, pose);
    const Vec3 head_rest{0.0, 1.65, 0.0};
    for (std::size_t v = 0; v < nose_bind.positions.size(); ++v) {
        CHECK(near(nose.positions[v], expected_point(described, t, 3, nose_bind.positions[v] - head_rest), 2e-6));
    }
    // Blended part: the torso's lowest vertices are half hips, half torso.
    const Primitive& torso_bind = model.meshes[0].primitives[3];
    const PosedPrimitive torso = pose_primitive(model, figure, 3, pose);
    const Vec3 hips_rest{0.0, 0.95, 0.0};
    const Vec3 torso_rest{0.0, 1.05, 0.0};
    int blended = 0;
    for (std::size_t v = 0; v < torso_bind.positions.size(); ++v) {
        const Vec3 bind = torso_bind.positions[v];
        const Vec3 by_torso = expected_point(described, t, 2, bind - torso_rest);
        if (torso_bind.weights[v][1] == 0.5f) {
            ++blended;
            const Vec3 by_hips = expected_point(described, t, 1, bind - hips_rest);
            CHECK(near(torso.positions[v], (by_torso + by_hips) * 0.5, 2e-6));
            CHECK(!near(by_torso, by_hips, 1e-3));  // the blend matters at this time
        } else {
            CHECK(near(torso.positions[v], by_torso, 2e-6));
        }
        CHECK(std::fabs(norm(torso.normals[v]) - 1.0) < 1e-9);
    }
    CHECK(blended == 12);  // 4 on the bottom face, 2 on each of the 4 sides
}

TEST_CASE("model: loop closure and wrapping") {
    const Model& model = rig_model();
    for (const char* name : {"idle", "run"}) {
        const Animation& animation = model.animation(name);
        const Pose start = evaluate(model, &animation, animation.start_s);
        const Pose end = evaluate(model, &animation, animation.end_s);
        for (std::size_t i = 0; i < start.global.size(); ++i) {
            for (std::size_t k = 0; k < 16; ++k) CHECK(std::fabs(start.global[i].m[k] - end.global[i].m[k]) < 1e-6);
        }
        // Loop mode wraps times outside the range; clamp mode holds the ends.
        const double d = animation.duration_s();
        const Pose inside = evaluate(model, &animation, 0.3 * d);
        const Pose wrapped = evaluate(model, &animation, 2.3 * d, TimeMode::Loop);
        const Pose negative = evaluate(model, &animation, -0.7 * d, TimeMode::Loop);
        const Pose clamped = evaluate(model, &animation, 2.3 * d, TimeMode::Clamp);
        for (std::size_t i = 0; i < inside.global.size(); ++i) {
            CHECK(near(inside.global[i].translation(), wrapped.global[i].translation(), 1e-6));
            CHECK(near(inside.global[i].translation(), negative.global[i].translation(), 1e-6));
            CHECK(near(end.global[i].translation(), clamped.global[i].translation(), 1e-9));
        }
    }
}

TEST_CASE("model: step and cubic-spline interpolation") {
    Model model;
    model.source = "hand-built";
    model.nodes.resize(1);
    Animation animation;
    animation.name = "a";
    animation.end_s = 1.0;
    Channel step;
    step.path = ChannelPath::Translation;
    step.interpolation = Interpolation::Step;
    step.times = {0.0, 0.5, 1.0};
    step.values = {0, 0, 0, 1, 0, 0, 2, 0, 0};
    animation.channels.push_back(step);
    model.animations.push_back(animation);
    CHECK(evaluate(model, &model.animations[0], 0.49).global[0].translation().x == 0.0);
    CHECK(evaluate(model, &model.animations[0], 0.5).global[0].translation().x == 1.0);
    CHECK(evaluate(model, &model.animations[0], 0.99).global[0].translation().x == 1.0);

    // Cubic spline from 0 to 1 with zero tangents: smoothstep.
    Channel cubic;
    cubic.path = ChannelPath::Translation;
    cubic.interpolation = Interpolation::CubicSpline;
    cubic.times = {0.0, 1.0};
    cubic.values = {0, 0, 0, 0, 0, 0, 0, 0, 0, /* key 1 */ 0, 0, 0, 0, 1, 0, 0, 0, 0};
    model.animations[0].channels = {cubic};
    CHECK(evaluate(model, &model.animations[0], 0.25).global[0].translation().y ==
          doctest::Approx(3 * 0.0625 - 2 * 0.015625));
}

TEST_CASE("model: loading from a file and loading errors") {
    const std::string path = std::string(POSE_ATLAS_TEST_OUTPUT_DIR) + "/model_test_rig.glb";
    rig::write_glb(path);
    const Model model = load_gltf(path);
    CHECK(model.source == path);
    CHECK(model.animations.size() == 2);

    CHECK_THROWS_WITH_AS(load_gltf("/nonexistent/rig.glb"), "/nonexistent/rig.glb: cannot parse: file not found",
                         ModelError);
    const std::vector<std::uint8_t> garbage = {'n', 'o', 't', ' ', 'g', 'l', 't', 'f'};
    CHECK_THROWS_WITH_AS(load_gltf(garbage, "garbage.bin"), doctest::Contains("garbage.bin: cannot parse"), ModelError);
    std::vector<std::uint8_t> truncated = rig::make_glb();
    truncated.resize(truncated.size() / 2);
    CHECK_THROWS_AS(load_gltf(truncated, "truncated.glb"), ModelError);
}
