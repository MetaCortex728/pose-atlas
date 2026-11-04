// SPDX-License-Identifier: Apache-2.0
#include "pose_atlas/image.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>

#include <stb_image.h>
#include <stb_image_write.h>

// Defined in stb_image_write's implementation (third_party/stb/stb_impl.c)
// but not declared in its header section.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace pose_atlas {

namespace {

int checked_int(std::size_t size, const char* what) {
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw ImageError(std::string(what) + ": data too large");
    }
    return static_cast<int>(size);
}

void append_png(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* p = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

}  // namespace

Image crop(const Image& image, int x, int y, int w, int h) {
    if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > image.width || y + h > image.height) {
        throw ImageError("crop: rectangle outside the image");
    }
    Image out(w, h);
    for (int row = 0; row < h; ++row) {
        std::memcpy(out.pixel(0, row), image.pixel(x, y + row), static_cast<std::size_t>(w) * 4);
    }
    return out;
}

void blit(Image& target, const Image& source, int x, int y) {
    if (x < 0 || y < 0 || x + source.width > target.width || y + source.height > target.height) {
        throw ImageError("blit: source does not fit at the target position");
    }
    for (int row = 0; row < source.height; ++row) {
        std::memcpy(target.pixel(x, y + row), source.pixel(0, row), static_cast<std::size_t>(source.width) * 4);
    }
}

Bounds alpha_bounds(const Image& image) {
    Bounds b{image.width, image.height, 0, 0};
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (image.pixel(x, y)[3] == 0) continue;
            if (x < b.x0) b.x0 = x;
            if (y < b.y0) b.y0 = y;
            if (x + 1 > b.x1) b.x1 = x + 1;
            if (y + 1 > b.y1) b.y1 = y + 1;
        }
    }
    if (b.x1 <= b.x0) return {};
    return b;
}

std::vector<std::uint8_t> encode_png(const Image& image) {
    if (image.width <= 0 || image.height <= 0) throw ImageError("encode_png: empty image");
    std::vector<std::uint8_t> out;
    if (stbi_write_png_to_func(append_png, &out, image.width, image.height, 4, image.rgba.data(),
                               image.width * 4) == 0) {
        throw ImageError("encode_png: encoder failed");
    }
    return out;
}

Image decode_png(const std::vector<std::uint8_t>& bytes, const std::string& source) {
    static const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), signature, 8) != 0) {
        throw ImageError(source + ": cannot decode PNG (not a PNG file)");
    }
    return decode_image(bytes, source);
}

Image decode_image(const std::vector<std::uint8_t>& bytes, const std::string& source) {
    int w = 0, h = 0, channels = 0;
    std::unique_ptr<stbi_uc, void (*)(void*)> data(
        stbi_load_from_memory(bytes.data(), checked_int(bytes.size(), source.c_str()), &w, &h, &channels, 4),
        stbi_image_free);
    if (!data) throw ImageError(source + ": cannot decode image (" + stbi_failure_reason() + ")");
    Image image(w, h);
    std::memcpy(image.rgba.data(), data.get(), image.rgba.size());
    return image;
}

std::vector<std::uint8_t> zlib_compress(const std::uint8_t* data, std::size_t size) {
    int out_size = 0;
    // stb takes a non-const pointer but does not write through it.
    std::unique_ptr<unsigned char, void (*)(void*)> out(
        stbi_zlib_compress(const_cast<unsigned char*>(data), checked_int(size, "zlib"), &out_size, 8), std::free);
    if (!out) throw ImageError("zlib: compression failed");
    return {out.get(), out.get() + out_size};
}

std::vector<std::uint8_t> zlib_decompress(const std::uint8_t* data, std::size_t size, std::size_t expected_size) {
    int out_size = 0;
    std::unique_ptr<char, void (*)(void*)> out(
        stbi_zlib_decode_malloc_guesssize(reinterpret_cast<const char*>(data), checked_int(size, "zlib"),
                                          checked_int(expected_size + 1, "zlib"), &out_size),
        std::free);
    if (!out) throw ImageError("zlib: corrupt stream");
    const auto* p = reinterpret_cast<const std::uint8_t*>(out.get());
    return {p, p + out_size};
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error(path + ": cannot open file");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) throw std::runtime_error(path + ": read error");
    return bytes;
}

std::string read_text_file(const std::string& path) {
    const auto bytes = read_file(path);
    return {bytes.begin(), bytes.end()};
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    const std::string temporary = path + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error(path + ": cannot create file");
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) throw std::runtime_error(path + ": write error");
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        throw std::runtime_error(path + ": cannot replace file");
    }
}

void write_text_file(const std::string& path, const std::string& text) {
    write_file(path, std::vector<std::uint8_t>(text.begin(), text.end()));
}

// SHA-256 (FIPS 180-4).
std::string sha256_hex(const std::uint8_t* data, std::size_t size) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto block = [&](const std::uint8_t* p) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t{p[i * 4]} << 24) | (std::uint32_t{p[i * 4 + 1]} << 16) |
                   (std::uint32_t{p[i * 4 + 2]} << 8) | std::uint32_t{p[i * 4 + 3]};
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };
    std::size_t full = size / 64;
    for (std::size_t i = 0; i < full; ++i) block(data + i * 64);
    std::array<std::uint8_t, 128> tail{};
    const std::size_t rest = size - full * 64;
    if (rest > 0) std::memcpy(tail.data(), data + full * 64, rest);
    tail[rest] = 0x80;
    const std::size_t tail_blocks = rest + 9 > 64 ? 2 : 1;
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[tail_blocks * 64 - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    for (std::size_t i = 0; i < tail_blocks; ++i) block(tail.data() + i * 64);
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (std::uint32_t word : h) {
        for (int shift = 28; shift >= 0; shift -= 4) out.push_back(hex[(word >> shift) & 0xf]);
    }
    return out;
}

}  // namespace pose_atlas
