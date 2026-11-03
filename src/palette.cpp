// SPDX-License-Identifier: Apache-2.0
#include "pose_atlas/palette.hpp"

#include <algorithm>
#include <cmath>
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

double squared_distance(const Oklab& x, const Oklab& y) {
    const double dl = x.L - y.L, da = x.a - y.a, db = x.b - y.b;
    return dl * dl + da * da + db * db;
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

Oklab linear_to_oklab(double r, double g, double b) {
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

PaletteMatcher::PaletteMatcher(std::vector<Rgb8> colours) : colours_(std::move(colours)) {
    if (colours_.empty()) throw PaletteError("palette matcher: no colours");
    for (const Rgb8& c : colours_) {
        const std::array<float, 3> lin = {static_cast<float>(srgb_to_linear(c[0] / 255.0)),
                                          static_cast<float>(srgb_to_linear(c[1] / 255.0)),
                                          static_cast<float>(srgb_to_linear(c[2] / 255.0))};
        linear_.push_back(lin);
        lab_.push_back(linear_to_oklab(lin[0], lin[1], lin[2]));
        sorted_.push_back(pack(c));
    }
    std::sort(sorted_.begin(), sorted_.end());
}

bool PaletteMatcher::contains(const Rgb8& colour) const {
    return std::binary_search(sorted_.begin(), sorted_.end(), pack(colour));
}

std::size_t PaletteMatcher::nearest(const Oklab& lab) const {
    std::size_t best = 0;
    double best_distance = squared_distance(lab, lab_[0]);
    for (std::size_t i = 1; i < lab_.size(); ++i) {
        const double d = squared_distance(lab, lab_[i]);
        if (d < best_distance) {
            best_distance = d;
            best = i;
        }
    }
    return best;
}

std::size_t PaletteMatcher::nearest_linear(float r, float g, float b) {
    // Memo key: the colour quantised to 2^-20 (far below 8-bit steps).
    auto q = [](float v) {
        const double c = std::clamp(static_cast<double>(v), 0.0, 1.0);
        return static_cast<std::uint64_t>(std::llround(c * 1048575.0));
    };
    const std::uint64_t key = (q(r) << 40) | (q(g) << 20) | q(b);
    const auto found = linear_memo_.find(key);
    if (found != linear_memo_.end()) return found->second;
    const double scale = 1.0 / 1048575.0;
    const std::size_t index = nearest(linear_to_oklab(static_cast<double>(q(r)) * scale,
                                                      static_cast<double>(q(g)) * scale,
                                                      static_cast<double>(q(b)) * scale));
    linear_memo_.emplace(key, index);
    return index;
}

std::size_t PaletteMatcher::nearest_srgb(const Rgb8& colour) {
    const std::uint32_t key = pack(colour);
    const auto found = srgb_memo_.find(key);
    if (found != srgb_memo_.end()) return found->second;
    const std::size_t index = nearest(linear_to_oklab(srgb_to_linear(colour[0] / 255.0),
                                                      srgb_to_linear(colour[1] / 255.0),
                                                      srgb_to_linear(colour[2] / 255.0)));
    srgb_memo_.emplace(key, index);
    return index;
}

std::string to_string(PaletteStage stage) {
    switch (stage) {
        case PaletteStage::None: return "none";
        case PaletteStage::Before: return "before";
        case PaletteStage::After: return "after";
        case PaletteStage::BeforeAfter: return "before_after";
    }
    return "none";
}

PaletteStage parse_palette_stage(const std::string& text) {
    for (PaletteStage s : {PaletteStage::None, PaletteStage::Before, PaletteStage::After, PaletteStage::BeforeAfter}) {
        if (text == to_string(s)) return s;
    }
    throw std::invalid_argument("unknown palette stage '" + text + "' (expected none, before, after or before_after)");
}

void snap_linear(LinearImage& image, PaletteMatcher& matcher) {
    for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        float* p = image.rgba.data() + i;
        const float a = p[3];
        if (!(a > 0.0f)) continue;
        const std::size_t index = matcher.nearest_linear(p[0] / a, p[1] / a, p[2] / a);
        const auto& c = matcher.linear(index);
        for (std::size_t k = 0; k < 3; ++k) p[k] = c[k] * a;
    }
}

void snap_image(Image& image, PaletteMatcher& matcher) {
    for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        std::uint8_t* p = image.rgba.data() + i;
        if (p[3] == 0) continue;
        const Rgb8& c = matcher.colours()[matcher.nearest_srgb({p[0], p[1], p[2]})];
        p[0] = c[0];
        p[1] = c[1];
        p[2] = c[2];
    }
}

void threshold_alpha(Image& image, double threshold) {
    for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        std::uint8_t* p = image.rgba.data() + i;
        if (alpha_passes(p[3], threshold)) {
            p[3] = 255;
        } else {
            p[0] = p[1] = p[2] = p[3] = 0;
        }
    }
}

}  // namespace pose_atlas
