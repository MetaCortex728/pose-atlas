// SPDX-License-Identifier: Apache-2.0
// Reading and writing .aseprite files (RGBA), following the documented file
// format: https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md
//
// Only what pose-atlas needs is supported: normal image layers, compressed
// image cels, tags, slices with a pivot, frame durations, an sRGB colour
// profile and a palette. The reader skips chunks it does not know.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "pose_atlas/image.hpp"

namespace pose_atlas {

struct AsepriteCel {
    int layer = 0;
    int x = 0;
    int y = 0;
    Image image;
};

struct AsepriteFrame {
    int duration_ms = 100;
    std::vector<AsepriteCel> cels;
};

struct AsepriteTag {
    std::string name;
    int from = 0;
    int to = 0;
};

struct AsepriteSliceKey {
    int frame = 0;
    int x = 0, y = 0, width = 0, height = 0;
    std::optional<std::array<int, 2>> pivot;
};

struct AsepriteSlice {
    std::string name;
    std::vector<AsepriteSliceKey> keys;
};

struct AsepriteDocument {
    int width = 0;
    int height = 0;
    std::vector<std::string> layers;  // bottom to top
    std::vector<AsepriteFrame> frames;
    std::vector<AsepriteTag> tags;
    std::vector<AsepriteSlice> slices;
};

class AsepriteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// File bytes. Deterministic. Throws AsepriteError when the document does
/// not fit the format (sizes, counts, names beyond 16-bit fields).
std::vector<std::uint8_t> write_aseprite(const AsepriteDocument& document);

/// Parse a 32-bit RGBA file; throws AsepriteError.
AsepriteDocument read_aseprite(const std::vector<std::uint8_t>& bytes, const std::string& source = "aseprite");

}  // namespace pose_atlas
