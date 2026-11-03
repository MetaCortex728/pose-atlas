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
