// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "aseprite_parser.hpp"
#include "pose_atlas/atlas.hpp"
#include "pose_atlas/camera.hpp"
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

AtlasJob rig_job(int poses = 8, bool small = false) {
    AtlasJob job;
    job.model_path = rig_path();
    job.profile = small ? pose_atlas_test::small_profile() : pose_atlas_test::gltf_profile();
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

const Atlas& small_rig_atlas() {
    static const Atlas atlas = render_atlas(pose_atlas_test::renderer(), rig_job(8, true));
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

namespace {

// An atlas with plans but no renders: enough for frame keys.
Atlas planned_atlas(int idle_poses, int run_poses) {
    Atlas atlas;
    for (const auto& [action, poses] : {std::pair<const char*, int>{"idle", idle_poses}, {"run", run_poses}}) {
        SamplingSpec spec;
        spec.action = action;
        spec.pose_count = poses;
        atlas.plans.push_back(plan_sampling(pose_atlas_test::rig(), spec));
        for (const Sample& s : atlas.plans.back().samples) atlas.pose_names.push_back(action + std::to_string(s.pose_index));
    }
    atlas.columns = static_cast<int>(atlas.pose_names.size());
    return atlas;
}

std::map<std::string, std::string> keys_by_name(const Atlas& atlas) {
    std::map<std::string, std::string> out;
    for (const AtlasFrame& f : atlas_frames(atlas)) out[f.name] = f.key;
    return out;
}

}  // namespace

TEST_CASE("atlas: frame keys") {
    CHECK(source_time_us(0.4166666666) == 416667);
    CHECK(source_time_us(-0.0000005) == -1);
    CHECK(frame_key("run", "se", 5.0 / 12.0) == "run.se@0.416667");
    CHECK(frame_key("idle", "e", 0.0) == "idle.e@0.000000");
    CHECK(frame_key("idle", "e", -0.0) == "idle.e@0.000000");
    CHECK(frame_key("jump", "nw", 12.5) == "jump.nw@12.500000");
    CHECK(frame_key("jump", "nw", -0.25) == "jump.nw@-0.250000");
    CHECK(frame_key("a", "n", 0.1 + 0.2) == frame_key("a", "n", 0.3));  // last-bit differences vanish
    CHECK_THROWS_AS(frame_key("a", "n", std::nan("")), AtlasError);

    // 8 + 8 poses: 128 distinct keys in atlas order.
    const Atlas base = planned_atlas(8, 8);
    const auto frames = atlas_frames(base);
    REQUIRE(frames.size() == 128);
    CHECK(frames[0].key == "idle.e@0.000000");
    CHECK(frames[9].name == "run1.e");
    CHECK(frames[9].key == "run.e@0.100000");
    CHECK(frames[127].key == "run.se@0.700000");
    std::set<std::string> distinct;
    for (const auto& f : frames) distinct.insert(f.key);
    CHECK(distinct.size() == 128);

    // A change of pose count in another action leaves the keys of this one.
    const auto before = keys_by_name(base);
    const auto other = keys_by_name(planned_atlas(8, 6));
    for (const auto& [name, key] : before) {
        if (name.rfind("idle", 0) == 0) CHECK(other.at(name) == key);
    }
    // A change of this action's pose count keeps the keys of poses whose
    // source time is unchanged: run 8 -> 4 samples 0, 0.2, 0.4, 0.6 s, all
    // of which run 8 had (as run0, run2, run4, run6).
    const auto fewer = keys_by_name(planned_atlas(8, 4));
    for (const char* view : {"e", "ne", "n", "nw", "w", "sw", "s", "se"}) {
        for (int pose = 0; pose < 4; ++pose) {
            CHECK(fewer.at("run" + std::to_string(pose) + "." + view) ==
                  before.at("run" + std::to_string(2 * pose) + "." + view));
        }
    }
    // run 8 -> 6 samples 0, 0.1333, 0.2667, 0.4, ...: run0 and run3 keep
    // the keys of run0 and run4 (0 and 0.4 s, computed differently).
    const auto six = keys_by_name(planned_atlas(8, 6));
    CHECK(six.at("run3.sw") == before.at("run4.sw"));
    CHECK(six.at("run3.sw") == "run.sw@0.400000");
    CHECK(six.at("run0.sw") == before.at("run0.sw"));
    CHECK(six.at("run1.sw") == "run.sw@0.133333");

    // Poses closer than a microsecond collide: refused with a clear error.
    AtlasJob job = rig_job(4, true);
    job.actions[1].start_s = 0.0;
    job.actions[1].end_s = 0.0000016;  // 0, 0.4, 0.8, 1.2 microseconds
    job.actions[1].cadence.kind = Cadence::Kind::FixedFrame;
    job.actions[1].cadence.frame_ms = 100;
    try {
        render_atlas(pose_atlas_test::renderer(), job);
        FAIL("expected an AtlasError");
    } catch (const AtlasError& e) {
        CHECK(std::string(e.what()).find("frames run0.e and run1.e would share the key \"run.e@0.000000\"") !=
              std::string::npos);
    }
}

TEST_CASE("atlas: corrections composite over the render") {
    Image render(3, 1), correction(3, 1);
    for (int x = 0; x < 3; ++x) {
        std::uint8_t* p = render.pixel(x, 0);
        p[0] = 200;
        p[1] = 100;
        p[2] = 50;
        p[3] = 255;
    }
    // x = 0: transparent (the render shows); x = 1: opaque paint; x = 2:
    // the erase colour.
    correction.pixel(1, 0)[1] = 7;
    correction.pixel(1, 0)[3] = 255;
    std::uint8_t* erase = correction.pixel(2, 0);
    erase[0] = 0xff;
    erase[2] = 0xff;
    erase[3] = 255;
    const Image out = composite_cell(render, correction, kDefaultEraseColour);
    CHECK(std::vector<std::uint8_t>(out.pixel(0, 0), out.pixel(0, 0) + 4) == std::vector<std::uint8_t>{200, 100, 50, 255});
    CHECK(std::vector<std::uint8_t>(out.pixel(1, 0), out.pixel(1, 0) + 4) == std::vector<std::uint8_t>{0, 7, 0, 255});
    CHECK(std::vector<std::uint8_t>(out.pixel(2, 0), out.pixel(2, 0) + 4) == std::vector<std::uint8_t>{0, 0, 0, 0});
    // Another erase colour: magenta is paint.
    const Image other = composite_cell(render, correction, Rgb8{1, 2, 3});
    CHECK(std::vector<std::uint8_t>(other.pixel(2, 0), other.pixel(2, 0) + 4) ==
          std::vector<std::uint8_t>{255, 0, 255, 255});
    // Partial alpha blends normally (invalid in an atlas, but defined).
    correction.pixel(1, 0)[3] = 51;
    const Image blended = composite_cell(render, correction, kDefaultEraseColour);
    CHECK(std::vector<std::uint8_t>(blended.pixel(1, 0), blended.pixel(1, 0) + 4) ==
          std::vector<std::uint8_t>{160, 81, 40, 255});
    CHECK(composite_cell(render, Image(), kDefaultEraseColour) == render);
    CHECK_THROWS(composite_cell(render, Image(2, 1), kDefaultEraseColour));

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
    // One tag per state (2 actions x 8 views), the same as in the .aseprite.
    const json& tags = metadata["meta"]["frameTags"];
    REQUIRE(tags.size() == 16);
    CHECK(tags[0] == json({{"name", "idle.e"}, {"from", 0}, {"to", 7}, {"direction", "forward"}, {"color", "#000000ff"}}));
    CHECK(tags[3]["name"] == "run.ne");
    CHECK(tags[3]["from"] == 24);
    CHECK(tags[3]["to"] == 31);
    CHECK(tags[15]["name"] == "run.se");
    CHECK(tags[15]["to"] == 127);
    CHECK(metadata["meta"]["slices"][0]["keys"][0]["pivot"] == json({{"x", 128}, {"y", 208}}));
    const json contract = read_json(dir + "/pose-contract.json");
    CHECK(contract["sources"]["model"]["sha256"] == sha256_hex(read_file(rig_path())));
    CHECK(contract["sources"]["profile"]["id"] == "quarter-view-30-gltf");
    CHECK_FALSE(contract["sources"].contains("palette"));
    CHECK_FALSE(contract.contains("palette_report"));
    CHECK(contract["actions"][1]["clocks"]["runtime"]["frame_cycle_ms"] == 800);
    CHECK(contract["frames"][8]["source_time_s"] == doctest::Approx(0.0));
    CHECK(contract["frames"][9]["source_time_s"] == doctest::Approx(0.1));
    CHECK(contract["version"] == 2);
    CHECK(contract["erase_colour"] == "#ff00ff");
    CHECK(contract["frames"][9]["key"] == "run.e@0.100000");
    CHECK(contract["frames"][9]["render_sha256"] == sha256_hex(atlas.cell(0, 9).rgba));
    CHECK(contract["frames"][9]["input_sha256"].get<std::string>().size() == 64);
    CHECK(contract["frames"][9]["input_sha256"] != contract["frames"][8]["input_sha256"]);

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
    // Each Render cel carries its frame's key as user data.
    for (std::size_t i = 0; i < doc.frames.size(); ++i) {
        REQUIRE(doc.frames[i].cels.size() == 1);
        CHECK(doc.frames[i].cels[0].user_text == contract["frames"][i]["key"]);
    }
    for (std::size_t i = 0; i < doc.tags.size(); ++i) {
        CHECK(tags[i]["name"] == doc.tags[i].name);
        CHECK(tags[i]["from"] == doc.tags[i].from);
        CHECK(tags[i]["to"] == doc.tags[i].to);
    }

    // Exporting again gives the same bytes.
    const std::string again = fresh_dir("atlas_export_again");
    export_atlas(atlas, again);
    CHECK(read_file(again + "/atlas.png") == read_file(dir + "/atlas.png"));
    CHECK(read_file(again + "/atlas.aseprite") == read_file(dir + "/atlas.aseprite"));
    CHECK(read_file(again + "/pose-contract.json") == read_file(dir + "/pose-contract.json"));
    // Without corrections atlas.png is the render alone: empty corrections
    // give the same bytes.
    Atlas uncorrected = atlas;
    uncorrected.corrections.resize(uncorrected.cells.size());
    const std::string empty = fresh_dir("atlas_export_empty_corrections");
    export_atlas(uncorrected, empty);
    CHECK(read_file(empty + "/atlas.png") == read_file(dir + "/atlas.png"));
    CHECK(read_file(empty + "/atlas.aseprite") == read_file(dir + "/atlas.aseprite"));
}

TEST_CASE("atlas: the validator finds broken exports") {
    const Atlas& atlas = small_rig_atlas();
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
        CHECK(has_problem(report, "atlas.json: frameTags are not one per state (16 tags"));
        CHECK(has_problem(report, "foot slice: expected one key on frame 0 with the profile's pivot"));
    }
    SUBCASE("clipped, blank and duplicate cells") {
        Atlas broken = atlas;
        broken.cells[0].pixel(0, 40)[3] = 255;                  // idle0.e touches the edge
        broken.cells[1] = Image(96, 96);                        // idle1.e blank
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
        doc.frames[7].cels[0].image.pixel(48, 40)[0] ^= 1;
        AsepriteCel correction;
        correction.layer = 1;
        correction.x = 30;
        correction.y = 31;
        correction.image = Image(2, 2);
        correction.image.pixel(1, 0)[3] = 100;
        doc.frames[2].cels.push_back(correction);
        doc.tags.pop_back();
        write_file(dir + "/atlas.aseprite", write_aseprite(doc));
        const auto report = validate_atlas(dir);
        CHECK(has_problem(report, "atlas.aseprite: SHA-256 differs"));
        CHECK(has_problem(report, "frame 7 (idle7.e): Import/export changed raw RGBA pixels"));
        CHECK(has_problem(report, "frame 7 (idle7.e): atlas.png is not the composite"));
        CHECK(has_problem(report, "frame 2 (idle2.e): corrections pixel (31, 31) has alpha 100"));
        CHECK(has_problem(report, "atlas.aseprite: 1 corrections pixels with partial alpha"));
        CHECK(has_problem(report, "frame 2 (idle2.e): atlas.png is not the composite of the atlas.aseprite layers "
                                  "Render and Manual corrections (erase colour #ff00ff)"));
        CHECK(has_problem(report, "tags are not one per state"));
        CHECK_FALSE(has_problem(report, "frame 3"));
    }
    SUBCASE("corrections in atlas.png") {
        // A valid correction is accepted only when atlas.png shows it.
        Atlas corrected = atlas;
        corrected.corrections.resize(corrected.cells.size());
        Image& image = corrected.corrections[4].image;
        image = Image(96, 96);
        for (int x = 40; x < 44; ++x) {
            std::uint8_t* p = image.pixel(x, 50);
            p[0] = 10;
            p[3] = 255;
        }
        export_atlas(corrected, dir);
        CHECK(validate_atlas(dir).passed());
        const auto png = read_file(dir + "/atlas.png");
        fs::create_directories(dir + "/plain");
        export_atlas(atlas, dir + "/plain");
        // Replace atlas.png (and its hash) by the render without corrections.
        json contract = read_json(dir + "/pose-contract.json");
        fs::copy_file(dir + "/plain/atlas.png", dir + "/atlas.png", fs::copy_options::overwrite_existing);
        contract["files"]["atlas"]["sha256"] = sha256_hex(read_file(dir + "/atlas.png"));
        contract.erase("erase_colour");
        write_text_file(dir + "/pose-contract.json", contract.dump());
        const auto report = validate_atlas(dir);
        CHECK(has_problem(report, "frame 4 (idle4.e): atlas.png is not the composite"));
        CHECK(has_problem(report, "contract: erase_colour is missing"));
        CHECK(png != read_file(dir + "/atlas.png"));
    }
    SUBCASE("contract inconsistent with its profile") {
        export_atlas(atlas, dir);
        json contract = read_json(dir + "/pose-contract.json");
        contract["pivot"] = {40, 78};
        contract["cell_span_m"] = 2.0;
        contract["frames"][9]["projected_heading"] = {0.0, 1.0};
        contract["frames"][10]["key"] = "run.e@0.000000";
        contract["frames"][11]["input_sha256"] = std::string(64, '0');
        contract["frames"][12]["render_sha256"] = std::string(64, '0');
        write_text_file(dir + "/pose-contract.json", contract.dump());
        const auto report = validate_atlas(dir);
        CHECK(has_problem(report, "contract: pivot does not match"));
        CHECK(has_problem(report, "Physical framing changed"));
        CHECK(has_problem(report, "frame 9 (run1.e): Facing does not project"));
        CHECK(has_problem(report, "frame 10 (run2.e): key \"run.e@0.000000\" is not \"run.e@0.200000\""));
        CHECK(has_problem(report, "frame 10 (run2.e): key \"run.e@0.000000\" is not unique"));
        CHECK(has_problem(report, "frame 10 (run2.e): atlas.aseprite Render cel's user data is \"run.e@0.200000\""));
        CHECK(has_problem(report, "frame 11 (run3.e): input_sha256 is not the hash"));
        CHECK(has_problem(report, "frame 12 (run4.e): Import/export changed raw RGBA pixels (the atlas.aseprite Render "
                                  "cel differs from the contract's render_sha256)"));
    }
    SUBCASE("missing or foreign files") {
        CHECK(has_problem(validate_atlas(dir), "cannot continue"));
        write_text_file(dir + "/pose-contract.json", "{\"format\": \"other\"}");
        CHECK(has_problem(validate_atlas(dir), "not a pose-atlas-contract"));
    }
}

TEST_CASE("atlas: the content-hash cache re-renders only what changed") {
    const std::string cache = fresh_dir("atlas_cache");
    auto& renderer = pose_atlas_test::renderer();
    auto reused = [](const Atlas& atlas, const std::string& action) {
        int n = 0;
        for (const auto& r : atlas.renders) n += (r.action == action && r.reused) ? 1 : 0;
        return n;
    };

    AtlasJob job = rig_job(4, true);
    const Atlas first = render_atlas(renderer, job, cache);
    CHECK(reused(first, "idle") == 0);
    CHECK(reused(first, "run") == 0);
    CHECK(std::distance(fs::directory_iterator(cache), fs::directory_iterator()) == 16);

    // Same inputs: everything comes from the cache, with the same pixels.
    const Atlas second = render_atlas(renderer, job, cache);
    CHECK(reused(second, "idle") == 8);
    CHECK(reused(second, "run") == 8);
    CHECK(second.cells == first.cells);

    // Change one action (its pose count): only that action is rendered again.
    job.actions[1].pose_count = 6;
    const Atlas third = render_atlas(renderer, job, cache);
    CHECK(reused(third, "idle") == 8);
    CHECK(reused(third, "run") == 0);
    CHECK(third.cell(3, 1) == first.cell(3, 1));
    // ... and it matches an uncached render.
    const Atlas uncached = render_atlas(renderer, job);
    CHECK(uncached.cells == third.cells);
    for (std::size_t i = 0; i < third.renders.size(); ++i) CHECK(third.renders[i].key == uncached.renders[i].key);

    // A different runtime cadence does not change the pixels: all reused.
    job.actions[0].cadence.kind = Cadence::Kind::FixedFrame;
    job.actions[0].cadence.frame_ms = 120;
    const Atlas fourth = render_atlas(renderer, job, cache);
    CHECK(reused(fourth, "idle") == 8);
    CHECK(reused(fourth, "run") == 8);
    CHECK(fourth.durations_ms[0] == std::vector<int>(4, 120));

    // Render settings and the profile key every render.
    job.settings.style.outline = true;
    CHECK(reused(render_atlas(renderer, job, cache), "idle") == 0);
    // So does the palette's content hash; the contract names the palette.
    job.settings = {};
    job.settings.palette = {{0x1a, 0x14, 0x1e}, {0xff, 0xff, 0xff}};
    job.settings.palette_stage = PaletteStage::BeforeAfter;
    job.palette_path = "two.json";
    const Atlas with_palette = render_atlas(renderer, job, cache);
    CHECK(reused(with_palette, "idle") == 0);
    CHECK(with_palette.renders[0].key != fourth.renders[0].key);
    const std::string palette_dir = fresh_dir("atlas_palette_contract");
    export_atlas(with_palette, palette_dir);
    const json contract = read_json(palette_dir + "/pose-contract.json");
    CHECK(contract["sources"]["palette"]["path"] == "two.json");
    CHECK(contract["sources"]["palette"]["sha256"] == palette_sha256(job.settings.palette));
    CHECK(contract["sources"]["palette"]["colors"] == json({"#1a141e", "#ffffff"}));
    CHECK(contract["sources"]["render_settings"]["values"]["palette_sha256"] == palette_sha256(job.settings.palette));
    job.palette_path.clear();
    job.settings = {};
    job.profile.resampling_filter = ResampleFilter::Box;
    CHECK(reused(render_atlas(renderer, job, cache), "idle") == 0);

    // A damaged entry is rendered again rather than trusted.
    job = rig_job(4, true);
    write_file(cache + "/" + first.renders[0].key + ".png", {1, 2, 3});
    const Atlas repaired = render_atlas(renderer, job, cache);
    CHECK(reused(repaired, "idle") == 7);
    CHECK(repaired.cells == first.cells);
}

TEST_CASE("atlas: palette report, before versus before_after") {
    // The open question: is snapping the supersampled render to the palette
    // (before) enough, or is a final snap after the Mitchell downsample also
    // needed (before_after)? Rendered with the rig's own colours.
    auto& renderer = pose_atlas_test::renderer();
    AtlasJob job = rig_job(4, true);
    job.settings.palette = test_rig::palette();
    job.palette_path = "palettes/test-rig.json";
    PaletteMatcher matcher(job.settings.palette);

    job.settings.palette_stage = PaletteStage::Before;
    const std::string cache = fresh_dir("atlas_palette_cache");
    const Atlas before = render_atlas(renderer, job, cache);
    job.settings.palette_stage = PaletteStage::BeforeAfter;
    const Atlas both = render_atlas(renderer, job);

    const PaletteStats b = before.palette_total(), ba = both.palette_total();
    MESSAGE("before:       " << palette_summary(before));
    MESSAGE("before_after: " << palette_summary(both));
    CHECK(b.opaque > 10000);
    // Before alone leaves off-palette pixels (the downsample blends regions);
    // with no final snap they reach the output.
    CHECK(b.off_palette_before_snap > 0);
    CHECK(b.off_palette_output == b.off_palette_before_snap);
    // The same pipeline up to the final snap, then none are left.
    CHECK(ba.off_palette_before_snap == b.off_palette_before_snap);
    CHECK(ba.partial_alpha == b.partial_alpha);
    CHECK(ba.off_palette_output == 0);
    CHECK(b.partial_alpha > 0);

    // Every off-palette pixel of `before` is a Mitchell blend at a region
    // border: the filter's footprint in the snapped supersampled render
    // (output pixel centre +- 2 px) holds more than one colour or coverage.
    // Pixels whose footprint is uniform are on the palette. Checked on the
    // first idle pose in all eight views.
    {
        const Profile& profile = job.profile;
        const int ss = profile.supersampling;
        const Model& model = pose_atlas_test::rig();
        const Pose pose = evaluate(model, &model.animation("idle"), 0.0);
        RenderSettings settings = job.settings;
        settings.palette_stage = PaletteStage::Before;
        int off = 0, off_uniform = 0, uniform_opaque = 0;
        for (int view = 0; view < 8; ++view) {
            const CameraGuide guide = camera_guide(profile, view);
            LinearImage snapped = renderer.render_linear(model, pose, profile, guide, settings);
            snap_linear(snapped, matcher);
            const Image cell = renderer.render(model, pose, profile, guide, settings);
            CHECK(cell == before.cell(view, 0));
            auto at = [&](int sx, int sy) {
                return snapped.rgba.data() +
                       (static_cast<std::size_t>(sy) * static_cast<std::size_t>(snapped.width) + static_cast<std::size_t>(sx)) * 4;
            };
            auto footprint = [&](int centre) {
                std::vector<int> taps;
                for (int k = (centre - 2) * ss; k < (centre + 3) * ss; ++k) {
                    if (std::fabs((k + 0.5) / ss - (centre + 0.5)) < 2.0 && k >= 0 && k < snapped.width) taps.push_back(k);
                }
                return taps;
            };
            for (int y = 0; y < cell.height; ++y) {
                const auto ys = footprint(y);
                for (int x = 0; x < cell.width; ++x) {
                    const std::uint8_t* p = cell.pixel(x, y);
                    if (p[3] == 0) continue;
                    const auto xs = footprint(x);
                    bool uniform = true;
                    const float* first = at(xs[0], ys[0]);
                    for (int sy : ys) {
                        for (int sx : xs) {
                            const float* q = at(sx, sy);
                            uniform = uniform && std::equal(first, first + 4, q);
                        }
                    }
                    const bool on_palette = matcher.contains({p[0], p[1], p[2]});
                    off += on_palette ? 0 : 1;
                    off_uniform += (!on_palette && uniform) ? 1 : 0;
                    uniform_opaque += uniform ? 1 : 0;
                }
            }
        }
        MESSAGE("idle0, 8 views: " << off << " off-palette pixels, " << off_uniform
                                   << " of them with a uniform footprint; " << uniform_opaque
                                   << " opaque pixels with a uniform footprint");
        CHECK(off > 0);
        CHECK(uniform_opaque > 0);
        CHECK(off_uniform == 0);
    }
    for (const Image& cell : both.cells) {
        for (int y = 0; y < cell.height; ++y) {
            for (int x = 0; x < cell.width; ++x) {
                const std::uint8_t* p = cell.pixel(x, y);
                CHECK((p[3] == 0 || p[3] == 255));
                if (p[3] == 255) CHECK(matcher.contains({p[0], p[1], p[2]}));
            }
        }
    }

    // Per (action, view) numbers add up; the contract carries them; the
    // cache keeps them.
    PaletteStats sum;
    for (const RenderRecord& r : before.renders) sum += r.palette;
    CHECK(sum == b);
    const std::string dir = fresh_dir("atlas_palette_report");
    export_atlas(before, dir);
    const json report = read_json(dir + "/pose-contract.json")["palette_report"];
    CHECK(report["stage"] == "before");
    CHECK(report["alpha_threshold"] == 0.5);
    CHECK(report["palette_sha256"] == palette_sha256(job.settings.palette));
    CHECK(report["outline_colour"].is_null());
    REQUIRE(report["renders"].size() == 16);
    CHECK(report["renders"][9]["action"] == "run");
    CHECK(report["renders"][9]["view"] == "ne");
    CHECK(report["renders"][9]["opaque"] == before.renders[9].palette.opaque);
    CHECK(report["total"]["off_palette_before_snap"] == b.off_palette_before_snap);
    CHECK(report["total"]["partial_alpha"] == b.partial_alpha);
    CHECK(report["total"]["off_palette_output"] == b.off_palette_output);

    // The palette-lock rule of validate: before fails it, before_after passes.
    const ValidationReport before_check = validate_atlas(dir);
    CHECK_FALSE(before_check.passed());
    CHECK(has_problem(before_check, "palette lock: atlas pixel ("));
    CHECK(has_problem(before_check, "palette lock: " + std::to_string(b.off_palette_output) +
                                        " opaque pixels off the palette and 0 pixels with partial alpha"));
    int listed = 0;
    for (const auto& p : before_check.problems) listed += p.find("palette lock: atlas pixel") == 0 ? 1 : 0;
    CHECK(listed == 5);
    const std::string both_dir = fresh_dir("atlas_palette_both");
    export_atlas(both, both_dir);
    const ValidationReport both_check = validate_atlas(both_dir);
    for (const auto& p : both_check.problems) MESSAGE(p);
    CHECK(both_check.passed());
    // A contract whose palette lost a colour: wrong hash, and offenders.
    json contract = read_json(both_dir + "/pose-contract.json");
    contract["sources"]["palette"]["colors"].erase(2);
    write_text_file(both_dir + "/pose-contract.json", contract.dump());
    const ValidationReport shrunk = validate_atlas(both_dir);
    CHECK(has_problem(shrunk, "sources.palette.sha256 is not the hash of its colours"));
    CHECK(has_problem(shrunk, std::string(" in frame ")));
    CHECK(has_problem(shrunk, hex_colour(job.settings.palette[2]) + " is not in the palette"));
    // Partial alpha is reported too.
    Atlas soft = both;
    soft.cells[3].pixel(40, 40)[3] = 128;
    export_atlas(soft, both_dir);
    CHECK(has_problem(validate_atlas(both_dir), "alpha 128 is neither 0 nor 255"));
    job.settings.palette_stage = PaletteStage::Before;
    const Atlas cached = render_atlas(renderer, job, cache);
    for (const RenderRecord& r : cached.renders) CHECK(r.reused);
    CHECK(cached.palette_total() == b);
    CHECK(cached.cells == before.cells);
    // An entry that lost its statistics is rendered again.
    fs::remove(cache + "/" + before.renders[0].key + ".palette.json");
    const Atlas repaired = render_atlas(renderer, job, cache);
    CHECK_FALSE(repaired.renders[0].reused);
    CHECK(repaired.renders[1].reused);
    CHECK(repaired.palette_total() == b);
}
