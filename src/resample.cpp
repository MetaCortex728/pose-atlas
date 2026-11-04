// SPDX-License-Identifier: Apache-2.0
#include "pose_atlas/resample.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pose_atlas {

namespace {

// Taps of one output sample along one axis.
struct Taps {
    int first = 0;                // first source index (may be out of range)
    std::vector<double> weights;  // normalised over all taps, in or out of range
};

std::vector<Taps> make_taps(int output_size, int factor, ResampleFilter filter) {
    const double radius = filter_radius(filter);
    std::vector<Taps> taps(static_cast<std::size_t>(output_size));
    for (int j = 0; j < output_size; ++j) {
        // Output pixel j is centred at j + 0.5, source pixel k at (k + 0.5) / factor,
        // both in output-pixel units.
        const double centre = j + 0.5;
        const int lo = static_cast<int>(std::floor((centre - radius) * factor - 0.5));
        const int hi = static_cast<int>(std::ceil((centre + radius) * factor - 0.5));
        Taps& t = taps[static_cast<std::size_t>(j)];
        t.first = lo;
        double sum = 0.0;
        for (int k = lo; k <= hi; ++k) {
            const double w = filter_weight(filter, (k + 0.5) / factor - centre);
            t.weights.push_back(w);
            sum += w;
        }
        for (double& w : t.weights) w /= sum;
    }
    return taps;
}

std::uint8_t to_byte(double v) {
    const double scaled = std::round(std::clamp(v, 0.0, 1.0) * 255.0);
    return static_cast<std::uint8_t>(scaled);
}

}  // namespace

double filter_radius(ResampleFilter filter) {
    switch (filter) {
        case ResampleFilter::Box: return 0.5;
        case ResampleFilter::Triangle: return 1.0;
        case ResampleFilter::Mitchell: return 2.0;
        case ResampleFilter::Lanczos3: return 3.0;
    }
    return 0.5;
}

double filter_weight(ResampleFilter filter, double x) {
    const double a = std::fabs(x);
    switch (filter) {
        case ResampleFilter::Box:
            return a <= 0.5 ? 1.0 : 0.0;
        case ResampleFilter::Triangle:
            return a < 1.0 ? 1.0 - a : 0.0;
        case ResampleFilter::Mitchell: {
            // Mitchell and Netravali (1988), B = C = 1/3.
            constexpr double B = 1.0 / 3.0;
            constexpr double C = 1.0 / 3.0;
            if (a < 1.0) {
                return ((12 - 9 * B - 6 * C) * a * a * a + (-18 + 12 * B + 6 * C) * a * a + (6 - 2 * B)) / 6;
            }
            if (a < 2.0) {
                return ((-B - 6 * C) * a * a * a + (6 * B + 30 * C) * a * a + (-12 * B - 48 * C) * a +
                        (8 * B + 24 * C)) /
                       6;
            }
            return 0.0;
        }
        case ResampleFilter::Lanczos3: {
            if (a >= 3.0) return 0.0;
            if (a == 0.0) return 1.0;
            const double px = kPi * a;
            return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
        }
    }
    return 0.0;
}

double linear_to_srgb(double linear) {
    if (linear <= 0.0031308) return 12.92 * linear;
    return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

double srgb_to_linear(double encoded) {
    if (encoded <= 0.04045) return encoded / 12.92;
    return std::pow((encoded + 0.055) / 1.055, 2.4);
}

Image downsample(const LinearImage& source, int factor, ResampleFilter filter) {
    if (factor < 1) throw std::invalid_argument("downsample: factor must be at least 1");
    if (source.width % factor != 0 || source.height % factor != 0) {
        throw std::invalid_argument("downsample: source size is not a multiple of the factor");
    }
    if (source.rgba.size() != static_cast<std::size_t>(source.width) * static_cast<std::size_t>(source.height) * 4) {
        throw std::invalid_argument("downsample: pixel buffer has the wrong size");
    }
    const int out_w = source.width / factor;
    const int out_h = source.height / factor;
    const auto x_taps = make_taps(out_w, factor, filter);
    const auto y_taps = make_taps(out_h, factor, filter);

    // Horizontal pass: source.height rows of out_w pixels.
    std::vector<double> rows(static_cast<std::size_t>(out_w) * static_cast<std::size_t>(source.height) * 4, 0.0);
    for (int y = 0; y < source.height; ++y) {
        const float* in = source.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(source.width) * 4;
        double* out = rows.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out_w) * 4;
        for (int j = 0; j < out_w; ++j) {
            const Taps& t = x_taps[static_cast<std::size_t>(j)];
            double acc[4] = {0, 0, 0, 0};
            for (std::size_t i = 0; i < t.weights.size(); ++i) {
                const int k = t.first + static_cast<int>(i);
                if (k < 0 || k >= source.width) continue;
                const float* p = in + static_cast<std::size_t>(k) * 4;
                for (int c = 0; c < 4; ++c) acc[c] += t.weights[i] * p[c];
            }
            for (int c = 0; c < 4; ++c) out[static_cast<std::size_t>(j) * 4 + static_cast<std::size_t>(c)] = acc[c];
        }
    }
    // Vertical pass and conversion.
    Image result(out_w, out_h);
    for (int j = 0; j < out_h; ++j) {
        const Taps& t = y_taps[static_cast<std::size_t>(j)];
        for (int x = 0; x < out_w; ++x) {
            double acc[4] = {0, 0, 0, 0};
            for (std::size_t i = 0; i < t.weights.size(); ++i) {
                const int k = t.first + static_cast<int>(i);
                if (k < 0 || k >= source.height) continue;
                const double* p = rows.data() + (static_cast<std::size_t>(k) * static_cast<std::size_t>(out_w) +
                                                 static_cast<std::size_t>(x)) * 4;
                for (int c = 0; c < 4; ++c) acc[c] += t.weights[i] * p[c];
            }
            std::uint8_t* px = result.pixel(x, j);
            const double alpha = std::clamp(acc[3], 0.0, 1.0);
            const std::uint8_t a8 = to_byte(alpha);
            if (a8 == 0) continue;  // stays (0, 0, 0, 0)
            for (int c = 0; c < 3; ++c) {
                const double straight = std::clamp(acc[c], 0.0, alpha) / alpha;
                px[c] = to_byte(linear_to_srgb(straight));
            }
            px[3] = a8;
        }
    }
    return result;
}

void apply_outline(Image& image, const std::array<std::uint8_t, 3>& colour) {
    const Image before = image;
    auto solid = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < before.width && y < before.height && before.pixel(x, y)[3] >= 128;
    };
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            std::uint8_t* p = image.pixel(x, y);
            if (solid(x, y)) {
                p[3] = 255;
            } else if (solid(x - 1, y) || solid(x + 1, y) || solid(x, y - 1) || solid(x, y + 1)) {
                p[0] = colour[0];
                p[1] = colour[1];
                p[2] = colour[2];
                p[3] = 255;
            } else {
                p[0] = p[1] = p[2] = p[3] = 0;
            }
        }
    }
}

}  // namespace pose_atlas
