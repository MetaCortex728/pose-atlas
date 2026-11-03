// SPDX-License-Identifier: Apache-2.0
// From a model and a profile to an atlas: sampling each action, rendering
// every (action, view), laying the cells out (rows = views in heading order,
// columns = the poses of all actions in order), and exporting the atlas PNG,
// its Aseprite-style JSON, an editable .aseprite file and the pose contract.
#pragma once

#include <stdexcept>
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
inline constexpr int kContractVersion = 2;

/// Errors in the atlas as a whole, such as two frames with the same key.
class AtlasError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct AtlasJob {
    std::string model_path;
    Profile profile;
    std::vector<SamplingSpec> actions;
    RenderSettings settings;
    /// Where settings.palette came from, for the contract (may be empty).
    std::string palette_path;
};

/// A hand correction of one frame: a cell-sized image painted over the
/// render on the "Manual corrections" layer.
struct FrameCorrection {
    Image image;         // cell-sized; empty (0 x 0) = no correction
    bool stale = false;  // the render under it changed since it was painted
};

/// User data of a stale corrections cel in atlas.aseprite.
inline constexpr const char* kStaleCorrectionText = "stale: render changed";
inline constexpr AsepriteColour kStaleCorrectionColour = {0xff, 0x66, 0x00, 0xff};

/// What happened to one (action, view) render.
struct RenderRecord {
    std::string action;
    std::string view;
    std::string key;      // content hash of the render's inputs
    bool reused = false;  // taken from the cache instead of rendered
    PaletteStats palette; // summed over the poses; zero without a palette
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
    /// Hand corrections: empty, or one per frame in atlas order.
    std::vector<FrameCorrection> corrections;

    const Image& cell(int view, int column) const {
        return cells[static_cast<std::size_t>(view * columns + column)];
    }
    /// Palette statistics over every render.
    PaletteStats palette_total() const;
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

/// A source time in whole microseconds (rounded to nearest, halves away from
/// zero).
long long source_time_us(double source_time_s);

/// The key of a frame, independent of its position in the atlas:
/// "<action>.<view>@<source time in seconds with exactly six decimals>",
/// e.g. "run.se@0.416667" (source time quantised to whole microseconds, a
/// leading "-" for negative times).
std::string frame_key(const std::string& action, const std::string& view, double source_time_s);

/// One frame of an atlas, in atlas order (view by view, the poses of each
/// view in column order).
struct AtlasFrame {
    std::string name;     // "<pose>.<view>", e.g. "run3.nw"
    std::string key;      // frame_key()
    std::size_t action = 0;
    int pose = 0;
    int view = 0;
    int column = 0;
};

/// Every frame of an atlas (needs only its plans and pose names). Throws
/// AtlasError when two frames would share a key.
std::vector<AtlasFrame> atlas_frames(const Atlas& atlas);

/// SHA-256 of everything that determines one frame's render: the model
/// file's hash, the profile's hash, the render settings' hash, the renderer,
/// the action, the view and the exact source time.
std::string frame_input_sha256(const Atlas& atlas, const AtlasFrame& frame);
std::string frame_input_sha256(const std::string& model_sha256, const std::string& profile_sha256,
                               const std::string& settings_sha256, const std::string& renderer,
                               const std::string& action, const std::string& view, double source_time_s);

/// Sample and render every action in eight views (after checking that frame
/// keys are unique). With a non-empty
/// `cache_dir` (created if missing), each (action, view) is first looked up
/// by its render key: a stored strip of that action's poses in that view is
/// reused, otherwise the poses are rendered and the strip is stored as
/// `<key>.png`. Atlas::renders reports which renders were reused. With a
/// palette, each entry also keeps its palette statistics in
/// `<key>.palette.json`; an entry without them is rendered again.
Atlas render_atlas(Renderer& renderer, const AtlasJob& job, const std::string& cache_dir = "");

/// The whole atlas image.
Image atlas_image(const Atlas& atlas);

/// Aseprite-style JSON (json-array format) for an atlas image file name.
std::string atlas_metadata_json(const Atlas& atlas, const std::string& image_file);

/// The atlas as an Aseprite document: one frame per cell in atlas order (view
/// by view, the poses of each view in column order) with its duration, a
/// base layer "Render" holding the cell (its user data text is the frame's
/// key), a layer "Manual corrections" above it holding the atlas's
/// corrections (trimmed to their pixels; a stale one has the user data text
/// kStaleCorrectionText and colour kStaleCorrectionColour), one tag per state ("<action>.<view>" over that action's poses in
/// that view) and a "foot" slice over the cell with the pivot.
AsepriteDocument aseprite_document(const Atlas& atlas);

/// One line summarising the palette statistics of an atlas with a palette.
std::string palette_summary(const Atlas& atlas);

/// Write atlas.png, atlas.json, atlas.aseprite and pose-contract.json into
/// `directory` (which must exist). Returns the files written. With a palette
/// the contract has a `palette_report` with the statistics per (action,
/// view) and in total.
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
