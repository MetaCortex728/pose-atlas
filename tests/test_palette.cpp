// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

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
    CHECK(validate_render_settings(settings).empty());
    CHECK(render_settings_json(settings).find(palette_sha256(settings.palette)) != std::string::npos);
    CHECK(render_settings_json(settings) != render_settings_json({}));
    settings.palette.push_back({0, 0, 0});
    CHECK_FALSE(validate_render_settings(settings).empty());
    settings.palette = {{1, 2, 3}};
    CHECK_FALSE(validate_render_settings(settings).empty());
}
