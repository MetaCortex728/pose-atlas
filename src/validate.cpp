// SPDX-License-Identifier: Apache-2.0
// Port of the prototype's check-pose-pipeline.py to this pipeline's outputs.
//
// Kept: frame count and layout (rows = views, columns = poses), cell size
// and pivot, physical framing (cell span = cell x metres per pixel), one
// frame tag per pose and view with from = to = its own frame, exactly one
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
// Added: format and version of the contract, file hashes, consistency of the
// contract with its embedded profile.
#include <cmath>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "pose_atlas/atlas.hpp"
#include "pose_atlas/camera.hpp"

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
        check_extra(contract);

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

        // Tags: one per pose and view, each on its own frame only.
        const json& tag_list = meta.at("frameTags");
        expect(tag_list.size() == total, "Expected one frame tag per pose and view");
        std::map<std::string, std::pair<int, int>> tags;
        for (const json& tag : tag_list) tags[tag.at("name")] = {tag.at("from").get<int>(), tag.at("to").get<int>()};

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
        if (frames.size() != total || items.size() != total) return;
        std::vector<std::string> cell_hashes;
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
            const auto tag = tags.find(expected_name);
            expect(tag != tags.end() && tag->second == std::make_pair(static_cast<int>(index), static_cast<int>(index)),
                   where + "Tag was extended when later frames were added, or is missing");
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

            const Image cell = crop(atlas, x, y, size, size);
            check_cell(index, cell);
            const Bounds b = alpha_bounds(cell);
            if (b.empty()) {
                problem(where + "blank pose");
            } else {
                expect(b.x0 > 0 && b.y0 > 0 && b.x1 < size && b.y1 < size, where + "Clipping: alpha touches the cell edge");
            }
            cell_hashes.push_back(sha256_hex(cell.rgba));
            report_.frames.push_back({expected_name, b, duration});
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

    // Hooks for checks of further outputs.
    void check_extra(const json&) {}
    void check_cell(std::size_t, const Image&) {}

    std::string dir_;
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
