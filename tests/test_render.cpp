// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

#include "pose_atlas/camera.hpp"
#include "pose_atlas/image.hpp"
#include "pose_atlas/palette.hpp"
#include "pose_atlas/render.hpp"
#include "pose_atlas/resample.hpp"
#include "render_support.hpp"

using namespace pose_atlas;
using pose_atlas_test::gltf_profile;
using pose_atlas_test::renderer;
using pose_atlas_test::rig;
namespace test_rig = pose_atlas::test_rig;

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

TEST_CASE("render: the base colour texture lands where its texels map") {
    // The torso's +Z face shows the 2x2 checker times the factor (1, 0.8, 1).
    // Seen from the front ("s"), the centre of each quadrant of the face must
    // carry its texel's colour.
    const Profile& profile = gltf_profile();
    const Pose rest = evaluate(rig(), nullptr, 0.0);
    const CameraGuide guide = camera_guide(profile, "s");
    const auto& d = test_rig::description();
    const test_rig::Part& torso = d.parts[3];
    REQUIRE(torso.name == "torso");
    REQUIRE(torso.texture == 0);
    const Image& texture = d.textures[0];
    const Vec3 origin{0.0, 1.05, 0.0};  // the torso joint at rest
    const Image cell = renderer().render(rig(), rest, profile, guide, {});
    for (int ty = 0; ty < 2; ++ty) {
        for (int tx = 0; tx < 2; ++tx) {
            const double u = 0.25 + 0.5 * tx, v = 0.25 + 0.5 * ty;
            const Vec3 point = origin + Vec3{torso.min.x + u * (torso.max.x - torso.min.x),
                                             torso.max.y - v * (torso.max.y - torso.min.y), torso.max.z};
            const Vec2 at = project(profile, guide, point);
            const int px = static_cast<int>(std::floor(at.x)), py = static_cast<int>(std::floor(at.y));
            const std::uint8_t* texel = texture.pixel(tx, ty);
            int expected[3];
            for (int c = 0; c < 3; ++c) {
                const double linear = srgb_to_linear(texel[c] / 255.0) * torso.colour[static_cast<std::size_t>(c)];
                expected[c] = static_cast<int>(std::lround(linear_to_srgb(linear) * 255));
            }
            CAPTURE(tx);
            CAPTURE(ty);
            // Nearest filtering: the whole neighbourhood is the texel.
            for (int dy = -3; dy <= 3; ++dy) {
                for (int dx = -3; dx <= 3; ++dx) {
                    const std::uint8_t* p = cell.pixel(px + dx, py + dy);
                    CHECK(p[3] == 255);
                    for (int c = 0; c < 3; ++c) CHECK(std::abs(p[c] - expected[c]) <= 1);
                }
            }
        }
    }
    RenderSettings bilinear;
    bilinear.texture_filter = TextureFilter::Bilinear;
    CHECK(render_settings_json(bilinear) != render_settings_json({}));
    // Bilinear filtering blends the texels, so the face looks different.
    CHECK(renderer().render(rig(), rest, profile, guide, bilinear) != cell);
    CHECK(parse_texture_filter("bilinear") == TextureFilter::Bilinear);
    CHECK_THROWS(parse_texture_filter("trilinear"));
}

// A 16x8 baseline JPEG of the solid colour (200, 40, 90), written once by
// stb_image_write (quality 90) for this test; stb's JPEG encoder is not
// sanitizer-clean, so the bytes are embedded rather than encoded here.
const std::uint8_t kSolidJpeg[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x84, 0x00, 0x03, 0x02, 0x02, 0x03, 0x02, 0x02, 0x03,
    0x03, 0x03, 0x03, 0x04, 0x03, 0x03, 0x04, 0x05, 0x08, 0x05, 0x05, 0x04, 0x04, 0x05, 0x0a, 0x07,
    0x07, 0x06, 0x08, 0x0c, 0x0a, 0x0c, 0x0c, 0x0b, 0x0a, 0x0b, 0x0b, 0x0d, 0x0e, 0x12, 0x10, 0x0d,
    0x0e, 0x11, 0x0e, 0x0b, 0x0b, 0x10, 0x16, 0x10, 0x11, 0x13, 0x14, 0x15, 0x15, 0x15, 0x0c, 0x0f,
    0x17, 0x18, 0x16, 0x14, 0x18, 0x12, 0x14, 0x15, 0x14, 0x01, 0x03, 0x04, 0x04, 0x05, 0x04, 0x05,
    0x09, 0x05, 0x05, 0x09, 0x14, 0x0d, 0x0b, 0x0d, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00,
    0x08, 0x00, 0x10, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x01,
    0xa2, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x10, 0x00,
    0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7d, 0x01,
    0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22,
    0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24,
    0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29,
    0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
    0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a,
    0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a,
    0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
    0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6,
    0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3,
    0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
    0xfa, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x11, 0x00,
    0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00,
    0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13,
    0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
    0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
    0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
    0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
    0xfa, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0xf2,
    0x3a, 0x28, 0xa2, 0xb2, 0x3f, 0xb2, 0x8f, 0xff, 0xd9,
};

TEST_CASE("image: JPEG decoding") {
    const std::vector<std::uint8_t> jpeg(std::begin(kSolidJpeg), std::end(kSolidJpeg));
    const Image decoded = decode_image(jpeg, "solid.jpg");
    REQUIRE(decoded.width == 16);
    REQUIRE(decoded.height == 8);
    for (std::size_t i = 0; i < decoded.rgba.size(); i += 4) {
        CHECK(std::abs(int(decoded.rgba[i]) - 200) <= 3);
        CHECK(std::abs(int(decoded.rgba[i + 1]) - 40) <= 3);
        CHECK(std::abs(int(decoded.rgba[i + 2]) - 90) <= 3);
        CHECK(decoded.rgba[i + 3] == 255);
    }
    CHECK_THROWS_AS(decode_png(jpeg), ImageError);
}

TEST_CASE("render: palette-locked cells") {
    const Profile& profile = pose_atlas_test::small_profile();
    const Model& model = rig();
    const Pose pose = evaluate(model, &model.animation("run"), 0.3);
    const CameraGuide guide = camera_guide(profile, "sw");
    // A palette that is not the rig's own colours, so that every stage has
    // to move colours.
    const std::vector<Rgb8> colours = {{0x10, 0x10, 0x18}, {0xf0, 0xf0, 0xe8}, {0xc0, 0x30, 0x30},
                                       {0x30, 0x60, 0xc0}, {0x40, 0xa0, 0x50}, {0xe0, 0xc0, 0x40},
                                       {0x90, 0x60, 0x40}, {0x80, 0x40, 0x90}};
    PaletteMatcher matcher(colours);
    RenderSettings settings;
    settings.palette = colours;
    for (PaletteStage stage : {PaletteStage::Before, PaletteStage::After, PaletteStage::BeforeAfter}) {
        for (bool outline : {false, true}) {
            CAPTURE(to_string(stage));
            CAPTURE(outline);
            settings.palette_stage = stage;
            settings.style.outline = outline;
            const Image cell = renderer().render(model, pose, profile, guide, settings);
            int opaque = 0, outline_pixels = 0, off_palette = 0;
            for (int y = 0; y < cell.height; ++y) {
                for (int x = 0; x < cell.width; ++x) {
                    const std::uint8_t* p = cell.pixel(x, y);
                    REQUIRE((p[3] == 0 || p[3] == 255));
                    if (p[3] == 0) {
                        CHECK((p[0] == 0 && p[1] == 0 && p[2] == 0));
                        continue;
                    }
                    ++opaque;
                    const Rgb8 c{p[0], p[1], p[2]};
                    if (outline && c == settings.style.outline_colour) {
                        ++outline_pixels;
                    } else if (!matcher.contains(c)) {
                        ++off_palette;
                    }
                }
            }
            CHECK(opaque > 500);
            CHECK((outline_pixels > 0) == outline);
            if (snaps_after(stage)) {
                CHECK(off_palette == 0);
            } else {
                CHECK(off_palette > 0);  // before alone: the downsample blends at borders
            }
        }
    }
    // The outline comes after the snap: it rings the thresholded figure.
    settings.palette_stage = PaletteStage::BeforeAfter;
    settings.style.outline = false;
    const Image plain = renderer().render(model, pose, profile, guide, settings);
    settings.style.outline = true;
    const Image ringed = renderer().render(model, pose, profile, guide, settings);
    for (int y = 0; y < plain.height; ++y) {
        for (int x = 0; x < plain.width; ++x) {
            if (plain.pixel(x, y)[3] == 255) {
                CHECK(std::equal(plain.pixel(x, y), plain.pixel(x, y) + 4, ringed.pixel(x, y)));
            }
        }
    }
    // A lower threshold keeps more of the edge.
    settings.style.outline = false;
    settings.alpha_threshold = 0.1;
    const Image generous = renderer().render(model, pose, profile, guide, settings);
    auto count = [](const Image& image) {
        int n = 0;
        for (std::size_t i = 3; i < image.rgba.size(); i += 4) n += image.rgba[i] == 255 ? 1 : 0;
        return n;
    };
    CHECK(count(generous) > count(plain));
    // Invalid settings are refused.
    settings.palette.clear();
    CHECK_THROWS_AS(renderer().render(model, pose, profile, guide, settings), RenderError);
}

TEST_CASE("render: palette-locked output is deterministic") {
    const Profile& profile = gltf_profile();
    const Model& model = rig();
    const Pose pose = evaluate(model, &model.animation("run"), 0.3);
    const CameraGuide guide = camera_guide(profile, "ne");
    RenderSettings settings;
    settings.palette = test_rig::palette();
    settings.style.outline = true;
    for (PaletteStage stage : {PaletteStage::Before, PaletteStage::BeforeAfter}) {
        settings.palette_stage = stage;
        const auto first = encode_png(renderer().render(model, pose, profile, guide, settings));
        CHECK(encode_png(renderer().render(model, pose, profile, guide, settings)) == first);
        Renderer other;
        CHECK(encode_png(other.render(model, pose, profile, guide, settings)) == first);
    }
}
