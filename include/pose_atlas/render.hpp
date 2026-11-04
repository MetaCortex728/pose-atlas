// SPDX-License-Identifier: Apache-2.0
// Headless renderer: OpenGL 3.3 core through EGL on Mesa's software device
// (llvmpipe), so that output does not depend on the machine's GPU.
//
// For each screen heading the orthographic camera of the profile's camera
// guide looks at the posed (CPU-skinned) model. The image is rendered at
// cell x supersampling pixels into a float target in linear light, then
// downsampled with the profile's filter on premultiplied alpha to the cell.
// The same inputs give the same pixels.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "pose_atlas/camera.hpp"
#include "pose_atlas/image.hpp"
#include "pose_atlas/model.hpp"
#include "pose_atlas/palette.hpp"
#include "pose_atlas/profile.hpp"
#include "pose_atlas/resample.hpp"

namespace pose_atlas {

/// Style: flat region colours, the base colour of each material (factor
/// times base colour texture), without lighting. Version this when the
/// meaning of the fields changes.
struct RenderStyle {
    bool outline = false;
    std::array<std::uint8_t, 3> outline_colour = {0x1a, 0x14, 0x1e};  // sRGB
};

/// Sampling of base colour textures at the supersampled resolution, without
/// mipmaps. Nearest keeps texels hard-edged.
enum class TextureFilter { Nearest, Bilinear };
std::string to_string(TextureFilter filter);
/// Parses "nearest" or "bilinear"; throws std::invalid_argument otherwise.
TextureFilter parse_texture_filter(const std::string& text);

struct RenderSettings {
    RenderStyle style;
    TextureFilter texture_filter = TextureFilter::Nearest;
    /// Palette-locked output: a palette (empty = none; recorded by content
    /// hash) and where it is applied. With a palette the final cell's alpha
    /// is thresholded at `alpha_threshold` coverage to 0 or 255, transparent
    /// pixels are (0, 0, 0, 0), and the outline comes after the snap.
    std::vector<Rgb8> palette;
    PaletteStage palette_stage = PaletteStage::None;
    double alpha_threshold = 0.5;
    /// When not empty, draw only primitives with these material names.
    std::vector<std::string> only_materials;
    /// Never draw primitives with these material names.
    std::vector<std::string> hidden_materials;
};

/// Name of the style implementation; part of every cache key.
inline constexpr const char* kRenderStyleName = "flat-v2";

/// Canonical JSON of the settings (stable key order), used for hashing and
/// recorded in the contract. The palette appears as its content hash
/// (palette_sha256), or null.
std::string render_settings_json(const RenderSettings& settings);

/// Problems with the settings; empty when they are valid. A palette needs a
/// stage other than none and vice versa, 2 to 256 distinct colours, and an
/// alpha threshold in (0, 1].
std::vector<std::string> validate_render_settings(const RenderSettings& settings);

/// Whether the settings ask for palette-locked output.
inline bool palette_active(const RenderSettings& settings) { return settings.palette_stage != PaletteStage::None; }

class RenderError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Owns an EGL display and context. Create one and reuse it; construction
/// throws RenderError when no software EGL device is available.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// GL_RENDERER and GL_VERSION, e.g. "llvmpipe (LLVM 15.0.7, 256 bits);
    /// 4.5 (Core Profile) Mesa 23.2.1". Recorded in outputs and cache keys.
    const std::string& description() const;

    /// Supersampled linear-light premultiplied render, cell x supersampling
    /// pixels square.
    LinearImage render_linear(const Model& model, const Pose& pose, const Profile& profile,
                              const CameraGuide& guide, const RenderSettings& settings);

    /// Final cell: render_linear, [snap to the palette], downsample, [snap,
    /// threshold alpha], optional outline. Throws RenderError for invalid
    /// settings. With a palette, `stats` (when given) receives this cell's
    /// counts.
    Image render(const Model& model, const Pose& pose, const Profile& profile, const CameraGuide& guide,
                 const RenderSettings& settings, PaletteStats* stats = nullptr);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace pose_atlas
