// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include "aseprite_parser.hpp"
#include "pose_atlas/aseprite.hpp"
#include "pose_atlas/image.hpp"

using namespace pose_atlas;
using pose_atlas_test::Bytes;
using pose_atlas_test::parse_raw_aseprite;

namespace {

AsepriteDocument sample_document() {
    AsepriteDocument doc;
    doc.width = 4;
    doc.height = 3;
    doc.layers = {"Render", "Manual corrections"};
    for (int f = 0; f < 3; ++f) {
        Image image(4, 3);
        for (std::size_t i = 0; i < image.rgba.size(); ++i) image.rgba[i] = static_cast<std::uint8_t>(i * 7 + static_cast<std::size_t>(f));
        AsepriteFrame frame;
        frame.duration_ms = 100 + f;
        AsepriteCel cel;
        cel.image = image;
        frame.cels.push_back(cel);
        doc.frames.push_back(frame);
    }
    doc.tags = {{"idle.e", 0, 1}, {"run.e", 2, 2}};
    AsepriteSliceKey key;
    key.width = 4;
    key.height = 3;
    key.pivot = std::array<int, 2>{2, 2};
    doc.slices.push_back({"foot", {key}});
    return doc;
}

}  // namespace

TEST_CASE("aseprite: the written file follows the documented layout") {
    const AsepriteDocument doc = sample_document();
    const auto bytes = write_aseprite(doc);
    CHECK(write_aseprite(doc) == bytes);  // deterministic
    const auto raw = parse_raw_aseprite(bytes);
    CHECK(raw.frames == 3);
    CHECK(raw.width == 4);
    CHECK(raw.height == 3);
    CHECK(raw.depth == 32);
    CHECK((raw.flags & 1) == 1);
    REQUIRE(raw.frame_list.size() == 3);
    CHECK(raw.frame_list[0].duration == 100);
    CHECK(raw.frame_list[2].duration == 102);

    // Frame 0: colour profile, palette, two layers, tags, slice, one cel.
    std::vector<std::uint16_t> types;
    for (const auto& c : raw.frame_list[0].chunks) types.push_back(c.type);
    CHECK(types == std::vector<std::uint16_t>{0x2007, 0x2019, 0x2004, 0x2004, 0x2018, 0x2022, 0x2005});
    CHECK(raw.frame_list[1].chunks.size() == 1);

    const auto& chunks = raw.frame_list[0].chunks;
    {
        Bytes profile(chunks[0].data);
        CHECK(profile.u(2) == 1);  // sRGB
    }
    for (int i : {2, 3}) {
        Bytes layer(chunks[static_cast<std::size_t>(i)].data);
        CHECK(layer.u(2) == 3);  // visible, editable
        CHECK(layer.u(2) == 0);  // normal layer
        CHECK(layer.u(2) == 0);  // child level
        layer.skip(4);
        CHECK(layer.u(2) == 0);  // normal blend
        CHECK(layer.u(1) == 255);
        layer.skip(3);
        CHECK(layer.str() == (i == 2 ? "Render" : "Manual corrections"));
    }
    {
        Bytes tags(chunks[4].data);
        CHECK(tags.u(2) == 2);
        tags.skip(8);
        CHECK(tags.u(2) == 0);
        CHECK(tags.u(2) == 1);
        CHECK(tags.u(1) == 0);  // forward
        tags.skip(2 + 6 + 3 + 1);
        CHECK(tags.str() == "idle.e");
        CHECK(tags.u(2) == 2);
        CHECK(tags.u(2) == 2);
        tags.skip(1 + 2 + 6 + 3 + 1);
        CHECK(tags.str() == "run.e");
    }
    {
        Bytes slice(chunks[5].data);
        CHECK(slice.u(4) == 1);  // one key
        CHECK(slice.u(4) == 2);  // has pivot
        slice.skip(4);
        CHECK(slice.str() == "foot");
        CHECK(slice.u(4) == 0);  // frame
        CHECK(slice.s32() == 0);
        CHECK(slice.s32() == 0);
        CHECK(slice.u(4) == 4);
        CHECK(slice.u(4) == 3);
        CHECK(slice.s32() == 2);  // pivot x
        CHECK(slice.s32() == 2);  // pivot y
    }
    for (std::size_t f = 0; f < 3; ++f) {
        const auto& cel_data = raw.frame_list[f].chunks.back().data;
        Bytes cel(cel_data);
        CHECK(cel.u(2) == 0);  // layer
        CHECK(cel.u(2) == 0);  // x
        CHECK(cel.u(2) == 0);  // y
        CHECK(cel.u(1) == 255);
        CHECK(cel.u(2) == 2);  // compressed image
        cel.skip(2 + 5);
        CHECK(cel.u(2) == 4);
        CHECK(cel.u(2) == 3);
        const auto pixels = zlib_decompress(cel_data.data() + cel.at(), cel_data.size() - cel.at(), 48);
        CHECK(pixels == doc.frames[f].cels[0].image.rgba);
    }
}

TEST_CASE("aseprite: the library reader round-trips") {
    const AsepriteDocument doc = sample_document();
    const AsepriteDocument back = read_aseprite(write_aseprite(doc));
    CHECK(back.width == 4);
    CHECK(back.layers == doc.layers);
    REQUIRE(back.frames.size() == 3);
    CHECK(back.frames[1].duration_ms == 101);
    CHECK(back.frames[2].cels[0].image == doc.frames[2].cels[0].image);
    REQUIRE(back.tags.size() == 2);
    CHECK(back.tags[1].name == "run.e");
    REQUIRE(back.slices.size() == 1);
    CHECK(back.slices[0].keys[0].pivot == std::array<int, 2>{2, 2});

    auto bytes = write_aseprite(doc);
    bytes[4] = 0;  // magic
    CHECK_THROWS_AS(read_aseprite(bytes), AsepriteError);
    CHECK_THROWS_AS(read_aseprite(std::vector<std::uint8_t>(10, 0)), AsepriteError);
    AsepriteDocument bad = doc;
    bad.tags[0].to = 9;
    CHECK_THROWS_AS(write_aseprite(bad), AsepriteError);
    bad = doc;
    bad.frames[0].duration_ms = 0;
    CHECK_THROWS_AS(write_aseprite(bad), AsepriteError);
}

namespace {

// File bytes from raw frames (header taken from `header`, sizes recomputed),
// to build files the library's writer does not produce.
std::vector<std::uint8_t> serialize_raw(const std::vector<std::uint8_t>& header, const pose_atlas_test::RawAseprite& raw) {
    std::vector<std::uint8_t> out(header.begin(), header.begin() + 128);
    auto put = [&out](std::uint32_t v, int n) {
        for (int i = 0; i < n; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    for (const auto& frame : raw.frame_list) {
        std::uint32_t size = 16;
        for (const auto& c : frame.chunks) size += 6 + static_cast<std::uint32_t>(c.data.size());
        put(size, 4);
        put(0xF1FA, 2);
        put(static_cast<std::uint32_t>(frame.chunks.size()), 2);
        put(frame.duration, 2);
        put(0, 2);
        put(static_cast<std::uint32_t>(frame.chunks.size()), 4);
        for (const auto& c : frame.chunks) {
            put(6 + static_cast<std::uint32_t>(c.data.size()), 4);
            put(c.type, 2);
            out.insert(out.end(), c.data.begin(), c.data.end());
        }
    }
    const auto total = static_cast<std::uint32_t>(out.size());
    for (int i = 0; i < 4; ++i) out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(total >> (8 * i));
    return out;
}

}  // namespace

TEST_CASE("aseprite: cel user data, opacity and linked cels") {
    AsepriteDocument doc = sample_document();
    doc.frames[0].cels[0].user_text = "idle.e@0.000000";
    AsepriteCel correction;
    correction.layer = 1;
    correction.x = 1;
    correction.y = 2;
    correction.image = Image(2, 1);
    correction.image.pixel(1, 0)[3] = 255;
    correction.user_text = "stale: render changed";
    correction.user_colour = AsepriteColour{255, 102, 0, 255};
    doc.frames[1].cels.push_back(correction);
    doc.frames[2].cels[0].opacity = 128;
    const auto bytes = write_aseprite(doc);

    // The user data chunk follows its cel chunk.
    const auto raw = parse_raw_aseprite(bytes);
    const auto& f0 = raw.frame_list[0].chunks;
    REQUIRE(f0.size() == 8);
    CHECK(f0[6].type == 0x2005);
    CHECK(f0[7].type == 0x2020);
    {
        Bytes data(f0[7].data);
        CHECK(data.u(4) == 1);  // has text
        CHECK(data.str() == "idle.e@0.000000");
        CHECK(data.at() == f0[7].data.size());
    }
    const auto& f1 = raw.frame_list[1].chunks;
    REQUIRE(f1.size() == 3);
    CHECK(f1[1].type == 0x2005);
    CHECK(f1[2].type == 0x2020);
    {
        Bytes data(f1[2].data);
        CHECK(data.u(4) == 3);  // has text and colour
        CHECK(data.str() == "stale: render changed");
        CHECK(data.u(1) == 255);
        CHECK(data.u(1) == 102);
        CHECK(data.u(1) == 0);
        CHECK(data.u(1) == 255);
    }
    CHECK(raw.frame_list[2].chunks.size() == 1);  // no user data, no chunk
    CHECK(Bytes(raw.frame_list[2].chunks[0].data, 6).u(1) == 128);

    const AsepriteDocument back = read_aseprite(bytes);
    CHECK(back.frames[0].cels[0].user_text == "idle.e@0.000000");
    CHECK_FALSE(back.frames[0].cels[0].user_colour.has_value());
    REQUIRE(back.frames[1].cels.size() == 2);
    CHECK(back.frames[1].cels[0].user_text.empty());
    const AsepriteCel& c = back.frames[1].cels[1];
    CHECK(c.layer == 1);
    CHECK(c.x == 1);
    CHECK(c.y == 2);
    CHECK(c.image == correction.image);
    CHECK(c.user_text == "stale: render changed");
    CHECK(c.user_colour == AsepriteColour{255, 102, 0, 255});
    CHECK(back.frames[2].cels[0].opacity == 128);
    CHECK(back.frames[1].cels[0].opacity == 255);

    // As Aseprite writes them: a cel extra chunk between the cel and its
    // user data, and a linked cel (type 1) in frame 2 pointing at frame 0.
    auto edited = raw;
    pose_atlas_test::RawChunk extra;
    extra.type = 0x2006;
    extra.data.assign(36, 0);
    edited.frame_list[0].chunks.insert(edited.frame_list[0].chunks.begin() + 7, extra);
    pose_atlas_test::RawChunk linked;
    linked.type = 0x2005;
    linked.data = {0, 0, 0, 0, 0, 0, 255, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // layer 0, linked to frame 0
    edited.frame_list[2].chunks = {linked};
    const AsepriteDocument relinked = read_aseprite(serialize_raw(bytes, edited));
    CHECK(relinked.frames[0].cels[0].user_text == "idle.e@0.000000");
    REQUIRE(relinked.frames[2].cels.size() == 1);
    CHECK(relinked.frames[2].cels[0].image == doc.frames[0].cels[0].image);
    CHECK(relinked.frames[2].cels[0].user_text == "idle.e@0.000000");
    // A link to a frame without a cel on that layer is an error.
    linked.data[0] = 1;  // layer 1: frame 0 has no cel there
    edited.frame_list[2].chunks = {linked};
    CHECK_THROWS_AS(read_aseprite(serialize_raw(bytes, edited)), AsepriteError);

    AsepriteDocument bad = doc;
    bad.frames[0].cels[0].opacity = 300;
    CHECK_THROWS_AS(write_aseprite(bad), AsepriteError);
}
