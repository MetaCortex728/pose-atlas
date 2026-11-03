// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include "cli_support.hpp"

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
