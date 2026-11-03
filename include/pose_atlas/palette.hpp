// SPDX-License-Identifier: Apache-2.0
// Palette-locked output: a palette of sRGB colours read from JSON or PNG,
// nearest-colour matching in Oklab, snapping of the supersampled render or
// the final cell to the palette, hard alpha, and the statistics that show
// how far an image is from being palette-locked.
#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "pose_atlas/image.hpp"
#include "pose_atlas/resample.hpp"

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

/// Oklab (Ottosson 2020) of a linear-light sRGB colour.
struct Oklab {
    double L = 0.0, a = 0.0, b = 0.0;
};
Oklab linear_to_oklab(double r, double g, double b);

/// Nearest palette colour by Euclidean distance in Oklab; ties go to the
/// lower index. Results are memoised per input colour.
class PaletteMatcher {
public:
    explicit PaletteMatcher(std::vector<Rgb8> colours);

    const std::vector<Rgb8>& colours() const { return colours_; }
    /// Palette colour i in linear light (decoded sRGB).
    const std::array<float, 3>& linear(std::size_t i) const { return linear_[i]; }
    bool contains(const Rgb8& colour) const;

    std::size_t nearest_linear(float r, float g, float b);
    std::size_t nearest_srgb(const Rgb8& colour);

private:
    std::size_t nearest(const Oklab& lab) const;

    std::vector<Rgb8> colours_;
    std::vector<std::array<float, 3>> linear_;
    std::vector<Oklab> lab_;
    std::vector<std::uint32_t> sorted_;  // 0xRRGGBB of the colours, sorted
    std::unordered_map<std::uint64_t, std::size_t> linear_memo_;
    std::unordered_map<std::uint32_t, std::size_t> srgb_memo_;
};

/// Where the colours are snapped to the palette.
enum class PaletteStage {
    None,         // no palette
    Before,       // each supersampled pixel, then downsample
    After,        // downsample, then each cell pixel
    BeforeAfter,  // both
};
std::string to_string(PaletteStage stage);
/// Parses "none", "before", "after", "before_after"; throws
/// std::invalid_argument otherwise.
PaletteStage parse_palette_stage(const std::string& text);
inline bool snaps_before(PaletteStage s) { return s == PaletteStage::Before || s == PaletteStage::BeforeAfter; }
inline bool snaps_after(PaletteStage s) { return s == PaletteStage::After || s == PaletteStage::BeforeAfter; }

/// Replace the colour of every covered pixel of a premultiplied linear image
/// by its nearest palette colour (matching the un-premultiplied colour,
/// keeping the alpha).
void snap_linear(LinearImage& image, PaletteMatcher& matcher);
/// Replace the RGB of every pixel with non-zero alpha by its nearest palette
/// colour.
void snap_image(Image& image, PaletteMatcher& matcher);

/// True when 8-bit alpha `a` is at least `threshold` coverage.
inline bool alpha_passes(std::uint8_t a, double threshold) { return a / 255.0 >= threshold; }
/// Hard alpha: pixels whose coverage is at least `threshold` become alpha
/// 255, all others (0, 0, 0, 0).
void threshold_alpha(Image& image, double threshold);

/// Pixel counts of palette-locked renders (summed over cells).
struct PaletteStats {
    /// Opaque pixels of the output (after thresholding and outline).
    std::int64_t opaque = 0;
    /// Pixels that end up opaque whose colour is not in the palette, counted
    /// after the downsample and before the final snap (stage after or
    /// before_after); for stage before, which has no final snap, in the output.
    std::int64_t off_palette_before_snap = 0;
    /// Pixels with alpha strictly between 0 and 255 after the downsample,
    /// before thresholding.
    std::int64_t partial_alpha = 0;
    /// Opaque output pixels outside the allowed set (palette plus the outline
    /// colour when the outline is on).
    std::int64_t off_palette_output = 0;

    PaletteStats& operator+=(const PaletteStats& other);
    bool operator==(const PaletteStats& other) const;
};

}  // namespace pose_atlas
