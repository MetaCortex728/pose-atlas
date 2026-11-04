// SPDX-License-Identifier: Apache-2.0
// Reading and writing .aseprite files (RGBA), following the documented file
// format: https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md
//
// Only what pose-atlas needs is supported: normal image layers, compressed
// image cels (the reader also takes raw and linked cels), cel user data (text
// and colour), tags, slices with a pivot, frame durations, an sRGB colour
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

/// RGBA, 8 bits per channel.
using AsepriteColour = std::array<std::uint8_t, 4>;

struct AsepriteCel {
    int layer = 0;
    int x = 0;
    int y = 0;
    Image image;
    int opacity = 255;
    /// User data (chunk 0x2020 after the cel chunk): text, and a colour that
    /// Aseprite shows in the timeline. Written when either is set. A linked
    /// cel is read as a copy of the cel it links to, user data included.
    std::string user_text;
    std::optional<AsepriteColour> user_colour;
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

/// The cel of `layer` in frame `frame` drawn onto a transparent image of the
/// sprite's size (clipped to it), with the cel's opacity applied to alpha.
/// Transparent when the frame has no cel on that layer.
Image flatten_layer(const AsepriteDocument& document, std::size_t frame, int layer);

/// Index of the layer with this name, or -1.
int find_layer(const AsepriteDocument& document, const std::string& name);

/// Parse a 32-bit RGBA file; throws AsepriteError.
AsepriteDocument read_aseprite(const std::vector<std::uint8_t>& bytes, const std::string& source = "aseprite");

}  // namespace pose_atlas
