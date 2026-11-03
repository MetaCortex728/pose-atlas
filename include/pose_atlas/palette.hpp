// SPDX-License-Identifier: Apache-2.0
// Palette-locked output: a palette of sRGB colours read from JSON or PNG.
#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "pose_atlas/image.hpp"

namespace pose_atlas {

/// An sRGB colour, 8 bits per channel.
using Rgb8 = std::array<std::uint8_t, 3>;

inline constexpr std::size_t kMinPaletteColours = 2;
inline constexpr std::size_t kMaxPaletteColours = 256;

class PaletteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Distinct sRGB colours in file order. The order matters only for ties in
/// nearest-colour matching (the first wins).
struct Palette {
    std::vector<Rgb8> colours;
};

/// Parse a palette: a PNG (detected by its signature), whose distinct fully
/// opaque pixels in row order are the colours (pixels with any transparency
/// are ignored), or JSON {"colors": ["#rrggbb", ...]} without duplicates.
/// Throws PaletteError unless there are 2 to 256 colours.
Palette parse_palette(const std::vector<std::uint8_t>& bytes, const std::string& source = "palette");
Palette load_palette(const std::string& path);

/// "#rrggbb", lowercase.
std::string hex_colour(const Rgb8& colour);
/// Parses "#rrggbb" (either case); false for anything else.
bool parse_hex_colour(const std::string& text, Rgb8& out);

/// Canonical JSON {"colors": [...]} of a colour list, and its SHA-256: the
/// palette's content hash, the same for a PNG and a JSON file with the same
/// colours in the same order.
std::string palette_json(const std::vector<Rgb8>& colours);
std::string palette_sha256(const std::vector<Rgb8>& colours);

}  // namespace pose_atlas
