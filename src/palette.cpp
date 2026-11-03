// SPDX-License-Identifier: Apache-2.0
#include "pose_atlas/palette.hpp"

#include <cstdio>
#include <cstring>
#include <set>

#include <nlohmann/json.hpp>

namespace pose_atlas {

namespace {

std::uint32_t pack(const Rgb8& c) {
    return (std::uint32_t{c[0]} << 16) | (std::uint32_t{c[1]} << 8) | std::uint32_t{c[2]};
}

void check_count(std::size_t count, const std::string& source) {
    if (count < kMinPaletteColours || count > kMaxPaletteColours) {
        throw PaletteError(source + ": a palette needs " + std::to_string(kMinPaletteColours) + " to " +
                           std::to_string(kMaxPaletteColours) + " colours, found " + std::to_string(count));
    }
}

Palette palette_from_png(const std::vector<std::uint8_t>& bytes, const std::string& source) {
    Image image;
    try {
        image = decode_png(bytes, source);
    } catch (const ImageError& e) {
        throw PaletteError(e.what());
    }
    Palette palette;
    std::set<std::uint32_t> seen;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::uint8_t* p = image.pixel(x, y);
            if (p[3] != 255) continue;
            const Rgb8 c{p[0], p[1], p[2]};
            if (!seen.insert(pack(c)).second) continue;
            palette.colours.push_back(c);
            // Stop early on photographs and the like.
            if (palette.colours.size() > kMaxPaletteColours) check_count(palette.colours.size(), source + " (PNG)");
        }
    }
    check_count(palette.colours.size(), source + " (PNG)");
    return palette;
}

Palette palette_from_json(const std::vector<std::uint8_t>& bytes, const std::string& source) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(bytes.begin(), bytes.end());
    } catch (const nlohmann::json::exception& e) {
        throw PaletteError(source + ": neither a PNG nor valid JSON (" + e.what() + ")");
    }
    if (!j.is_object() || !j.contains("colors") || !j["colors"].is_array()) {
        throw PaletteError(source + ": expected a PNG or JSON {\"colors\": [\"#rrggbb\", ...]}");
    }
    Palette palette;
    std::set<std::uint32_t> seen;
    for (std::size_t i = 0; i < j["colors"].size(); ++i) {
        const auto& item = j["colors"][i];
        Rgb8 c{};
        if (!item.is_string() || !parse_hex_colour(item.get<std::string>(), c)) {
            throw PaletteError(source + ": colors[" + std::to_string(i) + "] is not \"#rrggbb\"");
        }
        if (!seen.insert(pack(c)).second) {
            throw PaletteError(source + ": colors[" + std::to_string(i) + "] repeats " + hex_colour(c));
        }
        palette.colours.push_back(c);
    }
    check_count(palette.colours.size(), source);
    return palette;
}

}  // namespace

Palette parse_palette(const std::vector<std::uint8_t>& bytes, const std::string& source) {
    static const std::uint8_t signature[4] = {0x89, 'P', 'N', 'G'};
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), signature, 4) == 0) return palette_from_png(bytes, source);
    return palette_from_json(bytes, source);
}

Palette load_palette(const std::string& path) {
    std::vector<std::uint8_t> bytes;
    try {
        bytes = read_file(path);
    } catch (const std::exception& e) {
        throw PaletteError(e.what());
    }
    return parse_palette(bytes, path);
}

std::string hex_colour(const Rgb8& colour) {
    char buffer[8];
    std::snprintf(buffer, sizeof buffer, "#%02x%02x%02x", colour[0], colour[1], colour[2]);
    return buffer;
}

bool parse_hex_colour(const std::string& text, Rgb8& out) {
    if (text.size() != 7 || text[0] != '#') return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < 3; ++i) {
        const int hi = digit(text[1 + 2 * i]), lo = digit(text[2 + 2 * i]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return true;
}

std::string palette_json(const std::vector<Rgb8>& colours) {
    nlohmann::ordered_json j;
    j["colors"] = nlohmann::ordered_json::array();
    for (const Rgb8& c : colours) j["colors"].push_back(hex_colour(c));
    return j.dump();
}

std::string palette_sha256(const std::vector<Rgb8>& colours) { return sha256_hex(palette_json(colours)); }

}  // namespace pose_atlas
