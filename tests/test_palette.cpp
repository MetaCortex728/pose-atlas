// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "pose_atlas/image.hpp"
#include "pose_atlas/palette.hpp"
#include "pose_atlas/render.hpp"

using namespace pose_atlas;

namespace {

std::vector<std::uint8_t> bytes_of(const std::string& text) { return {text.begin(), text.end()}; }

std::string json_palette(std::size_t count) {
    std::string text = "{\"colors\": [";
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) text += ", ";
        text += "\"" + hex_colour({static_cast<std::uint8_t>(i % 256), static_cast<std::uint8_t>(i / 256), 7}) + "\"";
    }
    return text + "]}";
}

}  // namespace

TEST_CASE("palette: JSON and PNG input") {
    const Palette json = parse_palette(bytes_of(R"({"colors": ["#1A141E", "#ffffff", "#c03030"]})"), "p.json");
    REQUIRE(json.colours.size() == 3);
    CHECK(json.colours[0] == Rgb8{0x1a, 0x14, 0x1e});
    CHECK(json.colours[2] == Rgb8{0xc0, 0x30, 0x30});
    CHECK(hex_colour(json.colours[0]) == "#1a141e");
    CHECK(palette_json(json.colours) == R"({"colors":["#1a141e","#ffffff","#c03030"]})");

    // A PNG: distinct opaque pixels in row order; transparent and partly
    // transparent pixels are not colours, repeats count once.
    Image swatches(4, 2);
    const std::uint8_t pixels[8][4] = {{0x1a, 0x14, 0x1e, 255}, {255, 255, 255, 255}, {0x1a, 0x14, 0x1e, 255},
                                       {9, 9, 9, 0},            {1, 2, 3, 128},        {0xc0, 0x30, 0x30, 255},
                                       {255, 255, 255, 255},    {0, 0, 0, 0}};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t c = 0; c < 4; ++c) swatches.rgba[i * 4 + c] = pixels[i][c];
    }
    const Palette png = parse_palette(encode_png(swatches), "p.png");
    CHECK(png.colours == json.colours);
    // Same colours, same content hash, whatever the file format.
    CHECK(palette_sha256(png.colours) == palette_sha256(json.colours));
    CHECK(palette_sha256(png.colours).size() == 64);
    std::vector<Rgb8> reordered = {json.colours[1], json.colours[0], json.colours[2]};
    CHECK(palette_sha256(reordered) != palette_sha256(json.colours));

    Rgb8 c{};
    CHECK(parse_hex_colour("#A0b1C2", c));
    CHECK(c == Rgb8{0xa0, 0xb1, 0xc2});
    CHECK_FALSE(parse_hex_colour("a0b1c2", c));
    CHECK_FALSE(parse_hex_colour("#a0b1c", c));
    CHECK_FALSE(parse_hex_colour("#a0b1cg", c));
}

TEST_CASE("palette: validation") {
    CHECK(parse_palette(bytes_of(json_palette(2))).colours.size() == 2);
    CHECK(parse_palette(bytes_of(json_palette(256))).colours.size() == 256);
    CHECK_THROWS_WITH_AS(parse_palette(bytes_of(json_palette(1)), "one.json"),
                         "one.json: a palette needs 2 to 256 colours, found 1", PaletteError);
    CHECK_THROWS_WITH_AS(parse_palette(bytes_of(json_palette(257)), "many.json"),
                         "many.json: a palette needs 2 to 256 colours, found 257", PaletteError);
    CHECK_THROWS_WITH_AS(parse_palette(bytes_of(R"({"colors": ["#000000", "#00000"]})"), "p"),
                         "p: colors[1] is not \"#rrggbb\"", PaletteError);
    CHECK_THROWS_WITH_AS(parse_palette(bytes_of(R"({"colors": ["#000000", "#FFFFFF", "#ffffff"]})"), "p"),
                         "p: colors[2] repeats #ffffff", PaletteError);
    CHECK_THROWS_AS(parse_palette(bytes_of(R"({"colours": ["#000000", "#ffffff"]})")), PaletteError);
    CHECK_THROWS_AS(parse_palette(bytes_of("not json")), PaletteError);
    CHECK_THROWS_AS(load_palette("/nonexistent/palette.json"), PaletteError);
    // A PNG with too many colours (a gradient) or a single colour.
    Image gradient(32, 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 32; ++x) {
            std::uint8_t* p = gradient.pixel(x, y);
            p[0] = static_cast<std::uint8_t>(x * 8);
            p[1] = static_cast<std::uint8_t>(y * 16);
            p[3] = 255;
        }
    }
    CHECK_THROWS_WITH_AS(parse_palette(encode_png(gradient), "g.png"), doctest::Contains("found 257"), PaletteError);
    Image flat(2, 2);
    for (std::size_t i = 3; i < flat.rgba.size(); i += 4) flat.rgba[i] = 255;
    CHECK_THROWS_WITH_AS(parse_palette(encode_png(flat), "f.png"), doctest::Contains("found 1"), PaletteError);

    // Render settings: the palette joins the settings hash; bad palettes are
    // reported.
    RenderSettings settings;
    CHECK(validate_render_settings(settings).empty());
    settings.palette = {{0, 0, 0}, {255, 255, 255}};
    CHECK_FALSE(validate_render_settings(settings).empty());  // no stage
    settings.palette_stage = PaletteStage::BeforeAfter;
    CHECK(validate_render_settings(settings).empty());
    CHECK(render_settings_json(settings).find(palette_sha256(settings.palette)) != std::string::npos);
    CHECK(render_settings_json(settings) != render_settings_json({}));
    settings.palette.push_back({0, 0, 0});
    CHECK_FALSE(validate_render_settings(settings).empty());
    settings.palette = {{1, 2, 3}};
    CHECK_FALSE(validate_render_settings(settings).empty());
    settings.palette = {{0, 0, 0}, {255, 255, 255}};
    for (double bad : {0.0, -0.5, 1.5}) {
        settings.alpha_threshold = bad;
        CHECK_FALSE(validate_render_settings(settings).empty());
    }
    settings.alpha_threshold = 1.0;
    CHECK(validate_render_settings(settings).empty());
    RenderSettings stage_only;
    stage_only.palette_stage = PaletteStage::After;
    CHECK_FALSE(validate_render_settings(stage_only).empty());
}

TEST_CASE("palette: Oklab nearest colour") {
    const Oklab white = linear_to_oklab(1, 1, 1);
    CHECK(white.L == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(white.a == doctest::Approx(0.0).scale(1.0).epsilon(1e-6));
    CHECK(white.b == doctest::Approx(0.0).scale(1.0).epsilon(1e-6));
    // Ottosson's reference value for linear sRGB red.
    const Oklab red = linear_to_oklab(1, 0, 0);
    CHECK(red.L == doctest::Approx(0.627955).epsilon(1e-4));
    CHECK(red.a == doctest::Approx(0.224863).epsilon(1e-4));
    CHECK(red.b == doctest::Approx(0.125846).epsilon(1e-4));

    PaletteMatcher matcher({{0, 0, 0}, {255, 255, 255}, {200, 30, 30}});
    CHECK(matcher.contains({200, 30, 30}));
    CHECK_FALSE(matcher.contains({200, 30, 31}));
    CHECK(matcher.nearest_srgb({200, 30, 30}) == 2);
    CHECK(matcher.nearest_srgb({180, 50, 40}) == 2);
    // Mid grey (sRGB 128, linear 0.216) is perceptually nearer white than
    // black; in linear RGB it would be nearer black.
    PaletteMatcher grey({{0, 0, 0}, {255, 255, 255}});
    CHECK(grey.nearest_srgb({128, 128, 128}) == 1);
    CHECK(grey.nearest_linear(0.216f, 0.216f, 0.216f) == 1);
    CHECK(grey.nearest_srgb({90, 90, 90}) == 0);
    // Ties go to the first colour.
    PaletteMatcher twins({{10, 10, 10}, {10, 10, 10}});
    CHECK(twins.nearest_srgb({0, 0, 0}) == 0);
}

TEST_CASE("palette: snapping and hard alpha") {
    PaletteMatcher matcher({{0, 0, 0}, {255, 255, 255}, {200, 30, 30}});
    // Premultiplied linear: a half-covered reddish pixel keeps its alpha and
    // takes the palette red; transparent pixels stay untouched.
    LinearImage linear;
    linear.width = 2;
    linear.height = 1;
    linear.rgba = {0.3f, 0.01f, 0.01f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f};
    snap_linear(linear, matcher);
    CHECK(linear.rgba[0] == doctest::Approx(srgb_to_linear(200 / 255.0) * 0.5));
    CHECK(linear.rgba[1] == doctest::Approx(srgb_to_linear(30 / 255.0) * 0.5));
    CHECK(linear.rgba[3] == 0.5f);
    CHECK(linear.rgba[4] == 0.0f);

    Image image(3, 1);
    const std::uint8_t pixels[] = {190, 40, 20, 255, 240, 250, 255, 127, 9, 9, 9, 0};
    std::copy(std::begin(pixels), std::end(pixels), image.rgba.begin());
    snap_image(image, matcher);
    CHECK(image.pixel(0, 0)[0] == 200);
    CHECK(image.pixel(1, 0)[2] == 255);
    CHECK(image.pixel(2, 0)[0] == 9);  // transparent: untouched by the snap
    threshold_alpha(image, 0.5);
    CHECK(image.pixel(0, 0)[3] == 255);
    // 127/255 is below half coverage: transparent black.
    CHECK(std::vector<std::uint8_t>(image.pixel(1, 0), image.pixel(1, 0) + 4) == std::vector<std::uint8_t>{0, 0, 0, 0});
    CHECK(std::vector<std::uint8_t>(image.pixel(2, 0), image.pixel(2, 0) + 4) == std::vector<std::uint8_t>{0, 0, 0, 0});
    CHECK(alpha_passes(128, 0.5));
    CHECK_FALSE(alpha_passes(127, 0.5));
    CHECK(alpha_passes(255, 1.0));
    CHECK_FALSE(alpha_passes(254, 1.0));

    CHECK(parse_palette_stage("before_after") == PaletteStage::BeforeAfter);
    CHECK(to_string(PaletteStage::After) == "after");
    CHECK_THROWS_AS(parse_palette_stage("during"), std::invalid_argument);
    CHECK(snaps_before(PaletteStage::BeforeAfter));
    CHECK_FALSE(snaps_before(PaletteStage::After));
    CHECK(snaps_after(PaletteStage::After));
    CHECK_FALSE(snaps_after(PaletteStage::Before));
}
