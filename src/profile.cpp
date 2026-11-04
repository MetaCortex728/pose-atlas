// SPDX-License-Identifier: Apache-2.0
#include "pose_atlas/profile.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace pose_atlas {

namespace {

using nlohmann::ordered_json;

std::string join_problems(const std::string& source, const std::vector<std::string>& problems) {
    std::string text = source + ": invalid profile";
    for (const auto& problem : problems) text += "\n  " + source + ": " + problem;
    return text;
}

constexpr std::array<std::pair<Axis, const char*>, 6> kAxisNames = {{{Axis::PosX, "+X"},
                                                                     {Axis::NegX, "-X"},
                                                                     {Axis::PosY, "+Y"},
                                                                     {Axis::NegY, "-Y"},
                                                                     {Axis::PosZ, "+Z"},
                                                                     {Axis::NegZ, "-Z"}}};

constexpr std::array<std::pair<ResampleFilter, const char*>, 4> kFilterNames = {
    {{ResampleFilter::Box, "box"},
     {ResampleFilter::Triangle, "triangle"},
     {ResampleFilter::Mitchell, "mitchell"},
     {ResampleFilter::Lanczos3, "lanczos3"}}};

const std::set<std::string>& known_keys() {
    static const std::set<std::string> keys = {
        "format",           "version",          "id",
        "description",      "projection",       "elevation_degrees",
        "base_azimuth_degrees", "camera_distance_m", "screen_headings",
        "character_forward", "world_up",        "cell_pixels",
        "ground_pivot_pixels", "meters_per_pixel", "supersampling",
        "resampling_filter", "zoom_set"};
    return keys;
}

// Reads typed fields from a JSON object and records problems instead of
// stopping at the first one.
class Reader {
public:
    Reader(const ordered_json& object, std::vector<std::string>& problems)
        : object_(object), problems_(problems) {}

    const ordered_json* field(const std::string& key, bool required = true) {
        const auto it = object_.find(key);
        if (it == object_.end()) {
            if (required) problems_.push_back(key + ": missing");
            return nullptr;
        }
        return &*it;
    }

    bool number(const std::string& key, double& out) {
        const auto* value = field(key);
        if (value == nullptr) return false;
        if (!value->is_number()) {
            problems_.push_back(key + ": expected a number, got " + value->dump());
            return false;
        }
        out = value->get<double>();
        return true;
    }

    bool integer(const std::string& key, const ordered_json& value, int& out) {
        if (!value.is_number_integer() || value.get<long long>() < -1000000000LL ||
            value.get<long long>() > 1000000000LL) {
            problems_.push_back(key + ": expected an integer, got " + value.dump());
            return false;
        }
        out = static_cast<int>(value.get<long long>());
        return true;
    }

    bool integer(const std::string& key, int& out) {
        const auto* value = field(key);
        return value != nullptr && integer(key, *value, out);
    }

    bool string(const std::string& key, std::string& out, bool required = true) {
        const auto* value = field(key, required);
        if (value == nullptr) return false;
        if (!value->is_string()) {
            problems_.push_back(key + ": expected a string, got " + value->dump());
            return false;
        }
        out = value->get<std::string>();
        return true;
    }

    bool integer_array(const std::string& key, std::vector<int>& out) {
        const auto* value = field(key);
        if (value == nullptr) return false;
        if (!value->is_array()) {
            problems_.push_back(key + ": expected an array of integers, got " + value->dump());
            return false;
        }
        out.clear();
        bool ok = true;
        for (std::size_t i = 0; i < value->size(); ++i) {
            int item = 0;
            if (integer(key + "[" + std::to_string(i) + "]", (*value)[i], item)) {
                out.push_back(item);
            } else {
                ok = false;
            }
        }
        return ok;
    }

    template <typename Enum, std::size_t N>
    bool name(const std::string& key, const std::array<std::pair<Enum, const char*>, N>& names,
              Enum& out) {
        std::string text;
        if (!string(key, text)) return false;
        for (const auto& [value, label] : names) {
            if (text == label) {
                out = value;
                return true;
            }
        }
        std::string allowed;
        for (const auto& entry : names) allowed += std::string(allowed.empty() ? "" : ", ") + entry.second;
        problems_.push_back(key + ": unknown value \"" + text + "\" (expected one of " + allowed + ")");
        return false;
    }

private:
    const ordered_json& object_;
    std::vector<std::string>& problems_;
};

}  // namespace

ProfileError::ProfileError(const std::string& source, std::vector<std::string> problems)
    : std::runtime_error(join_problems(source, problems)), problems_(std::move(problems)) {}

std::string to_string(Axis axis) {
    for (const auto& [value, label] : kAxisNames) {
        if (value == axis) return label;
    }
    return "?";
}

std::string to_string(ResampleFilter filter) {
    for (const auto& [value, label] : kFilterNames) {
        if (value == filter) return label;
    }
    return "?";
}

Vec3 axis_vector(Axis axis) {
    switch (axis) {
        case Axis::PosX: return {1, 0, 0};
        case Axis::NegX: return {-1, 0, 0};
        case Axis::PosY: return {0, 1, 0};
        case Axis::NegY: return {0, -1, 0};
        case Axis::PosZ: return {0, 0, 1};
        case Axis::NegZ: return {0, 0, -1};
    }
    return {};
}

std::vector<std::string> validate_profile(const Profile& p) {
    std::vector<std::string> problems;
    auto finite = [&](const char* key, double value) {
        if (!std::isfinite(value)) {
            problems.push_back(std::string(key) + ": must be finite");
            return false;
        }
        return true;
    };
    if (p.id.empty()) problems.push_back("id: must not be empty");
    if (finite("elevation_degrees", p.elevation_degrees) &&
        !(p.elevation_degrees > 0.0 && p.elevation_degrees < 90.0)) {
        problems.push_back("elevation_degrees: must be between 0 and 90 (exclusive), got " +
                           std::to_string(p.elevation_degrees));
    }
    if (finite("base_azimuth_degrees", p.base_azimuth_degrees) &&
        std::fabs(p.base_azimuth_degrees) > 360.0) {
        problems.push_back("base_azimuth_degrees: must be within [-360, 360]");
    }
    if (finite("camera_distance_m", p.camera_distance_m) && !(p.camera_distance_m > 0.0)) {
        problems.push_back("camera_distance_m: must be positive");
    }
    if (finite("meters_per_pixel", p.meters_per_pixel) && !(p.meters_per_pixel > 0.0)) {
        problems.push_back("meters_per_pixel: must be positive");
    }
    if (std::abs(dot(axis_vector(p.character_forward), axis_vector(p.world_up))) > 0.5) {
        problems.push_back("character_forward: " + to_string(p.character_forward) +
                           " is not perpendicular to world_up " + to_string(p.world_up));
    }
    if (p.cell_pixels < 8 || p.cell_pixels > 4096) {
        problems.push_back("cell_pixels: must be between 8 and 4096, got " +
                           std::to_string(p.cell_pixels));
    } else {
        for (int i = 0; i < 2; ++i) {
            const int v = p.ground_pivot_pixels[static_cast<std::size_t>(i)];
            if (v <= 0 || v >= p.cell_pixels) {
                problems.push_back("ground_pivot_pixels[" + std::to_string(i) +
                                   "]: must lie strictly inside the cell (0, " +
                                   std::to_string(p.cell_pixels) + "), got " + std::to_string(v));
            }
        }
    }
    if (p.supersampling < 1 || p.supersampling > 16) {
        problems.push_back("supersampling: must be between 1 and 16, got " +
                           std::to_string(p.supersampling));
    }
    if (p.zoom_set.empty()) problems.push_back("zoom_set: must not be empty");
    for (std::size_t i = 0; i < p.zoom_set.size(); ++i) {
        const int zoom = p.zoom_set[i];
        if (zoom < 1 || zoom > 16) {
            problems.push_back("zoom_set[" + std::to_string(i) + "]: must be between 1 and 16, got " +
                               std::to_string(zoom));
        }
        if (i > 0 && zoom <= p.zoom_set[i - 1]) {
            problems.push_back("zoom_set: must be strictly increasing");
            break;
        }
    }
    return problems;
}

Profile parse_profile(const std::string& json_text, const std::string& source) {
    ordered_json root;
    try {
        root = ordered_json::parse(json_text);
    } catch (const nlohmann::json::parse_error& error) {
        throw ProfileError(source, {std::string("not valid JSON: ") + error.what()});
    }
    if (!root.is_object()) throw ProfileError(source, {"expected a JSON object at the top level"});

    std::vector<std::string> problems;
    Reader read(root, problems);

    std::string format;
    if (read.string("format", format) && format != kProfileFormat) {
        problems.push_back("format: expected \"" + std::string(kProfileFormat) + "\", got \"" + format +
                           "\"");
    }
    int version = 0;
    if (read.integer("version", version) && version != kProfileVersion) {
        // Other versions may change field meanings; refuse rather than guess.
        throw ProfileError(source, {"version: " + std::to_string(version) +
                                        " is not supported (this build reads version " +
                                        std::to_string(kProfileVersion) + ")"});
    }
    for (const auto& item : root.items()) {
        if (known_keys().count(item.key()) == 0) problems.push_back(item.key() + ": unknown field");
    }

    Profile p;
    read.string("id", p.id);
    read.string("description", p.description, false);
    std::string projection;
    if (read.string("projection", projection) && projection != "orthographic") {
        problems.push_back("projection: only \"orthographic\" is supported, got \"" + projection + "\"");
    }
    read.number("elevation_degrees", p.elevation_degrees);
    read.number("base_azimuth_degrees", p.base_azimuth_degrees);
    read.number("camera_distance_m", p.camera_distance_m);
    read.number("meters_per_pixel", p.meters_per_pixel);

    if (const auto* headings = read.field("screen_headings")) {
        ordered_json expected = ordered_json::array();
        for (const char* h : kScreenHeadings) expected.push_back(h);
        if (*headings != expected) {
            problems.push_back("screen_headings: must be exactly " + expected.dump() +
                               " (heading i is i * 45 degrees counter-clockwise from screen right), got " +
                               headings->dump());
        }
    }
    read.name("character_forward", kAxisNames, p.character_forward);
    read.name("world_up", kAxisNames, p.world_up);
    read.integer("cell_pixels", p.cell_pixels);
    std::vector<int> pivot;
    if (read.integer_array("ground_pivot_pixels", pivot)) {
        if (pivot.size() == 2) {
            p.ground_pivot_pixels = {pivot[0], pivot[1]};
        } else {
            problems.push_back("ground_pivot_pixels: expected [x, y], got " + std::to_string(pivot.size()) +
                               " values");
        }
    }
    read.integer("supersampling", p.supersampling);
    read.name("resampling_filter", kFilterNames, p.resampling_filter);
    read.integer_array("zoom_set", p.zoom_set);

    // Value checks only make sense once the fields parsed.
    if (problems.empty()) problems = validate_profile(p);
    if (!problems.empty()) throw ProfileError(source, std::move(problems));
    return p;
}

Profile load_profile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw ProfileError(path, {"cannot open file"});
    std::ostringstream text;
    text << file.rdbuf();
    return parse_profile(text.str(), path);
}

std::string profile_to_json(const Profile& p) {
    ordered_json root;
    root["format"] = kProfileFormat;
    root["version"] = kProfileVersion;
    root["id"] = p.id;
    if (!p.description.empty()) root["description"] = p.description;
    root["projection"] = "orthographic";
    root["elevation_degrees"] = p.elevation_degrees;
    root["base_azimuth_degrees"] = p.base_azimuth_degrees;
    root["camera_distance_m"] = p.camera_distance_m;
    root["screen_headings"] = ordered_json::array();
    for (const char* h : kScreenHeadings) root["screen_headings"].push_back(h);
    root["character_forward"] = to_string(p.character_forward);
    root["world_up"] = to_string(p.world_up);
    root["cell_pixels"] = p.cell_pixels;
    root["ground_pivot_pixels"] = {p.ground_pivot_pixels[0], p.ground_pivot_pixels[1]};
    root["meters_per_pixel"] = p.meters_per_pixel;
    root["supersampling"] = p.supersampling;
    root["resampling_filter"] = to_string(p.resampling_filter);
    root["zoom_set"] = p.zoom_set;
    return root.dump(2) + "\n";
}

}  // namespace pose_atlas
