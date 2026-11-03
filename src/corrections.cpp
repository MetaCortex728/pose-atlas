// SPDX-License-Identifier: Apache-2.0
#include "pose_atlas/corrections.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>

#include <nlohmann/json.hpp>

namespace pose_atlas {

namespace {

using nlohmann::json;
using nlohmann::ordered_json;

std::string join_path(const std::string& directory, const std::string& name) {
    if (directory.empty() || directory.back() == '/') return directory + name;
    return directory + "/" + name;
}

bool has_pixels(const Image& image) {
    for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
        if (image.rgba[i] != 0) return true;
    }
    return false;
}

// The previous contract, when present and readable: per frame index its key
// (stored, or computed from action, view and source time) and per key its
// render hash (contracts with frame keys only).
struct OldContract {
    std::vector<std::string> keys;
    std::map<std::string, std::string> render_sha256;
};

std::optional<OldContract> read_old_contract(const std::string& path) {
    if (!std::filesystem::exists(path)) return std::nullopt;
    try {
        const json contract = json::parse(read_text_file(path));
        OldContract out;
        for (const json& frame : contract.at("frames")) {
            std::string key = frame.value("key", "");
            if (key.empty()) {
                key = frame_key(frame.at("action").get<std::string>(), frame.at("view").get<std::string>(),
                                frame.at("source_time_s").get<double>());
            }
            if (frame.contains("render_sha256")) out.render_sha256[key] = frame.at("render_sha256").get<std::string>();
            out.keys.push_back(std::move(key));
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;  // unreadable: as if absent
    }
}

}  // namespace

std::string corrections_backup_name(const std::vector<std::uint8_t>& aseprite_bytes) {
    return "atlas.before-" + sha256_hex(aseprite_bytes).substr(0, 12) + ".aseprite";
}

ExistingCorrections read_existing_corrections(const std::string& directory, bool discard) {
    ExistingCorrections out;
    const std::string path = join_path(directory, kAtlasAseprite);
    if (!std::filesystem::exists(path)) return out;
    const std::vector<std::uint8_t> bytes = read_file(path);
    AsepriteDocument doc;
    try {
        doc = read_aseprite(bytes, path);
    } catch (const AsepriteError& e) {
        throw CorrectionsError(std::string(e.what()) + "; cannot look for manual corrections in it, so it is not "
                               "overwritten (move it away to render anyway)");
    }
    const int corrections_layer = find_layer(doc, kAsepriteCorrectionsLayer);
    if (corrections_layer < 0) return out;
    std::vector<std::size_t> painted;
    std::vector<Image> images;
    for (std::size_t f = 0; f < doc.frames.size(); ++f) {
        Image image = flatten_layer(doc, f, corrections_layer);
        if (!has_pixels(image)) continue;
        painted.push_back(f);
        images.push_back(std::move(image));
    }
    if (painted.empty()) return out;

    // The backup, before anything else can go wrong.
    out.backup = corrections_backup_name(bytes);
    const std::string backup_path = join_path(directory, out.backup);
    if (!std::filesystem::exists(backup_path)) write_file(backup_path, bytes);
    if (discard) {
        out.discarded = true;
        return out;
    }

    const int render_layer = find_layer(doc, kAsepriteBaseLayer);
    std::optional<OldContract> contract;
    bool contract_read = false;
    for (std::size_t i = 0; i < painted.size(); ++i) {
        const std::size_t f = painted[i];
        std::string key;
        for (const AsepriteCel& cel : doc.frames[f].cels) {
            if (cel.layer == render_layer && render_layer >= 0) key = cel.user_text;
        }
        if (!contract_read) {
            contract = read_old_contract(join_path(directory, kContractJson));
            contract_read = true;
        }
        if (key.empty()) {
            if (!contract || contract->keys.size() != doc.frames.size()) {
                throw CorrectionsError(
                    std::string(kAtlasAseprite) + " has manual corrections, but frame " + std::to_string(f) +
                    " has no frame key (the file was written before frame keys) and there is no " + kContractJson +
                    " with " + std::to_string(doc.frames.size()) +
                    " frames to recover the keys from. Nothing was overwritten; the corrections are kept in " +
                    out.backup + ". Restore the matching " + kContractJson +
                    ", or re-render with --discard-corrections to start without them.");
            }
            key = contract->keys[f];
        }
        ExistingCorrection correction;
        correction.key = key;
        correction.frame = static_cast<int>(f);
        correction.image = std::move(images[i]);
        if (contract && contract->render_sha256.count(key) != 0) {
            correction.render_sha256 = contract->render_sha256.at(key);
        } else {
            correction.render_sha256 = sha256_hex(flatten_layer(doc, f, render_layer).rgba);
        }
        out.corrections.push_back(std::move(correction));
    }
    return out;
}

std::size_t CorrectionsReport::stale_count() const {
    std::size_t n = 0;
    for (const CarriedCorrection& c : carried) n += c.stale ? 1 : 0;
    return n;
}

CorrectionsReport carry_corrections(const ExistingCorrections& existing, Atlas& atlas) {
    CorrectionsReport report;
    report.backup = existing.backup;
    report.discarded = existing.discarded;
    const std::vector<AtlasFrame> frames = atlas_frames(atlas);
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < frames.size(); ++i) index.emplace(frames[i].key, i);
    std::map<std::string, int> taken;  // key -> old frame
    const int cell = atlas.profile.cell_pixels;
    for (const ExistingCorrection& c : existing.corrections) {
        const auto found = index.find(c.key);
        if (found == index.end()) {
            report.orphaned.push_back({c.key, c.frame, "no frame with this key in the new atlas"});
            continue;
        }
        const auto first = taken.emplace(c.key, c.frame);
        if (!first.second) {
            report.orphaned.push_back(
                {c.key, c.frame, "old frame " + std::to_string(first.first->second) + " has the same key"});
            continue;
        }
        if (atlas.corrections.empty()) atlas.corrections.resize(frames.size());
        const AtlasFrame& f = frames[found->second];
        const Image& render = atlas.cell(f.view, f.column);
        FrameCorrection& target = atlas.corrections[found->second];
        target.image = Image(cell, cell);
        for (int y = 0; y < std::min(cell, c.image.height); ++y) {
            for (int x = 0; x < std::min(cell, c.image.width); ++x) {
                const std::uint8_t* p = c.image.pixel(x, y);
                std::copy(p, p + 4, target.image.pixel(x, y));
            }
        }
        target.stale = sha256_hex(render.rgba) != c.render_sha256;
        report.carried.push_back({c.key, c.frame, static_cast<int>(found->second), target.stale});
    }
    return report;
}

std::string CorrectionsReport::to_json() const {
    ordered_json carried_json = ordered_json::array(), stale_json = ordered_json::array(),
                 orphaned_json = ordered_json::array();
    for (const CarriedCorrection& c : carried) {
        const ordered_json item = {{"key", c.key}, {"old_frame", c.old_frame}, {"frame", c.frame}};
        carried_json.push_back(item);
        if (c.stale) stale_json.push_back(item);
    }
    for (const OrphanedCorrection& o : orphaned) {
        orphaned_json.push_back({{"key", o.key}, {"old_frame", o.old_frame}, {"reason", o.reason}});
    }
    ordered_json out;
    out["format"] = kCorrectionsReportFormat;
    out["version"] = kCorrectionsReportVersion;
    out["backup"] = backup.empty() ? ordered_json(nullptr) : ordered_json(backup);
    out["discarded"] = discarded;
    out["carried"] = carried_json;
    out["stale"] = stale_json;
    out["orphaned"] = orphaned_json;
    return out.dump(2) + "\n";
}

std::string CorrectionsReport::summary() const {
    if (backup.empty()) return "corrections: none to carry";
    if (discarded) return "corrections: discarded (--discard-corrections); previous file kept as " + backup;
    return "corrections: " + std::to_string(carried.size()) + " carried (" + std::to_string(stale_count()) +
           " stale), " + std::to_string(orphaned.size()) + " orphaned; previous file kept as " + backup;
}

}  // namespace pose_atlas
