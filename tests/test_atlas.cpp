// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "aseprite_parser.hpp"
#include "pose_atlas/atlas.hpp"
#include "pose_atlas/test_rig.hpp"
#include "render_support.hpp"

using namespace pose_atlas;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string fresh_dir(const std::string& name) {
    const fs::path dir = fs::path(POSE_ATLAS_TEST_OUTPUT_DIR) / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir.string();
}

std::string rig_path() {
    static const std::string path = [] {
        const std::string p = std::string(POSE_ATLAS_TEST_OUTPUT_DIR) + "/atlas_rig.glb";
        test_rig::write_glb(p);
        return p;
    }();
    return path;
}

AtlasJob rig_job(int poses = 8) {
    AtlasJob job;
    job.model_path = rig_path();
    job.profile = pose_atlas_test::gltf_profile();
    for (const char* action : {"idle", "run"}) {
        SamplingSpec spec;
        spec.action = action;
        spec.pose_count = poses;
        job.actions.push_back(spec);
    }
    return job;
}

const Atlas& rig_atlas() {
    static const Atlas atlas = render_atlas(pose_atlas_test::renderer(), rig_job());
    return atlas;
}

json read_json(const std::string& path) { return json::parse(read_text_file(path)); }

bool has_problem(const ValidationReport& report, const std::string& text) {
    for (const auto& p : report.problems) {
        if (p.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("atlas: frame durations round cumulatively") {
    SamplingPlan plan;
    plan.action = "a";
    for (int i = 0; i < 3; ++i) {
        Sample s;
        s.runtime_start_ms = i * 100.0 / 3.0;
        s.runtime_duration_ms = 100.0 / 3.0;
        plan.samples.push_back(s);
    }
    CHECK(frame_durations_ms(plan) == std::vector<int>{33, 34, 33});
    plan.samples[0].runtime_duration_ms.reset();
    CHECK_THROWS_AS(frame_durations_ms(plan), SamplingError);
}

TEST_CASE("atlas: the rig's two actions in eight views export and validate") {
    const Atlas& atlas = rig_atlas();
    CHECK(atlas.columns == 16);
    CHECK(atlas.cells.size() == 128);
    CHECK(atlas.renders.size() == 16);
    CHECK(atlas.pose_names.front() == "idle0");
    CHECK(atlas.pose_names.back() == "run7");
    CHECK(atlas.durations_ms[0] == std::vector<int>(8, 250));  // idle: 2 s / 8
    CHECK(atlas.durations_ms[1] == std::vector<int>(8, 100));  // run: 0.8 s / 8

    const std::string dir = fresh_dir("atlas_export");
    export_atlas(atlas, dir);
    const ValidationReport report = validate_atlas(dir);
    for (const auto& p : report.problems) MESSAGE(p);
    CHECK(report.passed());
    CHECK(report.frames.size() == 128);

    const Image image = decode_png(read_file(dir + "/atlas.png"));
    CHECK(image.width == 16 * 256);
    CHECK(image.height == 8 * 256);
    const json metadata = read_json(dir + "/atlas.json");
    CHECK(metadata["frames"][17]["filename"] == "idle1.ne");
    CHECK(metadata["frames"][17]["frame"] == json({{"x", 256}, {"y", 256}, {"w", 256}, {"h", 256}}));
    CHECK(metadata["frames"][17]["duration"] == 250);
    CHECK(metadata["meta"]["frameTags"].size() == 128);
    CHECK(metadata["meta"]["slices"][0]["keys"][0]["pivot"] == json({{"x", 128}, {"y", 208}}));
    const json contract = read_json(dir + "/pose-contract.json");
    CHECK(contract["sources"]["model"]["sha256"] == sha256_hex(read_file(rig_path())));
    CHECK(contract["sources"]["profile"]["id"] == "quarter-view-30-gltf");
    CHECK(contract["actions"][1]["clocks"]["runtime"]["frame_cycle_ms"] == 800);
    CHECK(contract["frames"][8]["source_time_s"] == doctest::Approx(0.0));
    CHECK(contract["frames"][9]["source_time_s"] == doctest::Approx(0.1));

    // The .aseprite file, read with the tests' own parser: one frame per
    // pose and view with its duration, two layers, one tag per state.
    const auto raw = pose_atlas_test::parse_raw_aseprite(read_file(dir + "/atlas.aseprite"));
    CHECK(raw.width == 256);
    CHECK(raw.frames == 128);
    CHECK(raw.frame_list[0].duration == 250);
    CHECK(raw.frame_list[8].duration == 100);
    const AsepriteDocument doc = read_aseprite(read_file(dir + "/atlas.aseprite"));
    REQUIRE(doc.tags.size() == 16);
    CHECK(doc.tags[3].name == "run.ne");
    CHECK(doc.tags[3].from == 24);
    CHECK(doc.tags[3].to == 31);
    CHECK(doc.frames[24].cels[0].image == atlas.cell(1, 8));

    // Exporting again gives the same bytes.
    const std::string again = fresh_dir("atlas_export_again");
    export_atlas(atlas, again);
    CHECK(read_file(again + "/atlas.png") == read_file(dir + "/atlas.png"));
    CHECK(read_file(again + "/atlas.aseprite") == read_file(dir + "/atlas.aseprite"));
    CHECK(read_file(again + "/pose-contract.json") == read_file(dir + "/pose-contract.json"));
}

TEST_CASE("atlas: the validator finds broken exports") {
    const Atlas& atlas = rig_atlas();
    const std::string dir = fresh_dir("atlas_broken");

    SUBCASE("changed metadata") {
        export_atlas(atlas, dir);
        json metadata = read_json(dir + "/atlas.json");
        metadata["frames"][3]["duration"] = 999;
        metadata["meta"]["frameTags"][5]["to"] = 6;
        metadata["meta"]["slices"][0]["keys"][0]["pivot"]["x"] = 127;
        write_text_file(dir + "/atlas.json", metadata.dump());
        const auto report = validate_atlas(dir);
        CHECK(has_problem(report, "atlas.json: SHA-256 differs"));
        CHECK(has_problem(report, "frame 3 (idle3.e): duration differs from the contract"));
        CHECK(has_problem(report, "frame 5 (idle5.e): Tag was extended"));
        CHECK(has_problem(report, "foot slice: expected one key on frame 0 with the profile's pivot"));
    }
    SUBCASE("clipped, blank and duplicate cells") {
        Atlas broken = atlas;
        broken.cells[0].pixel(0, 100)[3] = 255;                 // idle0.e touches the edge
        broken.cells[1] = Image(256, 256);                      // idle1.e blank
        broken.cells[16 + 2] = broken.cells[2];                 // idle2.ne repeats idle2.e
        for (int c = 9; c < 16; ++c) broken.cells[static_cast<std::size_t>(32 + c)] = broken.cells[32 + 8];  // run in n
        export_atlas(broken, dir);
        const auto report = validate_atlas(dir);
        CHECK(has_problem(report, "frame 0 (idle0.e): Clipping"));
        CHECK(has_problem(report, "frame 1 (idle1.e): blank pose"));
        CHECK(has_problem(report, "Directions did not change: pose idle2 has identical views"));
        CHECK(has_problem(report, "Poses did not change: action run shows one image in view n"));
        CHECK_FALSE(has_problem(report, "action idle shows one image"));
        CHECK_FALSE(has_problem(report, "SHA-256"));
    }
    SUBCASE("changed .aseprite") {
        export_atlas(atlas, dir);
        AsepriteDocument doc = aseprite_document(atlas);
        doc.frames[7].cels[0].image.pixel(128, 100)[0] ^= 1;
        doc.frames[2].cels.push_back({1, 0, 0, Image(2, 2)});
        doc.tags.pop_back();
        write_file(dir + "/atlas.aseprite", write_aseprite(doc));
        const auto report = validate_atlas(dir);
        CHECK(has_problem(report, "atlas.aseprite: SHA-256 differs"));
        CHECK(has_problem(report, "frame 7 (idle7.e): Import/export changed raw RGBA pixels"));
        CHECK(has_problem(report, "frame 2 has a cel on the corrections layer"));
        CHECK(has_problem(report, "tags are not one per state"));
    }
    SUBCASE("contract inconsistent with its profile") {
        export_atlas(atlas, dir);
        json contract = read_json(dir + "/pose-contract.json");
        contract["pivot"] = {120, 208};
        contract["cell_span_m"] = 2.0;
        contract["frames"][9]["projected_heading"] = {0.0, 1.0};
        write_text_file(dir + "/pose-contract.json", contract.dump());
        const auto report = validate_atlas(dir);
        CHECK(has_problem(report, "contract: pivot does not match"));
        CHECK(has_problem(report, "Physical framing changed"));
        CHECK(has_problem(report, "frame 9 (run1.e): Facing does not project"));
    }
    SUBCASE("missing or foreign files") {
        CHECK(has_problem(validate_atlas(dir), "cannot continue"));
        write_text_file(dir + "/pose-contract.json", "{\"format\": \"other\"}");
        CHECK(has_problem(validate_atlas(dir), "not a pose-atlas-contract"));
    }
}
