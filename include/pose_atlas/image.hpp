// SPDX-License-Identifier: Apache-2.0
// 8-bit RGBA images (straight alpha, sRGB colour), PNG encoding and decoding,
// zlib streams, file I/O and SHA-256 content hashes.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace pose_atlas {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4, rows top to bottom

    Image() = default;
    Image(int w, int h) : width(w), height(h), rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0) {}

    std::uint8_t* pixel(int x, int y) {
        return rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
    }
    const std::uint8_t* pixel(int x, int y) const {
        return rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
    }
    bool operator==(const Image& other) const {
        return width == other.width && height == other.height && rgba == other.rgba;
    }
    bool operator!=(const Image& other) const { return !(*this == other); }
};

class ImageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Copy of the rectangle (x, y, w, h), which must lie inside the image.
Image crop(const Image& image, int x, int y, int w, int h);
/// Copy `source` into `target` with its top-left corner at (x, y).
void blit(Image& target, const Image& source, int x, int y);

/// Smallest rectangle holding every pixel with non-zero alpha.
struct Bounds {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // x1, y1 exclusive
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};
Bounds alpha_bounds(const Image& image);

/// PNG bytes of an RGBA image. Deterministic: same pixels, same bytes.
std::vector<std::uint8_t> encode_png(const Image& image);
/// Decode a PNG into RGBA; throws ImageError.
Image decode_png(const std::vector<std::uint8_t>& bytes, const std::string& source = "png");
/// Decode a PNG or (baseline or progressive) JPEG into RGBA; throws
/// ImageError for anything else.
Image decode_image(const std::vector<std::uint8_t>& bytes, const std::string& source = "image");

/// zlib stream (RFC 1950) of `data`, and back.
std::vector<std::uint8_t> zlib_compress(const std::uint8_t* data, std::size_t size);
std::vector<std::uint8_t> zlib_decompress(const std::uint8_t* data, std::size_t size, std::size_t expected_size);

std::vector<std::uint8_t> read_file(const std::string& path);
std::string read_text_file(const std::string& path);
/// Writes atomically enough for our purposes: to a temporary name, then
/// renamed over `path`. Throws std::runtime_error on failure.
void write_file(const std::string& path, const std::vector<std::uint8_t>& bytes);
void write_text_file(const std::string& path, const std::string& text);

/// SHA-256 as 64 lowercase hex digits.
std::string sha256_hex(const std::uint8_t* data, std::size_t size);
inline std::string sha256_hex(const std::vector<std::uint8_t>& bytes) { return sha256_hex(bytes.data(), bytes.size()); }
inline std::string sha256_hex(const std::string& text) {
    return sha256_hex(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

}  // namespace pose_atlas
