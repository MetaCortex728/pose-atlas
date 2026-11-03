// SPDX-License-Identifier: Apache-2.0
// Manual corrections carried across re-renders of the generated rig.
#include <doctest.h>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "pose_atlas/corrections.hpp"
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
        const std::string p = std::string(POSE_ATLAS_TEST_OUTPUT_DIR) + "/corrections_rig.glb";
        test_rig::write_glb(p);
        return p;
    }();
    return path;
}

AtlasJob rig_job(int idle_poses, int run_poses) {
    AtlasJob job;
    job.model_path = rig_path();
    job.profile = pose_atlas_test::small_profile();
    for (const auto& [action, poses] : {std::pair<const char*, int>{"idle", idle_poses}, {"run", run_poses}}) {
        SamplingSpec spec;
        spec.action = action;
        spec.pose_count = poses;
        job.actions.push_back(spec);
    }
    return job;
}

// A 3 x 3 block in a palette colour at (x, y), with one pixel of the second
// colour: what an artist paints on the corrections layer.
AsepriteCel painted_cel(int x, int y) {
    AsepriteCel cel;
    cel.layer = 1;
    cel.x = x;
    cel.y = y;
    cel.image = Image(3, 3);
    const Rgb8 a = test_rig::palette()[0], b = test_rig::palette()[1];
    for (int py = 0; py < 3; ++py) {
        for (int px = 0; px < 3; ++px) {
            const Rgb8& c = (px == 1 && py == 1) ? b : a;
            std::uint8_t* p = cel.image.pixel(px, py);
            p[0] = c[0];
            p[1] = c[1];
            p[2] = c[2];
            p[3] = 255;
        }
    }
    return cel;
}

// Paints into the corrections layer of the given frames with the library's
// writer.
void paint(const std::string& dir, const std::vector<int>& frames) {
    AsepriteDocument doc = read_aseprite(read_file(dir + "/atlas.aseprite"));
    for (int f : frames) doc.frames[static_cast<std::size_t>(f)].cels.push_back(painted_cel(40 + f % 5, 30 + f % 7));
    write_file(dir + "/atlas.aseprite", write_aseprite(doc));
}

// Keys of the frames whose corrections cel holds something, in frame order.
std::vector<std::string> corrected_keys(const std::string& dir) {
    const AsepriteDocument doc = read_aseprite(read_file(dir + "/atlas.aseprite"));
    std::vector<std::string> out;
    for (std::size_t f = 0; f < doc.frames.size(); ++f) {
        for (const AsepriteCel& cel : doc.frames[f].cels) {
            if (cel.layer == 1) out.push_back(doc.frames[f].cels[0].user_text);
        }
    }
    return out;
}

std::vector<std::string> keys_of(const std::vector<CarriedCorrection>& list) {
    std::vector<std::string> out;
    for (const auto& c : list) out.push_back(c.key);
    return out;
}

// Re-render `job` into `dir` the way `pose-atlas render` does.
CorrectionsReport rerender(const AtlasJob& job, const std::string& dir, const std::string& cache, bool discard = false) {
    const ExistingCorrections existing = read_existing_corrections(dir, discard);
    Atlas atlas = render_atlas(pose_atlas_test::renderer(), job, cache);
    CorrectionsReport report = carry_corrections(existing, atlas);
    export_atlas(atlas, dir);
    write_text_file(dir + "/" + kCorrectionsReport, report.to_json());
    const ValidationReport check = validate_atlas(dir);
    for (const auto& p : check.problems) MESSAGE(p);
    CHECK(check.passed());
    return report;
}

}  // namespace

TEST_CASE("corrections: carried across re-renders by frame key") {
    const std::string dir = fresh_dir("corrections_carry");
    const std::string cache = dir + "/cache";
    AtlasJob job = rig_job(4, 4);  // 8 columns: idle0..3, run0..3 (0, 0.2, 0.4, 0.6 s)
    {
        const Atlas first = render_atlas(pose_atlas_test::renderer(), job, cache);
        export_atlas(first, dir);
    }
    // Nothing painted: no backup, nothing to carry.
    {
        const ExistingCorrections none = read_existing_corrections(dir, false);
        CHECK(none.backup.empty());
        CHECK(none.corrections.empty());
    }
    // Paint idle1.e, run1.e, run2.e and run2.ne.
    paint(dir, {1, 5, 6, 14});
    const auto painted_bytes = read_file(dir + "/atlas.aseprite");
    const AsepriteDocument painted = read_aseprite(painted_bytes);
    const std::string backup = corrections_backup_name(painted_bytes);
    CHECK(backup.size() == std::string("atlas.before-").size() + 12 + std::string(".aseprite").size());

    SUBCASE("unchanged inputs: all carried, none stale") {
        const CorrectionsReport report = rerender(job, dir, cache);
        MESSAGE("unchanged:\n" << report.to_json());
        CHECK(report.summary() == "corrections: 4 carried (0 stale), 0 orphaned; previous file kept as " + backup);
        CHECK(read_file(dir + "/" + backup) == painted_bytes);
        CHECK(keys_of(report.carried) ==
              std::vector<std::string>{"idle.e@0.500000", "run.e@0.200000", "run.e@0.400000", "run.ne@0.400000"});
        CHECK(report.stale_count() == 0);
        CHECK(report.orphaned.empty());
        // Same frames, same positions, same pixels; no stale marks.
        const AsepriteDocument after = read_aseprite(read_file(dir + "/atlas.aseprite"));
        for (int f : {1, 5, 6, 14}) {
            const auto& cels = after.frames[static_cast<std::size_t>(f)].cels;
            REQUIRE(cels.size() == 2);
            CHECK(cels[1].layer == 1);
            CHECK(cels[1].x == 40 + f % 5);
            CHECK(cels[1].y == 30 + f % 7);
            CHECK(cels[1].image == painted_cel(0, 0).image);
            CHECK(cels[1].user_text.empty());
            CHECK_FALSE(cels[1].user_colour.has_value());
            CHECK(flatten_layer(after, static_cast<std::size_t>(f), 1) ==
                  flatten_layer(painted, static_cast<std::size_t>(f), 1));
        }
        const json written = json::parse(read_text_file(dir + "/corrections-report.json"));
        CHECK(written["format"] == "pose-atlas-corrections-report");
        CHECK(written["backup"] == backup);
        CHECK(written["carried"].size() == 4);
        CHECK(written["carried"][1] == json({{"key", "run.e@0.200000"}, {"old_frame", 5}, {"frame", 5}}));
        // Deterministic: the same re-render again gives the same bytes.
        const auto once = read_file(dir + "/atlas.aseprite");
        const CorrectionsReport again = rerender(job, dir, cache);
        CHECK(again.carried.size() == 4);
        CHECK(read_file(dir + "/atlas.aseprite") == once);
    }

    SUBCASE("a changed pose count: surviving keys carried, others orphaned") {
        job.actions[1].pose_count = 2;  // run at 0 and 0.4 s: 6 columns
        const CorrectionsReport report = rerender(job, dir, cache);
        MESSAGE("run 4 -> 2 poses:\n" << report.to_json());
        CHECK(keys_of(report.carried) == std::vector<std::string>{"idle.e@0.500000", "run.e@0.400000", "run.ne@0.400000"});
        REQUIRE(report.carried.size() == 3);
        CHECK(report.carried[1].old_frame == 6);
        CHECK(report.carried[1].frame == 5);   // run1.e of 6 columns
        CHECK(report.carried[2].old_frame == 14);
        CHECK(report.carried[2].frame == 11);  // run1.ne
        CHECK(report.stale_count() == 0);
        REQUIRE(report.orphaned.size() == 1);
        CHECK(report.orphaned[0].key == "run.e@0.200000");
        CHECK(report.orphaned[0].old_frame == 5);
        CHECK(corrected_keys(dir) == std::vector<std::string>{"idle.e@0.500000", "run.e@0.400000", "run.ne@0.400000"});
        // The orphan stays in the backup.
        CHECK(read_file(dir + "/" + backup) == painted_bytes);
        CHECK(report.summary() == "corrections: 3 carried (0 stale), 1 orphaned; previous file kept as " + backup);
    }

    SUBCASE("a changed palette: carried and stale") {
        job.settings.palette = test_rig::palette();
        job.settings.palette_stage = PaletteStage::BeforeAfter;
        job.palette_path = "palettes/test-rig.json";
        const CorrectionsReport report = rerender(job, dir, cache);
        MESSAGE("palette added:\n" << report.to_json());
        CHECK(report.carried.size() == 4);
        CHECK(report.stale_count() == 4);
        CHECK(report.orphaned.empty());
        const json written = json::parse(read_text_file(dir + "/corrections-report.json"));
        CHECK(written["stale"].size() == 4);
        const AsepriteDocument after = read_aseprite(read_file(dir + "/atlas.aseprite"));
        for (int f : {1, 5, 6, 14}) {
            const auto& cels = after.frames[static_cast<std::size_t>(f)].cels;
            REQUIRE(cels.size() == 2);
            CHECK(cels[1].user_text == "stale: render changed");
            CHECK(cels[1].user_colour == kStaleCorrectionColour);
        }
    }

    SUBCASE("changed render settings (outline): carried and stale") {
        job.settings.style.outline = true;
        const CorrectionsReport report = rerender(job, dir, cache);
        CHECK(report.carried.size() == 4);
        CHECK(report.stale_count() == 4);
    }

    SUBCASE("--discard-corrections: backed up, not carried") {
        const CorrectionsReport report = rerender(job, dir, cache, true);
        CHECK(report.discarded);
        CHECK(report.carried.empty());
        CHECK(report.summary() == "corrections: discarded (--discard-corrections); previous file kept as " + backup);
        CHECK(read_file(dir + "/" + backup) == painted_bytes);
        CHECK(corrected_keys(dir).empty());
    }

    SUBCASE("an existing backup is never overwritten") {
        write_text_file(dir + "/" + backup, "keep me");
        const CorrectionsReport report = rerender(job, dir, cache);
        CHECK(report.carried.size() == 4);
        CHECK(read_text_file(dir + "/" + backup) == "keep me");
    }

    SUBCASE("files without frame keys: the old contract's order, or a refusal") {
        AsepriteDocument old = painted;
        for (auto& frame : old.frames) frame.cels[0].user_text.clear();
        const auto old_bytes = write_aseprite(old);
        write_file(dir + "/atlas.aseprite", old_bytes);
        const std::string old_backup = corrections_backup_name(old_bytes);
        // With the contract: keys recovered from its frame order.
        const ExistingCorrections recovered = read_existing_corrections(dir, false);
        CHECK(recovered.backup == old_backup);
        REQUIRE(recovered.corrections.size() == 4);
        CHECK(recovered.corrections[1].key == "run.e@0.200000");
        CHECK(recovered.corrections[3].key == "run.ne@0.400000");
        // A contract written before frame keys (no key, no render_sha256):
        // keys from action, view and source time, renders from the file.
        json contract = json::parse(read_text_file(dir + "/pose-contract.json"));
        for (auto& frame : contract["frames"]) {
            frame.erase("key");
            frame.erase("render_sha256");
        }
        write_text_file(dir + "/pose-contract.json", contract.dump());
        const ExistingCorrections v1 = read_existing_corrections(dir, false);
        REQUIRE(v1.corrections.size() == 4);
        CHECK(v1.corrections[2].key == "run.e@0.400000");
        CHECK(v1.corrections[2].render_sha256 == sha256_hex(old.frames[6].cels[0].image.rgba));
        // Without the contract: refused, naming the backup.
        fs::remove(dir + "/pose-contract.json");
        try {
            read_existing_corrections(dir, false);
            FAIL("expected a CorrectionsError");
        } catch (const CorrectionsError& e) {
            const std::string what = e.what();
            MESSAGE(what);
            CHECK(what.find("frame 1 has no frame key") != std::string::npos);
            CHECK(what.find("kept in " + old_backup) != std::string::npos);
            CHECK(what.find("--discard-corrections") != std::string::npos);
        }
        CHECK(fs::exists(dir + "/" + old_backup));
        // Discarding needs no keys.
        CHECK(read_existing_corrections(dir, true).discarded);
    }
}
