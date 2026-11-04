// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "cli_support.hpp"
#include "pose_atlas/aseprite.hpp"
#include "pose_atlas/image.hpp"
#include "pose_atlas/test_rig.hpp"

using pose_atlas_test::run_cli;

TEST_CASE("cli: version and help") {
    const auto version = run_cli("version");
    CHECK(version.exit_code == 0);
    CHECK(version.output == std::string("pose-atlas ") + POSE_ATLAS_EXPECTED_VERSION + "\n");

    const auto help = run_cli("help");
    CHECK(help.exit_code == 0);
    CHECK(help.output.find("usage: pose-atlas") != std::string::npos);

    const auto none = run_cli("");
    CHECK(none.exit_code == 2);

    const auto unknown = run_cli("frobnicate");
    CHECK(unknown.exit_code == 2);
    CHECK(unknown.output.find("unknown command 'frobnicate'") != std::string::npos);
}

TEST_CASE("cli: profile check and guides") {
    const std::string profile = std::string("\"") + POSE_ATLAS_SOURCE_DIR + "/profiles/quarter-view.json\"";
    const auto check = run_cli("profile check " + profile);
    CHECK(check.exit_code == 0);
    CHECK(check.output.find("valid profile \"quarter-view-30\"") != std::string::npos);

    const auto guides = run_cli("profile guides " + profile);
    CHECK(guides.exit_code == 0);
    CHECK(guides.output.find("\"direction\": \"se\"") != std::string::npos);

    const auto missing = run_cli("profile check /nonexistent.json");
    CHECK(missing.exit_code == 1);
    CHECK(missing.output.find("/nonexistent.json: cannot open file") != std::string::npos);

    const auto usage = run_cli("profile frob " + profile);
    CHECK(usage.exit_code == 2);
}

TEST_CASE("cli: make-test-rig writes the generated GLB") {
    const std::string path = std::string(POSE_ATLAS_TEST_OUTPUT_DIR) + "/cli_test_rig.glb";
    std::remove(path.c_str());
    const auto result = run_cli("make-test-rig \"" + path + "\"");
    CHECK(result.exit_code == 0);
    std::ifstream file(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    CHECK(bytes == pose_atlas::test_rig::make_glb());
}

TEST_CASE("cli: inspect and plan the generated rig") {
    const std::string path = std::string(POSE_ATLAS_TEST_OUTPUT_DIR) + "/cli_plan_rig.glb";
    pose_atlas::test_rig::write_glb(path);
    const std::string quoted = "\"" + path + "\"";

    const auto inspect = run_cli("inspect " + quoted);
    CHECK(inspect.exit_code == 0);
    CHECK(inspect.output.find("\"name\": \"run\"") != std::string::npos);

    const auto plan = run_cli("plan " + quoted + " --action run --poses 8 --cycle-m 1.6 --speed-mps 4");
    CHECK(plan.exit_code == 0);
    CHECK(plan.output.find("\"cadence\": \"distance\"") != std::string::npos);
    CHECK(plan.output.find("\"end_point\": \"exclude\"") != std::string::npos);

    const auto included = run_cli("plan " + quoted + " --action run --poses 9 --include-end");
    CHECK(included.exit_code == 0);
    CHECK(included.output.find("repeats the first") != std::string::npos);

    CHECK(run_cli("plan " + quoted + " --action run").exit_code == 2);
    CHECK(run_cli("plan " + quoted + " --action run --poses 8 --rate 2 --frame-ms 80").exit_code == 2);
    CHECK(run_cli("plan " + quoted + " --action run --poses x").exit_code == 2);
    const auto missing = run_cli("plan " + quoted + " --action walk --poses 8");
    CHECK(missing.exit_code == 1);
    CHECK(missing.output.find("no animation \"walk\"") != std::string::npos);
}

TEST_CASE("cli: render the rig's two actions and validate them") {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(POSE_ATLAS_TEST_OUTPUT_DIR) / "cli_render";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string rig = (root / "test-rig.glb").string();
    const std::string out = (root / "out").string();
    const std::string profile = std::string(POSE_ATLAS_SOURCE_DIR) + "/profiles/quarter-view-gltf.json";
    REQUIRE(run_cli("make-test-rig \"" + rig + "\"").exit_code == 0);
    const std::string base =
        "render \"" + rig + "\" --profile \"" + profile + "\" --actions idle,run --out \"" + out + "\"";

    const auto first = run_cli(base);
    CHECK(first.exit_code == 0);
    CHECK(first.output.find("renders: 16 rendered, 0 reused") != std::string::npos);
    CHECK(first.output.find("validated 128 frames (16 poses x 8 views)") != std::string::npos);
    for (const char* file : {"atlas.png", "atlas.json", "atlas.aseprite", "pose-contract.json"}) {
        CHECK(fs::exists(fs::path(out) / file));
    }
    const auto atlas_bytes = pose_atlas::read_file(out + "/atlas.png");

    const auto valid = run_cli("validate \"" + out + "\" --report \"" + (root / "report.json").string() + "\"");
    CHECK(valid.exit_code == 0);
    CHECK(valid.output.find("POSE_VALIDATION_PASS 128 frames") != std::string::npos);
    CHECK(fs::exists(root / "report.json"));

    // Unchanged inputs: everything reused, same atlas bytes.
    const auto again = run_cli(base);
    CHECK(again.exit_code == 0);
    CHECK(again.output.find("renders: 0 rendered, 16 reused") != std::string::npos);
    CHECK(pose_atlas::read_file(out + "/atlas.png") == atlas_bytes);

    // One action changed: only its eight views are rendered.
    const auto changed = run_cli(base + " --poses idle=8,run=6");
    CHECK(changed.exit_code == 0);
    CHECK(changed.output.find("renders: 8 rendered, 8 reused") != std::string::npos);
    CHECK(changed.output.find("idle: e=reused ne=reused") != std::string::npos);
    CHECK(changed.output.find("run: e=rendered") != std::string::npos);
    CHECK(run_cli("validate \"" + out + "\"").exit_code == 0);
    CHECK(changed.output.find("corrections: none to carry; report in ") != std::string::npos);

    // A hand correction on idle0.e survives a re-render; the previous file
    // is backed up.
    {
        pose_atlas::AsepriteDocument doc = pose_atlas::read_aseprite(pose_atlas::read_file(out + "/atlas.aseprite"));
        pose_atlas::AsepriteCel cel;
        cel.layer = 1;
        cel.x = 120;
        cel.y = 100;
        cel.image = pose_atlas::Image(2, 2);
        for (int i = 0; i < 4; ++i) cel.image.rgba[static_cast<std::size_t>(i) * 4 + 3] = 255;
        doc.frames[0].cels.push_back(cel);
        pose_atlas::write_file(out + "/atlas.aseprite", pose_atlas::write_aseprite(doc));
    }
    const auto carried = run_cli(base + " --poses idle=8,run=6");
    CHECK(carried.exit_code == 0);
    CHECK(carried.output.find("corrections: 1 carried (0 stale), 0 orphaned; previous file kept as atlas.before-") !=
          std::string::npos);
    CHECK(pose_atlas::read_text_file(out + "/corrections-report.json").find("\"key\": \"idle.e@0.000000\"") !=
          std::string::npos);
    const auto discarded = run_cli(base + " --poses idle=8,run=6 --discard-corrections");
    CHECK(discarded.exit_code == 0);
    CHECK(discarded.output.find("corrections: discarded (--discard-corrections); previous file kept as atlas.before-") !=
          std::string::npos);
    int backups = 0;
    for (const auto& entry : fs::directory_iterator(out)) {
        backups += entry.path().filename().string().rfind("atlas.before-", 0) == 0 ? 1 : 0;
    }
    // The re-render with the carried correction wrote the same bytes as the
    // hand-painted file, so both have the same (single) backup.
    CHECK(backups == 1);

    // A tampered export fails validation.
    pose_atlas::write_text_file(out + "/atlas.json", "{\"frames\": [], \"meta\": {}}");
    const auto broken = run_cli("validate \"" + out + "\"");
    CHECK(broken.exit_code == 1);
    CHECK(broken.output.find("POSE_VALIDATION_FAIL") != std::string::npos);

    CHECK(run_cli("render \"" + rig + "\" --actions idle --out x").exit_code == 2);
    CHECK(run_cli(base + " --poses walk=4").exit_code == 2);
    CHECK(run_cli(base + " --texture-filter trilinear").exit_code == 2);
    CHECK(run_cli(base + " --erase-color magenta").exit_code == 2);
    const std::string palette = " --palette \"" + std::string(POSE_ATLAS_SOURCE_DIR) + "/palettes/test-rig.json\"";
    CHECK(run_cli(base + " --palette-stage before").exit_code == 2);
    CHECK(run_cli(base + palette + " --palette-stage none").exit_code == 2);
    CHECK(run_cli(base + palette + " --palette-stage during").exit_code == 2);
    CHECK(run_cli(base + palette + " --alpha-threshold 0").exit_code == 2);
    CHECK(run_cli(base + palette + " --alpha-threshold 1.5").exit_code == 2);
    const std::string palette_out = (root / "palette").string();
    const std::string palette_base = "render \"" + rig + "\" --profile \"" + profile +
                                     "\" --actions idle,run --poses 2 --outline --out \"" + palette_out + "\"" + palette;
    const auto locked = run_cli(palette_base);
    CHECK(locked.exit_code == 0);
    CHECK(locked.output.find("palette: before_after, 16 colours: ") != std::string::npos);
    CHECK(locked.output.find(" 0 off-palette in the output") != std::string::npos);
    // The rig's palette holds the default erase colour (its pivot marker).
    CHECK(locked.output.find("warning: the erase colour #ff00ff is also a palette colour") != std::string::npos);
    const auto other_erase = run_cli(palette_base + " --erase-color \"#00FF01\"");
    CHECK(other_erase.exit_code == 0);
    CHECK(other_erase.output.find("warning: the erase colour") == std::string::npos);
    CHECK(pose_atlas::read_text_file(palette_out + "/pose-contract.json").find("\"erase_colour\": \"#00ff01\"") !=
          std::string::npos);
    CHECK(run_cli("validate \"" + palette_out + "\"").exit_code == 0);
    // Snapping only before the downsample is not palette-locked: the export
    // is written, but fails validation.
    const auto unlocked = run_cli(palette_base + " --palette-stage before");
    CHECK(unlocked.exit_code == 1);
    CHECK(unlocked.output.find("palette: before, 16 colours: ") != std::string::npos);
    CHECK(unlocked.output.find("validate: palette lock: atlas pixel (") != std::string::npos);
    const auto unlocked_check = run_cli("validate \"" + palette_out + "\"");
    CHECK(unlocked_check.exit_code == 1);
    CHECK(unlocked_check.output.find("opaque pixels off the palette") != std::string::npos);
    const auto bad_palette = run_cli(base + " --palette /nonexistent/palette.json");
    CHECK(bad_palette.exit_code == 1);
    CHECK(bad_palette.output.find("/nonexistent/palette.json: cannot open file") != std::string::npos);
    CHECK(run_cli("render \"" + rig + "\" --profile \"" + profile + "\" --actions jump --out \"" + out + "\"")
              .exit_code == 1);
    CHECK(run_cli("validate").exit_code == 2);
}
