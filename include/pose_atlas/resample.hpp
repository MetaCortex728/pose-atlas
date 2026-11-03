// SPDX-License-Identifier: Apache-2.0
// Downsampling of the supersampled render to the cell: premultiplied alpha,
// separable filter, double-precision accumulation, so that the result is the
// same on every run and does not depend on the GPU's filtering.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "pose_atlas/image.hpp"
#include "pose_atlas/profile.hpp"

namespace pose_atlas {

/// Linear-light RGBA with premultiplied alpha, rows top to bottom.
struct LinearImage {
    int width = 0;
    int height = 0;
    std::vector<float> rgba;
};

/// Filter kernel in output-pixel units and its support radius.
double filter_radius(ResampleFilter filter);
double filter_weight(ResampleFilter filter, double x);

/// Reduce `source` by an integer `factor` in both directions. Pixels outside
/// the source are transparent. The result is clamped (alpha to [0, 1],
/// colour to [0, alpha]), un-premultiplied, encoded as sRGB and rounded to
/// 8 bits; pixels whose alpha rounds to 0 become (0, 0, 0, 0).
Image downsample(const LinearImage& source, int factor, ResampleFilter filter);

/// Single-pixel outer outline with hard edges: pixels with alpha of at least
/// 128 become opaque, every other pixel with such a 4-neighbour becomes
/// `colour` (sRGB, opaque), and the rest becomes transparent.
void apply_outline(Image& image, const std::array<std::uint8_t, 3>& colour);

/// sRGB transfer function (IEC 61966-2-1), linear [0, 1] to encoded [0, 1].
double linear_to_srgb(double linear);
/// Its inverse, encoded [0, 1] to linear [0, 1].
double srgb_to_linear(double encoded);

}  // namespace pose_atlas
