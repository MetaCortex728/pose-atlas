// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include <cmath>
#include <string>

#include "pose_atlas/camera.hpp"
#include "pose_atlas/image.hpp"
#include "pose_atlas/render.hpp"
#include "pose_atlas/resample.hpp"
#include "render_support.hpp"

using namespace pose_atlas;
using pose_atlas_test::gltf_profile;
using pose_atlas_test::renderer;
using pose_atlas_test::rig;

TEST_CASE("resample: kernels") {
    // Mitchell-Netravali B = C = 1/3: k(0) = 8/9, k(1) = 1/18, k(2) = 0.
    CHECK(filter_weight(ResampleFilter::Mitchell, 0.0) == doctest::Approx(8.0 / 9.0));
    CHECK(filter_weight(ResampleFilter::Mitchell, 1.0) == doctest::Approx(1.0 / 18.0));
    CHECK(filter_weight(ResampleFilter::Mitchell, -2.0) == 0.0);
    CHECK(filter_weight(ResampleFilter::Mitchell, 1.5) < 0.0);  // negative lobe
    CHECK(filter_weight(ResampleFilter::Lanczos3, 1.0) == doctest::Approx(0.0));
    CHECK(filter_weight(ResampleFilter::Triangle, 0.25) == doctest::Approx(0.75));
    // Integer-spaced samples of each kernel sum to one (partition of unity).
    for (ResampleFilter f : {ResampleFilter::Triangle, ResampleFilter::Mitchell}) {
        for (double shift : {0.0, 0.125, 0.4}) {
            double sum = 0.0;
            for (int k = -4; k <= 4; ++k) sum += filter_weight(f, k + shift);
            CHECK(sum == doctest::Approx(1.0));
        }
    }
    CHECK(linear_to_srgb(0.0) == 0.0);
    CHECK(linear_to_srgb(1.0) == doctest::Approx(1.0));
    CHECK(linear_to_srgb(0.5) == doctest::Approx(0.735356983));
}

TEST_CASE("resample: premultiplied downsampling") {
    // A 16x16 source, factor 4: an opaque red 8x8 square in the middle on
    // transparent black.
    LinearImage source;
    source.width = source.height = 16;
    source.rgba.assign(16 * 16 * 4, 0.0f);
    for (int y = 4; y < 12; ++y) {
        for (int x = 4; x < 12; ++x) {
            float* p = source.rgba.data() + (y * 16 + x) * 4;
            p[0] = 1.0f;
            p[3] = 1.0f;
        }
    }
    for (ResampleFilter f : {ResampleFilter::Box, ResampleFilter::Triangle, ResampleFilter::Mitchell,
                             ResampleFilter::Lanczos3}) {
        const Image out = downsample(source, 4, f);
        REQUIRE(out.width == 4);
        // Premultiplied filtering keeps colour where there is coverage: no
        // dark fringe from the transparent black around the square.
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const std::uint8_t* p = out.pixel(x, y);
                if (p[3] == 0) {
                    CHECK(p[0] == 0);
                } else {
                    CHECK(p[0] == 255);
                    CHECK(p[1] == 0);
                    CHECK(p[2] == 0);
                }
            }
        }
        // Symmetric input, symmetric output.
        CHECK(out.pixel(1, 1)[3] == out.pixel(2, 2)[3]);
        CHECK(out.pixel(0, 1)[3] == out.pixel(3, 2)[3]);
    }
    const Image box = downsample(source, 4, ResampleFilter::Box);
    CHECK(box.pixel(1, 1)[3] == 255);
    CHECK(box.pixel(0, 0)[3] == 0);
    CHECK_THROWS(downsample(source, 3, ResampleFilter::Box));
}

TEST_CASE("resample: outline") {
    Image image(5, 5);
    image.pixel(2, 2)[3] = 200;
    image.pixel(4, 4)[3] = 60;  // faint fringe away from the figure
    apply_outline(image, {1, 2, 3});
    CHECK(image.pixel(2, 2)[3] == 255);
    CHECK(image.pixel(4, 4)[3] == 0);
    CHECK(image.pixel(2, 1)[3] == 255);
    CHECK(image.pixel(2, 1)[2] == 3);
    CHECK(image.pixel(1, 2)[3] == 255);
    CHECK(image.pixel(1, 1)[3] == 0);  // diagonal neighbours stay empty
    CHECK(image.pixel(2, 2)[0] == 0);  // the figure itself is unchanged
}

TEST_CASE("image: PNG round trip and SHA-256") {
    Image image(3, 2);
    for (std::size_t i = 0; i < image.rgba.size(); ++i) image.rgba[i] = static_cast<std::uint8_t>(i * 13);
    const auto png = encode_png(image);
    CHECK(decode_png(png) == image);
    CHECK(encode_png(image) == png);
    CHECK_THROWS_AS(decode_png({1, 2, 3}), ImageError);
    CHECK(sha256_hex(std::string()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex(std::string("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    const std::vector<std::uint8_t> data(70000, 7);
    const auto z = zlib_compress(data.data(), data.size());
    CHECK(z.size() < data.size());
    CHECK(zlib_decompress(z.data(), z.size(), data.size()) == data);
}

TEST_CASE("render: flat colours of the rest pose") {
    const Profile& profile = gltf_profile();
    const Pose rest = evaluate(rig(), nullptr, 0.0);
    const Image cell = renderer().render(rig(), rest, profile, camera_guide(profile, "s"), {});
    REQUIRE(cell.width == profile.cell_pixels);
    REQUIRE(cell.height == profile.cell_pixels);
    const Bounds b = alpha_bounds(cell);
    REQUIRE_FALSE(b.empty());
    // About 1.9 m tall at 0.0108 m/px, foreshortened by cos(30 deg): ~150 px.
    CHECK(b.y1 - b.y0 > 130);
    CHECK(b.y1 - b.y0 < 190);
    // The head's skin colour (linear 0.90, 0.70, 0.55) appears unlit.
    const int r = static_cast<int>(std::lround(linear_to_srgb(0.90) * 255));
    const int g = static_cast<int>(std::lround(linear_to_srgb(0.70) * 255));
    int head_pixels = 0;
    for (int y = 0; y < cell.height; ++y) {
        for (int x = 0; x < cell.width; ++x) {
            const std::uint8_t* p = cell.pixel(x, y);
            if (p[3] == 255 && p[0] == r && p[1] == g) ++head_pixels;
        }
    }
    CHECK(head_pixels > 100);
    CHECK_FALSE(renderer().description().empty());
}

TEST_CASE("render: deterministic, byte-identical PNG output") {
    const Profile& profile = gltf_profile();
    const Model& model = rig();
    const Pose pose = evaluate(model, &model.animation("run"), 0.3);
    const CameraGuide guide = camera_guide(profile, "ne");
    const auto first = encode_png(renderer().render(model, pose, profile, guide, {}));
    const auto second = encode_png(renderer().render(model, pose, profile, guide, {}));
    CHECK(first == second);
    // A fresh renderer (new EGL context) gives the same bytes.
    Renderer other;
    CHECK(encode_png(other.render(model, pose, profile, guide, {})) == first);
    // Another pose differs.
    const Pose later = evaluate(model, &model.animation("run"), 0.5);
    CHECK(encode_png(renderer().render(model, later, profile, guide, {})) != first);
}

TEST_CASE("render: outline and material selection") {
    const Profile& profile = gltf_profile();
    const Pose rest = evaluate(rig(), nullptr, 0.0);
    const CameraGuide guide = camera_guide(profile, "e");
    RenderSettings outlined;
    outlined.style.outline = true;
    Image solid = renderer().render(rig(), rest, profile, guide, {});
    for (std::size_t i = 3; i < solid.rgba.size(); i += 4) solid.rgba[i] = solid.rgba[i] >= 128 ? 255 : 0;
    const Image outline = renderer().render(rig(), rest, profile, guide, outlined);
    const Bounds a = alpha_bounds(solid), b = alpha_bounds(outline);
    CHECK(b.x0 == a.x0 - 1);
    CHECK(b.y0 == a.y0 - 1);
    CHECK(b.x1 == a.x1 + 1);
    CHECK(b.y1 == a.y1 + 1);
    for (std::size_t i = 3; i < outline.rgba.size(); i += 4) {
        CHECK((outline.rgba[i] == 0 || outline.rgba[i] == 255));
    }
    CHECK(render_settings_json(outlined) != render_settings_json({}));

    RenderSettings marker;
    marker.only_materials = {"pivot_marker"};
    const Bounds m = alpha_bounds(renderer().render(rig(), rest, profile, guide, marker));
    CHECK(m.x1 - m.x0 < 20);
    CHECK(m.y1 - m.y0 < 12);
}
