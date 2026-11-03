// SPDX-License-Identifier: Apache-2.0
// From a model and a profile to an atlas: sampling each action, rendering
// every (action, view), laying the cells out (rows = views in heading order,
// columns = the poses of all actions in order), and exporting the atlas PNG,
// its Aseprite-style JSON, an editable .aseprite file and the pose contract.
#pragma once

#include <string>
#include <vector>

#include "pose_atlas/aseprite.hpp"
#include "pose_atlas/image.hpp"
#include "pose_atlas/model.hpp"
#include "pose_atlas/profile.hpp"
#include "pose_atlas/render.hpp"
#include "pose_atlas/sampling.hpp"

namespace pose_atlas {

/// Output file names inside the output directory.
inline constexpr const char* kAtlasPng = "atlas.png";
inline constexpr const char* kAtlasJson = "atlas.json";
inline constexpr const char* kAtlasAseprite = "atlas.aseprite";
inline constexpr const char* kAsepriteBaseLayer = "Render";
inline constexpr const char* kAsepriteCorrectionsLayer = "Manual corrections";
inline constexpr const char* kContractJson = "pose-contract.json";
inline constexpr const char* kContractFormat = "pose-atlas-contract";
inline constexpr int kContractVersion = 1;

struct AtlasJob {
    std::string model_path;
    Profile profile;
    std::vector<SamplingSpec> actions;
    RenderSettings settings;
    /// Where settings.palette came from, for the contract (may be empty).
    std::string palette_path;
};

/// What happened to one (action, view) render.
struct RenderRecord {
    std::string action;
    std::string view;
    std::string key;      // content hash of the render's inputs
    bool reused = false;  // taken from the cache instead of rendered
};

struct Atlas {
    // Inputs, as recorded in the contract.
    std::string model_path;
    std::string model_sha256;
    Profile profile;
    std::string profile_sha256;  // of profile_to_json(profile)
    RenderSettings settings;
    std::string settings_sha256;  // of render_settings_json(settings)
    std::string palette_path;     // from the job
    std::string renderer;         // Renderer::description()
    // Layout.
    std::vector<SamplingPlan> plans;               // one per action, column order
    std::vector<std::vector<int>> durations_ms;    // per action, per pose
    std::vector<std::string> pose_names;           // one per column: action + pose index
    int columns = 0;
    std::vector<Image> cells;                      // index = view * columns + column
    std::vector<RenderRecord> renders;             // one per (action, view)

    const Image& cell(int view, int column) const {
        return cells[static_cast<std::size_t>(view * columns + column)];
    }
};

/// Integer runtime durations (milliseconds, at least 1) of a plan's poses,
/// rounded cumulatively so that they add up to the rounded cycle. Throws
/// SamplingError when the cadence gives no durations (distance without a
/// reference speed).
std::vector<int> frame_durations_ms(const SamplingPlan& plan);

/// Content key of one (action, view) render: SHA-256 over the model file's
/// hash, the profile's hash, the render settings' hash, the renderer, the
/// action name, its sampled source times and the view.
std::string render_key(const Atlas& atlas, const SamplingPlan& plan, int view);

/// Sample and render every action in eight views. With a non-empty
/// `cache_dir` (created if missing), each (action, view) is first looked up
/// by its render key: a stored strip of that action's poses in that view is
/// reused, otherwise the poses are rendered and the strip is stored as
/// `<key>.png`. Atlas::renders reports which renders were reused.
Atlas render_atlas(Renderer& renderer, const AtlasJob& job, const std::string& cache_dir = "");

/// The whole atlas image.
Image atlas_image(const Atlas& atlas);

/// Aseprite-style JSON (json-array format) for an atlas image file name.
std::string atlas_metadata_json(const Atlas& atlas, const std::string& image_file);

/// The atlas as an Aseprite document: one frame per cell in atlas order (view
/// by view, the poses of each view in column order) with its duration, a
/// base layer "Render" holding the cell, an empty layer "Manual corrections"
/// above it, one tag per state ("<action>.<view>" over that action's poses in
/// that view) and a "foot" slice over the cell with the pivot.
AsepriteDocument aseprite_document(const Atlas& atlas);

/// Write atlas.png, atlas.json, atlas.aseprite and pose-contract.json into
/// `directory` (which must exist). Returns the files written.
std::vector<std::string> export_atlas(const Atlas& atlas, const std::string& directory);

/// Validation of an exported directory: the rules of the prototype's
/// check-pose-pipeline.py that apply to this pipeline.
struct FrameStats {
    std::string name;
    Bounds bounds;
    int duration_ms = 0;
};

struct ValidationReport {
    std::vector<std::string> problems;
    std::vector<FrameStats> frames;
    bool passed() const { return problems.empty(); }
    std::string to_json() const;
};

ValidationReport validate_atlas(const std::string& directory);

}  // namespace pose_atlas
