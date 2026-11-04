// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "pose_atlas/profile.hpp"

using namespace pose_atlas;
using nlohmann::json;

namespace {

std::string read_text(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

const std::string kProfilePath = std::string(POSE_ATLAS_SOURCE_DIR) + "/profiles/quarter-view.json";

json base_json() { return json::parse(read_text(kProfilePath)); }

std::string error_of(const json& document) {
    try {
        parse_profile(document.dump(), "test.json");
    } catch (const ProfileError& error) {
        return error.what();
    }
    return "";
}

}  // namespace

TEST_CASE("profile: the shipped quarter-view profile carries the prototype's camera values") {
    const Profile p = load_profile(kProfilePath);
    const json prototype =
        json::parse(read_text(std::string(POSE_ATLAS_SOURCE_DIR) + "/prototype/quarter-view-profile.json"));
    CHECK(prototype["projection"] == "ORTHO");
    CHECK(p.elevation_degrees == prototype["elevation_degrees"].get<double>());
    CHECK(p.base_azimuth_degrees == prototype["base_azimuth_degrees"].get<double>());
    CHECK(p.camera_distance_m == prototype["camera_distance_m"].get<double>());
    CHECK(p.cell_pixels == prototype["cell_pixels"].get<int>());
    CHECK(p.meters_per_pixel == prototype["meters_per_pixel"].get<double>());
    CHECK(p.ground_pivot_pixels[0] == prototype["ground_pivot_pixels"][0].get<int>());
    CHECK(p.ground_pivot_pixels[1] == prototype["ground_pivot_pixels"][1].get<int>());
    CHECK(to_string(p.character_forward) == prototype["character_forward"].get<std::string>());
    CHECK(to_string(p.world_up) == prototype["world_up"].get<std::string>());
    CHECK(prototype["screen_headings"] == json({"e", "ne", "n", "nw", "w", "sw", "s", "se"}));
    CHECK(p.supersampling == 4);
    CHECK(p.resampling_filter == ResampleFilter::Mitchell);
    CHECK(p.zoom_set == std::vector<int>{1, 2, 3});
    CHECK(p.cell_span_m() == doctest::Approx(2.7733333333));
}

TEST_CASE("profile: JSON round trip") {
    const Profile p = load_profile(kProfilePath);
    const Profile q = parse_profile(profile_to_json(p));
    CHECK(profile_to_json(q) == profile_to_json(p));
    CHECK(q.meters_per_pixel == p.meters_per_pixel);
    CHECK(q.description == p.description);
}

TEST_CASE("profile: errors name the source, the field and the problem") {
    json doc = base_json();
    doc["elevation_degrees"] = 90;
    CHECK(error_of(doc).find("test.json: elevation_degrees: must be between 0 and 90") !=
          std::string::npos);

    doc = base_json();
    doc["projection"] = "perspective";
    CHECK(error_of(doc).find("projection: only \"orthographic\" is supported") != std::string::npos);

    doc = base_json();
    doc["screen_headings"] = {"n", "ne", "e", "se", "s", "sw", "w", "nw"};
    CHECK(error_of(doc).find("screen_headings: must be exactly") != std::string::npos);

    doc = base_json();
    doc["character_forward"] = "+Z";
    CHECK(error_of(doc).find("character_forward: +Z is not perpendicular to world_up +Z") !=
          std::string::npos);

    doc = base_json();
    doc["world_up"] = "up";
    CHECK(error_of(doc).find("world_up: unknown value \"up\"") != std::string::npos);

    doc = base_json();
    doc["ground_pivot_pixels"] = {128, 256};
    CHECK(error_of(doc).find("ground_pivot_pixels[1]: must lie strictly inside the cell") !=
          std::string::npos);

    doc = base_json();
    doc["ground_pivot_pixels"] = {128};
    CHECK(error_of(doc).find("ground_pivot_pixels: expected [x, y]") != std::string::npos);

    doc = base_json();
    doc["zoom_set"] = {1, 3, 2};
    CHECK(error_of(doc).find("zoom_set: must be strictly increasing") != std::string::npos);

    doc = base_json();
    doc["resampling_filter"] = "bicubic";
    CHECK(error_of(doc).find("resampling_filter: unknown value \"bicubic\" (expected one of box, "
                             "triangle, mitchell, lanczos3)") != std::string::npos);

    doc = base_json();
    doc["cell_pixels"] = 256.5;
    CHECK(error_of(doc).find("cell_pixels: expected an integer") != std::string::npos);

    doc = base_json();
    doc["meters_per_pixel"] = "0.01";
    CHECK(error_of(doc).find("meters_per_pixel: expected a number") != std::string::npos);

    doc = base_json();
    doc["supersampling"] = 0;
    CHECK(error_of(doc).find("supersampling: must be between 1 and 16") != std::string::npos);

    doc = base_json();
    doc["elevation"] = 30;
    CHECK(error_of(doc).find("elevation: unknown field") != std::string::npos);

    doc = base_json();
    doc.erase("meters_per_pixel");
    CHECK(error_of(doc).find("meters_per_pixel: missing") != std::string::npos);

    doc = base_json();
    doc["version"] = 2;
    CHECK(error_of(doc).find("version: 2 is not supported (this build reads version 1)") !=
          std::string::npos);

    doc = base_json();
    doc["format"] = "something-else";
    CHECK(error_of(doc).find("format: expected \"pose-atlas-profile\"") != std::string::npos);
}

TEST_CASE("profile: every problem is reported at once") {
    json doc = base_json();
    doc["camera_distance_m"] = -1;
    doc["meters_per_pixel"] = 0;
    doc["zoom_set"] = json::array();
    try {
        parse_profile(doc.dump(), "test.json");
        FAIL("expected a ProfileError");
    } catch (const ProfileError& error) {
        CHECK(error.problems().size() == 3);
    }
}

TEST_CASE("profile: unreadable input") {
    CHECK_THROWS_WITH_AS(parse_profile("{not json", "bad.json"),
                         doctest::Contains("bad.json: not valid JSON"), ProfileError);
    CHECK_THROWS_WITH_AS(parse_profile("[1, 2]", "bad.json"),
                         doctest::Contains("expected a JSON object"), ProfileError);
    CHECK_THROWS_WITH_AS(load_profile("/nonexistent/profile.json"),
                         doctest::Contains("cannot open file"), ProfileError);
}
