// SPDX-License-Identifier: Apache-2.0
// Shared fixtures for tests that render: one renderer per test process and
// the generated rig loaded once.
#pragma once

#include <string>

#include "pose_atlas/model.hpp"
#include "pose_atlas/profile.hpp"
#include "pose_atlas/render.hpp"
#include "pose_atlas/test_rig.hpp"

namespace pose_atlas_test {

inline pose_atlas::Renderer& renderer() {
    static pose_atlas::Renderer instance;
    return instance;
}

inline const pose_atlas::Model& rig() {
    static const pose_atlas::Model model = pose_atlas::load_gltf(pose_atlas::test_rig::make_glb(), "test-rig.glb");
    return model;
}

/// The prototype's camera for models in glTF axes.
inline const pose_atlas::Profile& gltf_profile() {
    static const pose_atlas::Profile profile =
        pose_atlas::load_profile(std::string(POSE_ATLAS_SOURCE_DIR) + "/profiles/quarter-view-gltf.json");
    return profile;
}

/// The same camera and cell span at 96 px with 2x supersampling: for tests
/// that need many renders but not full-size cells.
inline const pose_atlas::Profile& small_profile() {
    static const pose_atlas::Profile profile = [] {
        pose_atlas::Profile p = gltf_profile();
        p.id = "quarter-view-30-gltf-small";
        p.cell_pixels = 96;
        p.ground_pivot_pixels = {48, 78};
        p.meters_per_pixel = gltf_profile().cell_span_m() / 96;
        p.supersampling = 2;
        return p;
    }();
    return profile;
}

}  // namespace pose_atlas_test
