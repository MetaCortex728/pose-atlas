// SPDX-License-Identifier: Apache-2.0
#include "pose_atlas/sampling.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace pose_atlas {

namespace {

using nlohmann::ordered_json;

constexpr double kClosedPositionM = 1e-5;
constexpr double kClosedRotationDegrees = 1e-3;

void require_positive(double value, const char* what) {
    if (!std::isfinite(value) || !(value > 0.0)) {
        throw SamplingError(std::string(what) + " must be a positive finite number");
    }
}

ordered_json optional_json(const std::optional<double>& value) {
    return value ? ordered_json(*value) : ordered_json(nullptr);
}

}  // namespace

std::string to_string(EndPoint endpoint) { return endpoint == EndPoint::Include ? "include" : "exclude"; }

std::string to_string(Cadence::Kind kind) {
    switch (kind) {
        case Cadence::Kind::SourceRate: return "source_rate";
        case Cadence::Kind::FixedFrame: return "fixed_frame";
        case Cadence::Kind::Distance: return "distance";
    }
    return "?";
}

std::vector<double> sample_times(double start_s, double end_s, int pose_count, EndPoint endpoint) {
    if (!std::isfinite(start_s) || !std::isfinite(end_s)) throw SamplingError("source range must be finite");
    if (end_s < start_s) throw SamplingError("source range end is before its start");
    const int minimum = endpoint == EndPoint::Include ? 2 : 1;
    if (pose_count < minimum) {
        throw SamplingError("pose count must be at least " + std::to_string(minimum) + " with the end point " +
                            (endpoint == EndPoint::Include ? "included" : "excluded") + ", got " +
                            std::to_string(pose_count));
    }
    const double duration = end_s - start_s;
    if (duration == 0.0 && pose_count > 1) {
        throw SamplingError("source range is empty; only a single pose can be sampled from it");
    }
    const int intervals = endpoint == EndPoint::Include ? pose_count - 1 : pose_count;
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(pose_count));
    for (int i = 0; i < pose_count; ++i) times.push_back(start_s + duration * i / intervals);
    // The included end point is exactly the end, not a rounded neighbour.
    if (endpoint == EndPoint::Include) times.back() = end_s;
    return times;
}

LoopClosure loop_closure(const Model& model, const Animation& animation, double start_s, double end_s) {
    const Pose a = evaluate(model, &animation, start_s, TimeMode::Clamp);
    const Pose b = evaluate(model, &animation, end_s, TimeMode::Clamp);
    LoopClosure closure;
    for (std::size_t i = 0; i < a.global.size(); ++i) {
        closure.max_position_delta_m =
            std::max(closure.max_position_delta_m, norm(a.global[i].translation() - b.global[i].translation()));
        closure.max_rotation_delta_degrees =
            std::max(closure.max_rotation_delta_degrees, degrees(rotation_angle_between(a.global[i], b.global[i])));
    }
    return closure;
}

SamplingPlan plan_sampling(const Model& model, const SamplingSpec& spec) {
    const Animation& animation = model.animation(spec.action);
    SamplingPlan plan;
    plan.action = animation.name;
    plan.animation_start_s = animation.start_s;
    plan.animation_end_s = animation.end_s;
    plan.source_start_s = spec.start_s.value_or(animation.start_s);
    plan.source_end_s = spec.end_s.value_or(animation.end_s);
    plan.pose_count = spec.pose_count;
    plan.endpoint = spec.endpoint;
    plan.cadence = spec.cadence;

    const std::vector<double> times =
        sample_times(plan.source_start_s, plan.source_end_s, spec.pose_count, spec.endpoint);
    plan.intervals = spec.endpoint == EndPoint::Include ? spec.pose_count - 1 : spec.pose_count;
    const double duration = plan.source_end_s - plan.source_start_s;
    plan.source_step_s = duration / plan.intervals;

    if (plan.source_start_s < animation.start_s || plan.source_end_s > animation.end_s) {
        plan.warnings.push_back("source range lies partly outside the animation's keys; the first or last key is held there");
    }

    // Runtime clock.
    const Cadence& c = spec.cadence;
    std::optional<double> pose_ms;
    std::optional<double> pose_m;
    switch (c.kind) {
        case Cadence::Kind::SourceRate:
            require_positive(c.playback_rate, "playback rate");
            pose_ms = plan.source_step_s * 1000.0 / c.playback_rate;
            plan.playback_rate = c.playback_rate;
            break;
        case Cadence::Kind::FixedFrame:
            require_positive(c.frame_ms, "frame duration");
            pose_ms = c.frame_ms;
            if (plan.source_step_s > 0.0) plan.playback_rate = plan.source_step_s * 1000.0 / c.frame_ms;
            break;
        case Cadence::Kind::Distance:
            require_positive(c.cycle_length_m, "cycle length");
            if (!std::isfinite(c.reference_speed_mps) || c.reference_speed_mps < 0.0) {
                throw SamplingError("reference speed must be zero (none) or a positive finite number");
            }
            pose_m = c.cycle_length_m / plan.intervals;
            plan.runtime_cycle_m = c.cycle_length_m;
            if (c.reference_speed_mps > 0.0) {
                pose_ms = *pose_m / c.reference_speed_mps * 1000.0;
                plan.playback_rate = duration / (c.cycle_length_m / c.reference_speed_mps);
            }
            break;
    }
    if (pose_ms) plan.runtime_cycle_ms = *pose_ms * plan.intervals;

    for (int i = 0; i < spec.pose_count; ++i) {
        Sample sample;
        sample.pose_index = i;
        sample.source_time_s = times[static_cast<std::size_t>(i)];
        sample.phase = static_cast<double>(i) / plan.intervals;
        if (pose_ms) {
            sample.runtime_start_ms = *pose_ms * i;
            sample.runtime_duration_ms = *pose_ms;
        }
        if (pose_m) {
            sample.runtime_start_m = *pose_m * i;
            sample.runtime_distance_m = *pose_m;
        }
        plan.samples.push_back(sample);
    }

    plan.loop_closure = loop_closure(model, animation, plan.source_start_s, plan.source_end_s);
    const bool closed = plan.loop_closure.max_position_delta_m <= kClosedPositionM &&
                        plan.loop_closure.max_rotation_delta_degrees <= kClosedRotationDegrees;
    if (spec.endpoint == EndPoint::Include && closed && duration > 0.0) {
        plan.warnings.push_back(
            "the end pose equals the start pose, so the last sample repeats the first; exclude the end point for loops");
    }
    if (spec.endpoint == EndPoint::Exclude && !closed) {
        plan.warnings.push_back("the end pose differs from the start pose; played as a loop, the action jumps");
    }
    return plan;
}

std::string plan_to_json(const SamplingPlan& plan) {
    ordered_json out;
    out["action"] = plan.action;
    out["source_clock"] = {{"unit", "s"},
                           {"animation_start", plan.animation_start_s},
                           {"animation_end", plan.animation_end_s},
                           {"start", plan.source_start_s},
                           {"end", plan.source_end_s},
                           {"step", plan.source_step_s}};
    out["pose_clock"] = {{"pose_count", plan.pose_count},
                         {"intervals", plan.intervals},
                         {"end_point", to_string(plan.endpoint)}};
    ordered_json runtime = {{"cadence", to_string(plan.cadence.kind)}};
    switch (plan.cadence.kind) {
        case Cadence::Kind::SourceRate: runtime["playback_rate"] = plan.cadence.playback_rate; break;
        case Cadence::Kind::FixedFrame: runtime["frame_ms"] = plan.cadence.frame_ms; break;
        case Cadence::Kind::Distance:
            runtime["cycle_length_m"] = plan.cadence.cycle_length_m;
            runtime["reference_speed_mps"] =
                plan.cadence.reference_speed_mps > 0.0 ? ordered_json(plan.cadence.reference_speed_mps) : nullptr;
            break;
    }
    runtime["cycle_ms"] = optional_json(plan.runtime_cycle_ms);
    runtime["cycle_m"] = optional_json(plan.runtime_cycle_m);
    runtime["implied_playback_rate"] = optional_json(plan.playback_rate);
    out["runtime_clock"] = runtime;
    out["loop_closure"] = {{"max_position_delta_m", plan.loop_closure.max_position_delta_m},
                           {"max_rotation_delta_degrees", plan.loop_closure.max_rotation_delta_degrees}};
    out["warnings"] = plan.warnings;
    out["samples"] = ordered_json::array();
    for (const Sample& s : plan.samples) {
        out["samples"].push_back({{"pose_index", s.pose_index},
                                  {"source_time_s", s.source_time_s},
                                  {"phase", s.phase},
                                  {"runtime_start_ms", optional_json(s.runtime_start_ms)},
                                  {"runtime_duration_ms", optional_json(s.runtime_duration_ms)},
                                  {"runtime_start_m", optional_json(s.runtime_start_m)},
                                  {"runtime_distance_m", optional_json(s.runtime_distance_m)}});
    }
    return out.dump(2) + "\n";
}

}  // namespace pose_atlas
