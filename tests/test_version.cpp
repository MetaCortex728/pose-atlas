// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include "pose_atlas/version.hpp"

TEST_CASE("version matches the CMake project version") {
    CHECK(pose_atlas::version() == POSE_ATLAS_EXPECTED_VERSION);
}
