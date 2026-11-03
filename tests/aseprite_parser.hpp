// SPDX-License-Identifier: Apache-2.0
// A small .aseprite parser for the tests, written from the file format
// documentation independently of the library's reader: it splits the file
// into header, frames and raw chunks and checks the framing on the way.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pose_atlas_test {

struct RawChunk {
    std::uint16_t type = 0;
    std::vector<std::uint8_t> data;  // without the 6-byte chunk header
};

struct RawFrame {
    std::uint16_t duration = 0;
    std::vector<RawChunk> chunks;
};

struct RawAseprite {
    std::uint16_t frames = 0, width = 0, height = 0, depth = 0;
    std::uint32_t flags = 0;
    std::vector<RawFrame> frame_list;
};

class Bytes {
public:
    Bytes(const std::vector<std::uint8_t>& d, std::size_t at = 0) : d_(d), at_(at) {}
    std::uint32_t u(int n) {
        REQUIRE(at_ + static_cast<std::size_t>(n) <= d_.size());
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= std::uint32_t{d_[at_ + static_cast<std::size_t>(i)]} << (8 * i);
        at_ += static_cast<std::size_t>(n);
        return v;
    }
    std::int32_t s32() { return static_cast<std::int32_t>(u(4)); }
    std::string str() {
        const std::size_t n = u(2);
        REQUIRE(at_ + n <= d_.size());
        std::string s(d_.begin() + static_cast<std::ptrdiff_t>(at_), d_.begin() + static_cast<std::ptrdiff_t>(at_ + n));
        at_ += n;
        return s;
    }
    void skip(std::size_t n) { at_ += n; }
    std::size_t at() const { return at_; }

private:
    const std::vector<std::uint8_t>& d_;
    std::size_t at_;
};

inline RawAseprite parse_raw_aseprite(const std::vector<std::uint8_t>& file) {
    RawAseprite out;
    REQUIRE(file.size() >= 128);
    Bytes h(file);
    CHECK(h.u(4) == file.size());
    CHECK(h.u(2) == 0xA5E0);
    out.frames = static_cast<std::uint16_t>(h.u(2));
    out.width = static_cast<std::uint16_t>(h.u(2));
    out.height = static_cast<std::uint16_t>(h.u(2));
    out.depth = static_cast<std::uint16_t>(h.u(2));
    out.flags = h.u(4);
    std::size_t at = 128;
    for (int f = 0; f < out.frames; ++f) {
        Bytes fr(file, at);
        const std::uint32_t size = fr.u(4);
        REQUIRE(at + size <= file.size());
        CHECK(fr.u(2) == 0xF1FA);
        const std::uint32_t old_count = fr.u(2);
        RawFrame frame;
        frame.duration = static_cast<std::uint16_t>(fr.u(2));
        fr.skip(2);
        const std::uint32_t count = fr.u(4);
        CHECK((old_count == count || (old_count == 0xFFFF && count >= 0xFFFF)));
        std::size_t c = at + 16;
        for (std::uint32_t i = 0; i < count; ++i) {
            Bytes ch(file, c);
            const std::uint32_t chunk_size = ch.u(4);
            REQUIRE(chunk_size >= 6);
            REQUIRE(c + chunk_size <= at + size);
            RawChunk chunk;
            chunk.type = static_cast<std::uint16_t>(ch.u(2));
            chunk.data.assign(file.begin() + static_cast<std::ptrdiff_t>(c + 6),
                              file.begin() + static_cast<std::ptrdiff_t>(c + chunk_size));
            frame.chunks.push_back(std::move(chunk));
            c += chunk_size;
        }
        CHECK(c == at + size);  // chunks fill the frame exactly
        out.frame_list.push_back(std::move(frame));
        at += size;
    }
    CHECK(at == file.size());  // frames fill the file exactly
    return out;
}

}  // namespace pose_atlas_test
