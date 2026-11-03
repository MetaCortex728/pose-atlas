// SPDX-License-Identifier: Apache-2.0
#include "pose_atlas/aseprite.hpp"

#include <limits>

namespace pose_atlas {

namespace {

constexpr std::uint16_t kFileMagic = 0xA5E0;
constexpr std::uint16_t kFrameMagic = 0xF1FA;
constexpr std::uint16_t kChunkLayer = 0x2004;
constexpr std::uint16_t kChunkCel = 0x2005;
constexpr std::uint16_t kChunkColorProfile = 0x2007;
constexpr std::uint16_t kChunkTags = 0x2018;
constexpr std::uint16_t kChunkPalette = 0x2019;
constexpr std::uint16_t kChunkSlice = 0x2022;
constexpr std::uint16_t kCelCompressedImage = 2;

// Little-endian writer.
class Writer {
public:
    void byte(std::uint8_t v) { out_.push_back(v); }
    void word(std::uint32_t v) {
        if (v > 0xffff) throw AsepriteError("value " + std::to_string(v) + " does not fit a WORD");
        byte(static_cast<std::uint8_t>(v));
        byte(static_cast<std::uint8_t>(v >> 8));
    }
    void short_(int v) {
        if (v < -32768 || v > 32767) throw AsepriteError("value " + std::to_string(v) + " does not fit a SHORT");
        word(static_cast<std::uint16_t>(static_cast<std::int16_t>(v)));
    }
    void dword(std::uint64_t v) {
        if (v > 0xffffffffu) throw AsepriteError("value does not fit a DWORD");
        for (int i = 0; i < 4; ++i) byte(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void long_(long long v) {
        if (v < std::numeric_limits<std::int32_t>::min() || v > std::numeric_limits<std::int32_t>::max()) {
            throw AsepriteError("value does not fit a LONG");
        }
        dword(static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
    }
    void zeros(std::size_t n) { out_.insert(out_.end(), n, 0); }
    void string(const std::string& s) {
        word(static_cast<std::uint32_t>(s.size()));
        out_.insert(out_.end(), s.begin(), s.end());
    }
    void bytes(const std::vector<std::uint8_t>& b) { out_.insert(out_.end(), b.begin(), b.end()); }
    std::size_t size() const { return out_.size(); }
    std::vector<std::uint8_t>& data() { return out_; }
    void patch_dword(std::size_t at, std::uint64_t v) {
        if (v > 0xffffffffu) throw AsepriteError("file too large");
        for (std::size_t i = 0; i < 4; ++i) out_[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }

private:
    std::vector<std::uint8_t> out_;
};

// Writes chunk header placeholders and patches the size when done.
class Chunk {
public:
    Chunk(Writer& w, std::uint16_t type, int& count) : w_(w), start_(w.size()) {
        w_.dword(0);
        w_.word(type);
        ++count;
    }
    ~Chunk() { w_.patch_dword(start_, w_.size() - start_); }
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;

private:
    Writer& w_;
    std::size_t start_;
};

// Little-endian reader with bounds checks.
class Reader {
public:
    Reader(const std::vector<std::uint8_t>& data, std::size_t begin, std::size_t end, std::string source)
        : data_(data), pos_(begin), end_(end), source_(std::move(source)) {}
    std::uint8_t byte() {
        need(1);
        return data_[pos_++];
    }
    std::uint16_t word() {
        need(2);
        const std::uint16_t v = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
        pos_ += 2;
        return v;
    }
    int short_() { return static_cast<std::int16_t>(word()); }
    std::uint32_t dword() {
        need(4);
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < 4; ++i) v |= std::uint32_t{data_[pos_ + i]} << (8 * i);
        pos_ += 4;
        return v;
    }
    int long_() { return static_cast<std::int32_t>(dword()); }
    void skip(std::size_t n) {
        need(n);
        pos_ += n;
    }
    std::string string() {
        const std::size_t n = word();
        need(n);
        std::string s(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return s;
    }
    std::size_t pos() const { return pos_; }
    std::size_t end() const { return end_; }
    const std::uint8_t* here() const { return data_.data() + pos_; }
    [[noreturn]] void fail(const std::string& what) const {
        throw AsepriteError(source_ + ": " + what + " at byte " + std::to_string(pos_));
    }

private:
    void need(std::size_t n) const {
        if (n > end_ - pos_) fail("unexpected end of data");
    }
    const std::vector<std::uint8_t>& data_;
    std::size_t pos_;
    std::size_t end_;
    std::string source_;
};

}  // namespace

std::vector<std::uint8_t> write_aseprite(const AsepriteDocument& doc) {
    if (doc.width < 1 || doc.height < 1 || doc.width > 65535 || doc.height > 65535) {
        throw AsepriteError("sprite size must be 1 to 65535 pixels");
    }
    if (doc.frames.empty() || doc.frames.size() > 65535) throw AsepriteError("1 to 65535 frames required");
    if (doc.layers.empty()) throw AsepriteError("at least one layer required");
    for (const auto& tag : doc.tags) {
        if (tag.from < 0 || tag.to < tag.from || tag.to >= static_cast<int>(doc.frames.size())) {
            throw AsepriteError("tag \"" + tag.name + "\" has an invalid frame range");
        }
    }

    Writer w;
    // Header, 128 bytes.
    w.dword(0);  // file size, patched below
    w.word(kFileMagic);
    w.word(static_cast<std::uint32_t>(doc.frames.size()));
    w.word(static_cast<std::uint32_t>(doc.width));
    w.word(static_cast<std::uint32_t>(doc.height));
    w.word(32);   // colour depth: RGBA
    w.dword(1);   // flags: layer opacity is valid
    w.word(100);  // speed (deprecated)
    w.dword(0);
    w.dword(0);
    w.byte(0);  // transparent index (indexed sprites only)
    w.zeros(3);
    w.word(1);  // number of colours
    w.byte(1);  // pixel width
    w.byte(1);  // pixel height
    w.short_(0);
    w.short_(0);
    w.word(16);  // grid width
    w.word(16);  // grid height
    w.zeros(84);

    for (std::size_t f = 0; f < doc.frames.size(); ++f) {
        const AsepriteFrame& frame = doc.frames[f];
        if (frame.duration_ms < 1 || frame.duration_ms > 65535) throw AsepriteError("frame duration out of range");
        const std::size_t frame_start = w.size();
        w.dword(0);  // bytes in frame, patched below
        w.word(kFrameMagic);
        const std::size_t old_count_at = w.size();
        w.word(0);  // old chunk count, patched below
        w.word(static_cast<std::uint32_t>(frame.duration_ms));
        w.zeros(2);
        const std::size_t new_count_at = w.size();
        w.dword(0);  // chunk count, patched below
        int chunks = 0;
        if (f == 0) {
            {
                Chunk c(w, kChunkColorProfile, chunks);
                w.word(1);  // sRGB
                w.word(0);  // flags
                w.dword(0);  // fixed gamma (unused)
                w.zeros(8);
            }
            {
                Chunk c(w, kChunkPalette, chunks);
                w.dword(1);  // palette size
                w.dword(0);  // first index
                w.dword(0);  // last index
                w.zeros(8);
                w.word(0);  // entry flags
                w.byte(0);
                w.byte(0);
                w.byte(0);
                w.byte(255);
            }
            for (const std::string& name : doc.layers) {
                Chunk c(w, kChunkLayer, chunks);
                w.word(3);  // visible, editable
                w.word(0);  // normal image layer
                w.word(0);  // child level
                w.word(0);  // default width (ignored)
                w.word(0);  // default height (ignored)
                w.word(0);  // blend mode: normal
                w.byte(255);
                w.zeros(3);
                w.string(name);
            }
            if (!doc.tags.empty()) {
                Chunk c(w, kChunkTags, chunks);
                w.word(static_cast<std::uint32_t>(doc.tags.size()));
                w.zeros(8);
                for (const AsepriteTag& tag : doc.tags) {
                    w.word(static_cast<std::uint32_t>(tag.from));
                    w.word(static_cast<std::uint32_t>(tag.to));
                    w.byte(0);  // forward
                    w.word(0);  // repeat: infinite
                    w.zeros(6);
                    w.zeros(3);  // RGB (deprecated)
                    w.byte(0);
                    w.string(tag.name);
                }
            }
            for (const AsepriteSlice& slice : doc.slices) {
                Chunk c(w, kChunkSlice, chunks);
                const bool pivot = !slice.keys.empty() && slice.keys.front().pivot.has_value();
                for (const auto& key : slice.keys) {
                    if (key.pivot.has_value() != pivot) {
                        throw AsepriteError("slice \"" + slice.name + "\": all keys must have a pivot or none");
                    }
                }
                w.dword(slice.keys.size());
                w.dword(pivot ? 2 : 0);
                w.dword(0);
                w.string(slice.name);
                for (const auto& key : slice.keys) {
                    w.dword(static_cast<std::uint32_t>(key.frame));
                    w.long_(key.x);
                    w.long_(key.y);
                    w.dword(static_cast<std::uint32_t>(key.width));
                    w.dword(static_cast<std::uint32_t>(key.height));
                    if (pivot) {
                        w.long_((*key.pivot)[0]);
                        w.long_((*key.pivot)[1]);
                    }
                }
            }
        }
        for (const AsepriteCel& cel : frame.cels) {
            if (cel.layer < 0 || cel.layer >= static_cast<int>(doc.layers.size())) {
                throw AsepriteError("cel on a layer that does not exist");
            }
            Chunk c(w, kChunkCel, chunks);
            w.word(static_cast<std::uint32_t>(cel.layer));
            w.short_(cel.x);
            w.short_(cel.y);
            w.byte(255);  // opacity
            w.word(kCelCompressedImage);
            w.short_(0);  // z-index
            w.zeros(5);
            w.word(static_cast<std::uint32_t>(cel.image.width));
            w.word(static_cast<std::uint32_t>(cel.image.height));
            w.bytes(zlib_compress(cel.image.rgba.data(), cel.image.rgba.size()));
        }
        w.patch_dword(frame_start, w.size() - frame_start);
        auto& data = w.data();
        const std::uint32_t old_count = chunks < 0xffff ? static_cast<std::uint32_t>(chunks) : 0xffff;
        data[old_count_at] = static_cast<std::uint8_t>(old_count);
        data[old_count_at + 1] = static_cast<std::uint8_t>(old_count >> 8);
        w.patch_dword(new_count_at, static_cast<std::uint64_t>(chunks));
    }
    w.patch_dword(0, w.size());
    return std::move(w.data());
}

AsepriteDocument read_aseprite(const std::vector<std::uint8_t>& bytes, const std::string& source) {
    Reader header(bytes, 0, bytes.size(), source);
    const std::uint32_t file_size = header.dword();
    if (header.word() != kFileMagic) header.fail("not an .aseprite file (bad magic)");
    if (file_size != bytes.size()) header.fail("file size in the header differs from the data");
    AsepriteDocument doc;
    const int frame_count = header.word();
    doc.width = header.word();
    doc.height = header.word();
    if (header.word() != 32) header.fail("only RGBA (32-bit) files are supported");
    header.skip(128 - 14);

    std::size_t pos = 128;
    for (int f = 0; f < frame_count; ++f) {
        Reader fr(bytes, pos, bytes.size(), source);
        const std::uint32_t frame_size = fr.dword();
        if (frame_size < 16 || frame_size > bytes.size() - pos) fr.fail("bad frame size");
        if (fr.word() != kFrameMagic) fr.fail("bad frame magic");
        const std::uint16_t old_count = fr.word();
        AsepriteFrame frame;
        frame.duration_ms = fr.word();
        fr.skip(2);
        const std::uint32_t new_count = fr.dword();
        const std::uint32_t chunks = new_count != 0 ? new_count : old_count;
        std::size_t chunk_pos = pos + 16;
        for (std::uint32_t c = 0; c < chunks; ++c) {
            Reader ch(bytes, chunk_pos, pos + frame_size, source);
            const std::uint32_t size = ch.dword();
            if (size < 6 || size > pos + frame_size - chunk_pos) ch.fail("bad chunk size");
            const std::uint16_t type = ch.word();
            Reader r(bytes, chunk_pos + 6, chunk_pos + size, source);
            if (type == kChunkLayer) {
                r.skip(2);
                if (r.word() != 0) r.fail("only normal image layers are supported");
                r.skip(2 + 2 + 2 + 2 + 1 + 3);
                doc.layers.push_back(r.string());
            } else if (type == kChunkCel) {
                AsepriteCel cel;
                cel.layer = r.word();
                cel.x = r.short_();
                cel.y = r.short_();
                r.skip(1);
                const std::uint16_t cel_type = r.word();
                r.skip(2 + 5);
                if (cel_type != kCelCompressedImage && cel_type != 0) r.fail("unsupported cel type");
                const int w = r.word();
                const int h = r.word();
                cel.image = Image(w, h);
                if (cel_type == 0) {
                    if (r.end() - r.pos() < cel.image.rgba.size()) r.fail("raw cel too short");
                    std::copy(r.here(), r.here() + cel.image.rgba.size(), cel.image.rgba.begin());
                } else {
                    const auto pixels = zlib_decompress(r.here(), r.end() - r.pos(), cel.image.rgba.size());
                    if (pixels.size() != cel.image.rgba.size()) r.fail("cel pixel data has the wrong size");
                    cel.image.rgba = pixels;
                }
                frame.cels.push_back(std::move(cel));
            } else if (type == kChunkTags) {
                const int n = r.word();
                r.skip(8);
                for (int i = 0; i < n; ++i) {
                    AsepriteTag tag;
                    tag.from = r.word();
                    tag.to = r.word();
                    r.skip(1 + 2 + 6 + 3 + 1);
                    tag.name = r.string();
                    doc.tags.push_back(tag);
                }
            } else if (type == kChunkSlice) {
                AsepriteSlice slice;
                const std::uint32_t keys = r.dword();
                const std::uint32_t flags = r.dword();
                r.skip(4);
                slice.name = r.string();
                for (std::uint32_t k = 0; k < keys; ++k) {
                    AsepriteSliceKey key;
                    key.frame = static_cast<int>(r.dword());
                    key.x = r.long_();
                    key.y = r.long_();
                    key.width = static_cast<int>(r.dword());
                    key.height = static_cast<int>(r.dword());
                    if (flags & 1) r.skip(4 * 4);  // 9-patch centre
                    if (flags & 2) {
                        const int px = r.long_();
                        const int py = r.long_();
                        key.pivot = std::array<int, 2>{px, py};
                    }
                    slice.keys.push_back(key);
                }
                doc.slices.push_back(std::move(slice));
            }
            chunk_pos += size;
        }
        doc.frames.push_back(std::move(frame));
        pos += frame_size;
    }
    if (pos != bytes.size()) throw AsepriteError(source + ": trailing data after the last frame");
    return doc;
}

}  // namespace pose_atlas
