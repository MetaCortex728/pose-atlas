// SPDX-License-Identifier: Apache-2.0
// Sampling plans. Three clocks are kept apart and reported side by side:
//
//   source time    seconds on the animation's own clock in the glTF file;
//   pose index     which of the N sampled poses (0..N-1), i.e. the sprite
//                  frame within the action;
//   runtime        how the game advances through the poses: by elapsed time
//                  (milliseconds) or by distance travelled (metres).
//
// Changing the runtime cadence never changes which poses are sampled, and
// changing the pose count never changes the source clock.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pose_atlas/model.hpp"

namespace pose_atlas {

/// Whether the last sample sits on the end of the source range.
enum class EndPoint {
    Exclude,  // loops: N poses at start + i * D / N; the end equals pose 0
    Include,  // one-shots: N poses at start + i * D / (N - 1), N >= 2
};

struct Cadence {
    enum class Kind {
        SourceRate,  // runtime time = source time / playback_rate
        FixedFrame,  // every pose shows for frame_ms, whatever its source step
        Distance,    // one pass over the poses covers cycle_length_m
    };
    Kind kind = Kind::SourceRate;
    double playback_rate = 1.0;        // SourceRate
    double frame_ms = 100.0;           // FixedFrame
    double cycle_length_m = 0.0;       // Distance
    double reference_speed_mps = 0.0;  // Distance, optional: > 0 also gives durations
};

struct SamplingSpec {
    std::string action;
    int pose_count = 8;
    EndPoint endpoint = EndPoint::Exclude;
    std::optional<double> start_s;  // defaults to the animation's start
    std::optional<double> end_s;    // defaults to the animation's end
    Cadence cadence;
};

struct Sample {
    int pose_index = 0;                      // pose clock
    double source_time_s = 0.0;              // source clock
    double phase = 0.0;                      // pose_index / intervals, in [0, 1]
    std::optional<double> runtime_start_ms;  // runtime clock (time)
    std::optional<double> runtime_duration_ms;
    std::optional<double> runtime_start_m;  // runtime clock (distance)
    std::optional<double> runtime_distance_m;
};

/// How far the pose at the end of the source range is from the pose at its
/// start, over all nodes in model space.
struct LoopClosure {
    double max_position_delta_m = 0.0;
    double max_rotation_delta_degrees = 0.0;
};

struct SamplingPlan {
    std::string action;
    double animation_start_s = 0.0;
    double animation_end_s = 0.0;
    double source_start_s = 0.0;
    double source_end_s = 0.0;
    double source_step_s = 0.0;
    int pose_count = 0;
    int intervals = 0;  // N for Exclude, N - 1 for Include
    EndPoint endpoint = EndPoint::Exclude;
    Cadence cadence;
    std::optional<double> runtime_cycle_ms;
    std::optional<double> runtime_cycle_m;
    /// Source seconds per runtime second (Distance with a reference speed, or
    /// SourceRate's playback_rate).
    std::optional<double> playback_rate;
    LoopClosure loop_closure;
    std::vector<std::string> warnings;
    std::vector<Sample> samples;
};

class SamplingError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Source times of N poses over [start, end].
std::vector<double> sample_times(double start_s, double end_s, int pose_count, EndPoint endpoint);

LoopClosure loop_closure(const Model& model, const Animation& animation, double start_s, double end_s);

SamplingPlan plan_sampling(const Model& model, const SamplingSpec& spec);

/// Plan as JSON with the three clocks side by side.
std::string plan_to_json(const SamplingPlan& plan);

std::string to_string(EndPoint endpoint);
std::string to_string(Cadence::Kind kind);

}  // namespace pose_atlas
