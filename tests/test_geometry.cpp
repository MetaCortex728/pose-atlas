// SPDX-License-Identifier: Apache-2.0
// Geometry of rendered views against the profile: the rig's pivot marker
// lands on the ground pivot, each view faces its screen heading (the
// prototype's projected_heading check, here measured on the rendered image),
// and nothing is clipped at the cell edges.
#include <doctest.h>

#include <cmath>
#include <string>

#include "pose_atlas/camera.hpp"
#include "pose_atlas/render.hpp"
#include "pose_atlas/sampling.hpp"
#include "render_support.hpp"

using namespace pose_atlas;
using pose_atlas_test::gltf_profile;
using pose_atlas_test::renderer;
using pose_atlas_test::rig;

namespace {

// Alpha-weighted centroid in pixel coordinates (pixel i centred at i + 0.5).
Vec2 centroid(const Image& image) {
    double sx = 0, sy = 0, sw = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const double a = image.pixel(x, y)[3];
            sx += a * (x + 0.5);
            sy += a * (y + 0.5);
            sw += a;
        }
    }
    REQUIRE(sw > 0);
    return {sx / sw, sy / sw};
}

// The prototype camera, and the same camera with another cell and pivot.
std::vector<Profile> profiles() {
    Profile other = gltf_profile();
    other.id = "other-cell";
    other.cell_pixels = 192;
    other.ground_pivot_pixels = {90, 150};
    other.meters_per_pixel = 0.0125;
    other.supersampling = 3;
    REQUIRE(validate_profile(other).empty());
    return {gltf_profile(), other};
}

RenderSettings only(const char* material) {
    RenderSettings s;
    s.only_materials = {material};
    return s;
}

}  // namespace

TEST_CASE("geometry: the pivot marker lands on the ground pivot in every view") {
    for (const Profile& profile : profiles()) {
        CAPTURE(profile.id);
        // The rest pose and a running pose: the pivot joint is not animated.
        const Model& model = rig();
        for (const Pose& pose : {evaluate(model, nullptr, 0.0), evaluate(model, &model.animation("run"), 0.3)}) {
            for (int view = 0; view < 8; ++view) {
                CAPTURE(view);
                const CameraGuide guide = camera_guide(profile, view);
                const Vec2 c = centroid(renderer().render(model, pose, profile, guide, only("pivot_marker")));
                // The marker is a 0.12 x 0.01 x 0.12 m box centred on the
                // origin on the ground; its silhouette's centroid is the
                // projection of its centre, 5 mm above the ground.
                const Vec2 centre = project(profile, guide, {0.0, 0.005, 0.0});
                const Vec2 origin = project(profile, guide, {0.0, 0.0, 0.0});
                const double x = c.x - (centre.x - origin.x);
                const double y = c.y - (centre.y - origin.y);
                CHECK(std::fabs(x - profile.ground_pivot_pixels[0]) < 0.5);
                CHECK(std::fabs(y - profile.ground_pivot_pixels[1]) < 0.5);
                // Much tighter in practice; keep a record of the actual error.
                CHECK(std::hypot(x - profile.ground_pivot_pixels[0], y - profile.ground_pivot_pixels[1]) < 0.1);
            }
        }
    }
}

TEST_CASE("geometry: each view faces its screen heading") {
    for (const Profile& profile : profiles()) {
        CAPTURE(profile.id);
        const Pose rest = evaluate(rig(), nullptr, 0.0);
        for (int view = 0; view < 8; ++view) {
            CAPTURE(view);
            const CameraGuide guide = camera_guide(profile, view);
            const double angle = view * kPi / 4;
            // Analytic: the prototype's projected_heading.
            const Vec2 forward = projected_forward(profile, guide);
            const double length = std::hypot(forward.x, forward.y);
            CHECK(std::hypot(forward.x / length - std::cos(angle), forward.y / length - std::sin(angle)) < 1e-5);
            // Rendered: the forward tick sits in front of the marker. Screen Y
            // points down, so flip it.
            const Vec2 marker = centroid(renderer().render(rig(), rest, profile, guide, only("pivot_marker")));
            const Vec2 tick = centroid(renderer().render(rig(), rest, profile, guide, only("pivot_forward_tick")));
            const double dx = tick.x - marker.x;
            const double dy = -(tick.y - marker.y);
            const double measured = std::atan2(dy, dx);
            double error = std::remainder(measured - angle, 2 * kPi);
            CHECK(std::fabs(degrees(error)) < 3.0);
            // And the whole figure's nose is on the forward side of the head.
            CHECK(std::hypot(dx, dy) > 3.0);
        }
    }
}

TEST_CASE("geometry: nothing is clipped at the cell edges") {
    for (const Profile& profile : profiles()) {
        CAPTURE(profile.id);
        const Model& model = rig();
        for (const char* action : {"idle", "run"}) {
            SamplingSpec spec;
            spec.action = action;
            spec.pose_count = 8;
            const SamplingPlan plan = plan_sampling(model, spec);
            for (const Sample& sample : plan.samples) {
                const Pose pose = evaluate(model, &model.animation(action), sample.source_time_s);
                for (int view = 0; view < 8; ++view) {
                    CAPTURE(action);
                    CAPTURE(sample.pose_index);
                    CAPTURE(view);
                    const Image cell = renderer().render(model, pose, profile, camera_guide(profile, view), {});
                    const Bounds b = alpha_bounds(cell);
                    REQUIRE_FALSE(b.empty());
                    // A margin of a pixel on every side: no alpha in the
                    // outermost rows and columns.
                    CHECK(b.x0 > 0);
                    CHECK(b.y0 > 0);
                    CHECK(b.x1 < cell.width);
                    CHECK(b.y1 < cell.height);
                }
            }
        }
    }
}
