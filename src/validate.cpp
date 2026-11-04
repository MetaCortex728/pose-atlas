// SPDX-License-Identifier: Apache-2.0
// Port of the prototype's check-pose-pipeline.py to this pipeline's outputs.
//
// Kept: frame count and layout (rows = views, columns = poses), cell size
// and pivot, physical framing (cell span = cell x metres per pixel), exactly one
// "foot" slice with the pivot and full-cell bounds, frame rectangles,
// untrimmed and unrotated frames with full source size, durations matching
// the contract, frame names "<pose>.<view>", projected headings matching
// their screen directions, and non-blank cells with a clear margin (no
// clipping).
//
// Changed: the prototype required every cell to differ. A real animation can
// repeat a pose (holds, or passing poses of a run that look alike from the
// side), so this requires instead that the eight views of each pose differ
// (directions changed) and that no action of several poses shows one image
// throughout a view (poses changed).
//
// Dropped: the prototype's lists of pose names and schema numbers, its fixed
// 64/96 px cells and 208 cm span, and the requirement of binary alpha (the
// renderer's filtered edges have partial alpha by design).
//
// Changed: the prototype had one frame tag per pose and view (from = to =
// its own frame). Engine importers expect one tag per state, as Aseprite's
// own JSON export writes it, so atlas.json and atlas.aseprite both need one
// tag per state, "<action>.<view>" over that state's frames, in the same
// order.
//
// Added: format and version of the contract, file hashes, consistency of the
// contract with its embedded profile, frame keys and input hashes, and, when
// the contract names a palette, the palette lock of atlas.png (Render with
// the corrections): every opaque pixel in the palette (plus the outline
// colour), every alpha 0 or 255. Erased pixels are transparent, so excluded.
//
// The .aseprite file takes the place of the prototype's raw frame files and
// its Aseprite import script: every frame's base-layer cel must hold exactly
// the render the contract hashed (render_sha256; "Import/export changed raw
// RGBA pixels") and carry the frame's key, the corrections layer may hold
// hand corrections (fully opaque or fully transparent pixels), the atlas.png
// cell must be Render with the corrections on top (erase-colour pixels
// cleared), durations must match, there must be one
// tag per state over that state's frames, and a "foot" slice with the pivot.
#include <cmath>
#include <set>
#include <tuple>
#include <sstream>

#include <nlohmann/json.hpp>

#include "pose_atlas/atlas.hpp"
#include "pose_atlas/camera.hpp"
#include "pose_atlas/palette.hpp"

namespace pose_atlas {

namespace {

using nlohmann::json;
using nlohmann::ordered_json;

std::string join_path(const std::string& directory, const std::string& name) {
    if (directory.empty() || directory.back() == '/') return directory + name;
    return directory + "/" + name;
}

json rect(int x, int y, int w, int h) { return {{"x", x}, {"y", y}, {"w", w}, {"h", h}}; }

class Validator {
public:
    explicit Validator(std::string directory) : dir_(std::move(directory)) {}

    ValidationReport run() {
        try {
            check();
        } catch (const std::exception& e) {
            // Structural errors (missing files, wrong JSON types) stop the
            // check; everything found so far is kept.
            problem(std::string("cannot continue: ") + e.what());
        }
        return std::move(report_);
    }

private:
    void problem(const std::string& text) { report_.problems.push_back(text); }
    void expect(bool ok, const std::string& text) {
        if (!ok) problem(text);
    }

    json load_json(const std::string& name) {
        try {
            return json::parse(read_text_file(join_path(dir_, name)));
        } catch (const json::exception& e) {
            throw std::runtime_error(name + ": invalid JSON (" + e.what() + ")");
        }
    }

    void check_hash(const json& files, const char* key, const std::vector<std::uint8_t>& bytes, const char* file) {
        if (!files.contains(key)) {
            problem(std::string("contract: files.") + key + " missing");
            return;
        }
        expect(files[key].at("file") == file, std::string("contract: files.") + key + ".file is not " + file);
        expect(files[key].at("sha256") == sha256_hex(bytes),
               std::string(file) + ": SHA-256 differs from the contract (file changed after export)");
    }

    void check() {
        const json contract = load_json(kContractJson);
        if (contract.value("format", "") != kContractFormat || contract.value("version", 0) != kContractVersion) {
            problem(std::string(kContractJson) + ": not a " + kContractFormat + " version " +
                    std::to_string(kContractVersion));
            return;
        }
        const auto atlas_bytes = read_file(join_path(dir_, kAtlasPng));
        const auto metadata_bytes = read_file(join_path(dir_, kAtlasJson));
        const json& files = contract.at("files");
        check_hash(files, "atlas", atlas_bytes, kAtlasPng);
        check_hash(files, "metadata", metadata_bytes, kAtlasJson);
        const auto aseprite_bytes = read_file(join_path(dir_, kAtlasAseprite));
        check_hash(files, "aseprite", aseprite_bytes, kAtlasAseprite);
        aseprite_ = read_aseprite(aseprite_bytes, kAtlasAseprite);

        const json metadata = json::parse(metadata_bytes.begin(), metadata_bytes.end());
        const json& frames = metadata.at("frames");
        const json& meta = metadata.at("meta");
        const json& items = contract.at("frames");

        // Profile and framing.
        const Profile profile = parse_profile(contract.at("sources").at("profile").at("values").dump(), "contract profile");
        const int size = profile.cell_pixels;
        expect(contract.at("cell") == json({size, size}), "contract: cell does not match the profile's cell_pixels");
        expect(contract.at("pivot") == json({profile.ground_pivot_pixels[0], profile.ground_pivot_pixels[1]}),
               "contract: pivot does not match the profile's ground_pivot_pixels");
        expect(contract.at("meters_per_pixel").get<double>() == profile.meters_per_pixel,
               "contract: meters_per_pixel does not match the profile");
        expect(std::fabs(contract.at("cell_span_m").get<double>() - size * profile.meters_per_pixel) < 1e-9,
               "Physical framing changed: cell_span_m is not cell x meters_per_pixel");

        // Views and poses.
        const int views = contract.at("view_count").get<int>();
        expect(views == 8, "contract: view_count must be 8");
        expect(contract.at("views") == json(kScreenHeadings), "contract: views are not e, ne, n, nw, w, sw, s, se");
        const auto poses = contract.at("pose_names").get<std::vector<std::string>>();
        const int columns = static_cast<int>(poses.size());
        expect(contract.at("columns").get<int>() == columns, "contract: columns differs from the pose names");
        expect(std::set<std::string>(poses.begin(), poses.end()).size() == poses.size(), "contract: duplicate pose names");
        int covered = 0;
        for (const json& action : contract.at("actions")) {
            const std::string name = action.at("name");
            const int count = action.at("pose_count");
            expect(action.at("first_column").get<int>() == covered, "contract: action " + name + " is not contiguous");
            for (int i = 0; i < count && covered + i < columns; ++i) {
                expect(poses[static_cast<std::size_t>(covered + i)] == name + std::to_string(i),
                       "contract: column " + std::to_string(covered + i) + " is not " + name + std::to_string(i));
            }
            // State coverage: every action rendered in every view.
            for (const char* v : kScreenHeadings) {
                expect(action.at("render_keys").contains(v), "contract: action " + name + " has no render in view " + v);
            }
            covered += count;
        }
        expect(covered == columns, "contract: actions do not cover the pose columns");

        const std::size_t total = static_cast<std::size_t>(columns) * static_cast<std::size_t>(views);
        expect(frames.size() == total && items.size() == total, "Expected " + std::to_string(total) +
                                                                     " frames (columns x views), metadata has " +
                                                                     std::to_string(frames.size()) + ", contract " +
                                                                     std::to_string(items.size()));
        expect(meta.at("size") == json({{"w", size * columns}, {"h", size * views}}), "meta.size is not the atlas size");
        expect(meta.at("image") == kAtlasPng, std::string("meta.image is not ") + kAtlasPng);

        // Tags: one per state, the same in atlas.json and atlas.aseprite.
        expected_tags_ = state_tags(contract, columns, views);
        std::vector<std::tuple<std::string, int, int>> json_tags;
        for (const json& tag : meta.at("frameTags")) {
            json_tags.emplace_back(tag.at("name").get<std::string>(), tag.at("from").get<int>(),
                                   tag.at("to").get<int>());
        }
        expect(json_tags == expected_tags_, "atlas.json: frameTags are not one per state (" +
                                                std::to_string(expected_tags_.size()) +
                                                " tags, <action>.<view> over that state's frames, view by view)");

        // The foot slice.
        const json& slices = meta.at("slices");
        if (slices.size() != 1 || slices[0].at("name") != "foot") {
            problem("Expected exactly one foot slice");
        } else {
            const json& keys = slices[0].at("keys");
            expect(keys.size() == 1 && keys[0].at("frame") == 0 &&
                       keys[0].at("pivot") ==
                           json({{"x", profile.ground_pivot_pixels[0]}, {"y", profile.ground_pivot_pixels[1]}}),
                   "foot slice: expected one key on frame 0 with the profile's pivot");
            expect(keys.size() == 1 && keys[0].at("bounds") == rect(0, 0, size, size),
                   "foot slice: bounds must be the whole cell");
        }

        // Pixels and frames.
        const Image atlas = decode_png(atlas_bytes, kAtlasPng);
        if (atlas.width != size * columns || atlas.height != size * views) {
            problem("atlas.png is " + std::to_string(atlas.width) + "x" + std::to_string(atlas.height) + ", expected " +
                    std::to_string(size * columns) + "x" + std::to_string(size * views));
            return;
        }
        check_palette_lock(contract, atlas, size, columns);
        if (frames.size() != total || items.size() != total) return;
        if (!contract.contains("erase_colour") || !contract.at("erase_colour").is_string() ||
            !parse_hex_colour(contract.at("erase_colour").get<std::string>(), erase_colour_)) {
            problem("contract: erase_colour is missing or not \"#rrggbb\"");
        }
        check_aseprite(contract, profile, columns, views);
        std::vector<std::string> cell_hashes;
        std::set<std::string> keys;
        const json& sources = contract.at("sources");
        const std::string model_sha = sources.at("model").at("sha256");
        const std::string profile_sha = sources.at("profile").at("sha256");
        const std::string settings_sha = sources.at("render_settings").at("sha256");
        const std::string renderer = contract.at("renderer");
        for (std::size_t index = 0; index < total; ++index) {
            const json& frame = frames[index];
            const json& item = items[index];
            const int column = static_cast<int>(index) % columns;
            const int row = static_cast<int>(index) / columns;
            const std::string expected_name =
                poses[static_cast<std::size_t>(column)] + "." + kScreenHeadings[static_cast<std::size_t>(row)];
            const std::string where = "frame " + std::to_string(index) + " (" + expected_name + "): ";
            const int x = column * size, y = row * size;
            expect(item.at("name") == expected_name, where + "contract name is " + item.at("name").dump());
            expect(frame.value("filename", "") == expected_name, where + "metadata filename differs");
            expect(item.at("view") == kScreenHeadings[static_cast<std::size_t>(row)], where + "contract view differs");
            expect(frame.at("frame") == rect(x, y, size, size), where + "frame rectangle is not its cell");
            expect(frame.at("trimmed") == false && frame.at("rotated") == false, where + "trimmed or rotated");
            expect(frame.at("sourceSize") == json({{"w", size}, {"h", size}}), where + "sourceSize is not the cell");
            expect(frame.at("spriteSourceSize") == rect(0, 0, size, size), where + "Unexpected source offset");
            const int duration = frame.at("duration").get<int>();
            expect(duration == item.at("duration_ms").get<int>(), where + "duration differs from the contract");
            expect(duration >= 1, where + "duration must be at least 1 ms");
            const double angle = row * kPi / 4;
            const auto heading = item.at("projected_heading").get<std::vector<double>>();
            expect(heading.size() == 2 &&
                       std::hypot(heading[0] - std::cos(angle), heading[1] - std::sin(angle)) < 1e-5,
                   where + "Facing does not project to its screen direction");
            const Vec2 analytic = projected_forward(profile, camera_guide(profile, row));
            const double length = std::hypot(analytic.x, analytic.y);
            expect(heading.size() == 2 &&
                       std::hypot(heading[0] - analytic.x / length, heading[1] - analytic.y / length) < 1e-9,
                   where + "projected_heading does not follow from the profile");

            // The frame key and the hash of the frame's inputs.
            const std::string action = item.at("action"), view = item.at("view");
            const double time = item.at("source_time_s");
            const std::string key = item.value("key", "");
            std::string expected_key;
            try {
                expected_key = frame_key(action, view, time);
            } catch (const AtlasError& e) {
                problem(where + e.what());
            }
            expect(key == expected_key, where + "key " + json(key).dump() + " is not " + json(expected_key).dump() +
                                            " (<action>.<view>@<source time to the microsecond>)");
            expect(keys.insert(key).second, where + "key " + json(key).dump() + " is not unique");
            expect(item.value("input_sha256", "") ==
                       frame_input_sha256(model_sha, profile_sha, settings_sha, renderer, action, view, time),
                   where + "input_sha256 is not the hash of the frame's inputs");

            const Image cell = crop(atlas, x, y, size, size);
            check_aseprite_cell(index, cell, item, key, where);
            const Bounds b = alpha_bounds(cell);
            if (b.empty()) {
                problem(where + "blank pose");
            } else {
                expect(b.x0 > 0 && b.y0 > 0 && b.x1 < size && b.y1 < size, where + "Clipping: alpha touches the cell edge");
            }
            cell_hashes.push_back(sha256_hex(cell.rgba));
            report_.frames.push_back({expected_name, b, duration});
        }
        if (partial_corrections_ > 0) {
            problem("atlas.aseprite: " + std::to_string(partial_corrections_) +
                    " corrections pixels with partial alpha (neither 0 nor 255)");
        }
        auto hash_at = [&](int row, int column) {
            return cell_hashes[static_cast<std::size_t>(row * columns + column)];
        };
        for (int column = 0; column < columns; ++column) {
            std::set<std::string> views_of_pose;
            for (int row = 0; row < views; ++row) views_of_pose.insert(hash_at(row, column));
            expect(static_cast<int>(views_of_pose.size()) == views,
                   "Directions did not change: pose " + poses[static_cast<std::size_t>(column)] +
                       " has identical views");
        }
        for (const json& action : contract.at("actions")) {
            const int first = action.at("first_column"), count = action.at("pose_count");
            if (count < 2 || first + count > columns) continue;
            for (int row = 0; row < views; ++row) {
                std::set<std::string> poses_in_view;
                for (int c = first; c < first + count; ++c) poses_in_view.insert(hash_at(row, c));
                expect(poses_in_view.size() > 1, "Poses did not change: action " + action.at("name").get<std::string>() +
                                                     " shows one image in view " +
                                                     kScreenHeadings[static_cast<std::size_t>(row)]);
            }
        }
    }

    // When the contract names a palette: every opaque pixel is a palette
    // colour (or the outline colour, when the outline is on) and every alpha
    // is 0 or 255. Reports the first few offenders and the totals.
    void check_palette_lock(const json& contract, const Image& atlas, int size, int columns) {
        const json& sources = contract.at("sources");
        if (!sources.contains("palette")) return;
        const json& palette = sources.at("palette");
        std::vector<Rgb8> colours;
        for (const json& item : palette.at("colors")) {
            Rgb8 c{};
            if (!item.is_string() || !parse_hex_colour(item.get<std::string>(), c)) {
                problem("contract: sources.palette.colors holds " + item.dump() + ", not \"#rrggbb\"");
                return;
            }
            colours.push_back(c);
        }
        expect(palette.at("sha256") == palette_sha256(colours),
               "contract: sources.palette.sha256 is not the hash of its colours");
        const json& settings = sources.at("render_settings").at("values");
        expect(settings.value("palette_sha256", json()) == palette.at("sha256"),
               "contract: the render settings' palette_sha256 differs from sources.palette");
        std::vector<Rgb8> allowed = colours;
        std::string extra;
        if (settings.value("outline", false)) {
            Rgb8 outline{};
            if (parse_hex_colour(settings.value("outline_colour", ""), outline)) {
                allowed.push_back(outline);
                extra = " or the outline colour " + hex_colour(outline);
            }
        }
        const PaletteMatcher matcher(allowed);
        constexpr int kShown = 5;
        std::int64_t off = 0, partial = 0;
        int shown = 0;
        for (int y = 0; y < atlas.height; ++y) {
            for (int x = 0; x < atlas.width; ++x) {
                const std::uint8_t* p = atlas.pixel(x, y);
                std::string what;
                if (p[3] != 0 && p[3] != 255) {
                    ++partial;
                    what = "alpha " + std::to_string(p[3]) + " is neither 0 nor 255";
                } else if (p[3] == 255 && !matcher.contains({p[0], p[1], p[2]})) {
                    ++off;
                    what = hex_colour({p[0], p[1], p[2]}) + " is not in the palette" + extra;
                }
                if (what.empty() || shown >= kShown) continue;
                ++shown;
                const int column = x / size, row = y / size;
                problem("palette lock: atlas pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") in frame " +
                        std::to_string(row * columns + column) + " at cell (" + std::to_string(x % size) + ", " +
                        std::to_string(y % size) + "): " + what);
            }
        }
        if (off + partial > 0) {
            problem("palette lock: " + std::to_string(off) + " opaque pixels off the palette and " +
                    std::to_string(partial) + " pixels with partial alpha in " + kAtlasPng);
        }
    }

    // "<action>.<view>" over that action's poses in that view, view by view,
    // actions in column order.
    static std::vector<std::tuple<std::string, int, int>> state_tags(const json& contract, int columns, int views) {
        std::vector<std::tuple<std::string, int, int>> out;
        for (int row = 0; row < views && row < static_cast<int>(kScreenHeadings.size()); ++row) {
            for (const json& action : contract.at("actions")) {
                const int from = row * columns + action.at("first_column").get<int>();
                out.emplace_back(action.at("name").get<std::string>() + "." +
                                     kScreenHeadings[static_cast<std::size_t>(row)],
                                 from, from + action.at("pose_count").get<int>() - 1);
            }
        }
        return out;
    }

    void check_aseprite(const json& contract, const Profile& profile, int columns, int views) {
        const AsepriteDocument& doc = aseprite_;
        const int size = profile.cell_pixels;
        const std::size_t total = static_cast<std::size_t>(columns) * static_cast<std::size_t>(views);
        aseprite_ok_ = doc.width == size && doc.height == size && doc.frames.size() == total &&
                       doc.layers == std::vector<std::string>{kAsepriteBaseLayer, kAsepriteCorrectionsLayer};
        expect(doc.width == size && doc.height == size, "atlas.aseprite: sprite size is not the cell");
        expect(doc.frames.size() == total, "atlas.aseprite: expected one frame per pose and view");
        expect(doc.layers == std::vector<std::string>{kAsepriteBaseLayer, kAsepriteCorrectionsLayer},
               std::string("atlas.aseprite: layers must be \"") + kAsepriteBaseLayer + "\" and \"" +
                   kAsepriteCorrectionsLayer + "\"");
        const json& items = contract.at("frames");
        for (std::size_t i = 0; i < doc.frames.size() && i < items.size(); ++i) {
            expect(doc.frames[i].duration_ms == items[i].at("duration_ms").get<int>(),
                   "atlas.aseprite: frame " + std::to_string(i) + " duration differs from the contract");
        }
        // One tag per state: action.view over its poses in that view.
        std::vector<std::tuple<std::string, int, int>> found;
        for (const AsepriteTag& tag : doc.tags) found.emplace_back(tag.name, tag.from, tag.to);
        expect(found == expected_tags_, "atlas.aseprite: tags are not one per state over that state's frames");
        expect(doc.slices.size() == 1 && doc.slices[0].name == "foot" && doc.slices[0].keys.size() == 1 &&
                   doc.slices[0].keys[0].frame == 0 && doc.slices[0].keys[0].x == 0 && doc.slices[0].keys[0].y == 0 &&
                   doc.slices[0].keys[0].width == size && doc.slices[0].keys[0].height == size &&
                   doc.slices[0].keys[0].pivot ==
                       std::array<int, 2>{profile.ground_pivot_pixels[0], profile.ground_pivot_pixels[1]},
               "atlas.aseprite: expected one foot slice over the cell with the profile's pivot");
    }

    // The frame in atlas.aseprite: the Render cel carries the frame's key and
    // holds the render the contract hashed; the corrections layer holds only
    // fully opaque or fully transparent pixels; Render with the corrections
    // on top (and erase pixels cleared) is the atlas.png cell.
    void check_aseprite_cell(std::size_t index, const Image& cell, const json& item, const std::string& key,
                             const std::string& where) {
        if (!aseprite_ok_) return;
        for (const AsepriteCel& cel : aseprite_.frames[index].cels) {
            if (cel.layer != 0) continue;
            expect(cel.user_text == key, where + "atlas.aseprite Render cel's user data is " +
                                             json(cel.user_text).dump() + ", not the frame key " + json(key).dump());
            if (cel.x < 0 || cel.y < 0 || cel.x + cel.image.width > cell.width ||
                cel.y + cel.image.height > cell.height) {
                problem(where + "atlas.aseprite Render cel lies outside the cell");
                return;
            }
        }
        const Image render = flatten_layer(aseprite_, index, 0);
        expect(item.value("render_sha256", "") == sha256_hex(render.rgba),
               where + "Import/export changed raw RGBA pixels (the atlas.aseprite Render cel differs from the "
                       "contract's render_sha256)");
        const Image corrections = flatten_layer(aseprite_, index, 1);
        for (int y = 0; y < corrections.height; ++y) {
            for (int x = 0; x < corrections.width; ++x) {
                const std::uint8_t a = corrections.pixel(x, y)[3];
                if (a == 0 || a == 255) continue;
                if (++partial_corrections_ <= 5) {
                    problem(where + "corrections pixel (" + std::to_string(x) + ", " + std::to_string(y) +
                            ") has alpha " + std::to_string(a) + "; corrections must be fully opaque or fully "
                            "transparent");
                }
            }
        }
        expect(composite_cell(render, corrections, erase_colour_) == cell,
               where + "atlas.png is not the composite of the atlas.aseprite layers " + kAsepriteBaseLayer + " and " +
                   kAsepriteCorrectionsLayer + " (erase colour " + hex_colour(erase_colour_) + ")");
    }

    std::string dir_;
    std::vector<std::tuple<std::string, int, int>> expected_tags_;
    AsepriteDocument aseprite_;
    bool aseprite_ok_ = false;
    Rgb8 erase_colour_ = kDefaultEraseColour;
    long long partial_corrections_ = 0;
    ValidationReport report_;
};

}  // namespace

std::string ValidationReport::to_json() const {
    ordered_json out;
    out["passed"] = passed();
    out["problems"] = problems;
    out["frames"] = ordered_json::array();
    for (const FrameStats& f : frames) {
        out["frames"].push_back({{"pose", f.name},
                                 {"bounds", {f.bounds.x0, f.bounds.y0, f.bounds.x1, f.bounds.y1}},
                                 {"visible_height", f.bounds.y1 - f.bounds.y0},
                                 {"duration_ms", f.duration_ms}});
    }
    return out.dump(2) + "\n";
}

ValidationReport validate_atlas(const std::string& directory) { return Validator(directory).run(); }

}  // namespace pose_atlas
