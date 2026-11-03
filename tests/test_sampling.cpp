// SPDX-License-Identifier: Apache-2.0
#include <doctest.h>

#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "pose_atlas/sampling.hpp"
#include "pose_atlas/test_rig.hpp"

using namespace pose_atlas;

namespace {

const Model& rig_model() {
    static const Model model = load_gltf(test_rig::make_glb(), "test_rig.glb");
    return model;
}

const double kRunEnd = static_cast<double>(0.8f);  // key times are stored as float32

}  // namespace

TEST_CASE("sampling: the end point is excluded or included as configured") {
    const auto excluded = sample_times(0.0, 0.8, 8, EndPoint::Exclude);
    REQUIRE(excluded.size() == 8);
    CHECK(excluded.front() == 0.0);
    CHECK(excluded[4] == doctest::Approx(0.4));
    CHECK(excluded.back() == doctest::Approx(0.7));
    CHECK(excluded.back() < 0.8);

    const auto included = sample_times(0.0, 0.8, 9, EndPoint::Include);
    REQUIRE(included.size() == 9);
    CHECK(included.front() == 0.0);
    CHECK(included[4] == doctest::Approx(0.4));
    CHECK(included.back() == 0.8);  // exactly the end
    for (std::size_t i = 0; i < 8; ++i) CHECK(included[i] == doctest::Approx(excluded[i]));

    const auto offset = sample_times(1.0, 3.0, 4, EndPoint::Exclude);
    CHECK(offset == std::vector<double>{1.0, 1.5, 2.0, 2.5});
    CHECK(sample_times(0.5, 0.5, 1, EndPoint::Exclude) == std::vector<double>{0.5});

    CHECK_THROWS_WITH_AS(sample_times(0.0, 1.0, 1, EndPoint::Include),
                         "pose count must be at least 2 with the end point included, got 1", SamplingError);
    CHECK_THROWS_AS(sample_times(0.0, 1.0, 0, EndPoint::Exclude), SamplingError);
    CHECK_THROWS_AS(sample_times(1.0, 0.0, 4, EndPoint::Exclude), SamplingError);
    CHECK_THROWS_AS(sample_times(0.5, 0.5, 3, EndPoint::Exclude), SamplingError);
}

TEST_CASE("sampling: a looping action sampled without its end point closes") {
    SamplingSpec spec;
    spec.action = "run";
    spec.pose_count = 8;
    const SamplingPlan plan = plan_sampling(rig_model(), spec);
    CHECK(plan.source_end_s == kRunEnd);
    CHECK(plan.intervals == 8);
    CHECK(plan.source_step_s == doctest::Approx(0.1));
    CHECK(plan.loop_closure.max_position_delta_m < 1e-6);
    CHECK(plan.loop_closure.max_rotation_delta_degrees < 1e-3);
    CHECK(plan.warnings.empty());
    REQUIRE(plan.samples.size() == 8);
    CHECK(plan.samples.back().source_time_s == doctest::Approx(0.7));
    CHECK(plan.samples.back().phase == doctest::Approx(0.875));

    // The same action with its end point included repeats pose 0 and says so.
    spec.pose_count = 9;
    spec.endpoint = EndPoint::Include;
    const SamplingPlan included = plan_sampling(rig_model(), spec);
    CHECK(included.samples.back().source_time_s == kRunEnd);
    CHECK(included.samples.back().phase == 1.0);
    REQUIRE(included.warnings.size() == 1);
    CHECK(included.warnings[0].find("repeats the first") != std::string::npos);

    // Half a cycle does not close; played as a loop it would jump.
    spec.pose_count = 4;
    spec.endpoint = EndPoint::Exclude;
    spec.end_s = 0.4;
    const SamplingPlan half = plan_sampling(rig_model(), spec);
    CHECK(half.loop_closure.max_rotation_delta_degrees == doctest::Approx(70.0).epsilon(1e-4));
    REQUIRE(half.warnings.size() == 1);
    CHECK(half.warnings[0].find("jumps") != std::string::npos);
}

TEST_CASE("sampling: the three clocks are reported side by side and kept apart") {
    SamplingSpec spec;
    spec.action = "run";
    spec.pose_count = 8;

    // Source rate: runtime follows the source clock, scaled.
    spec.cadence.kind = Cadence::Kind::SourceRate;
    spec.cadence.playback_rate = 2.0;
    const SamplingPlan source = plan_sampling(rig_model(), spec);
    CHECK(*source.samples[3].runtime_start_ms == doctest::Approx(150.0));
    CHECK(*source.samples[3].runtime_duration_ms == doctest::Approx(50.0));
    CHECK(*source.runtime_cycle_ms == doctest::Approx(400.0));
    CHECK(!source.samples[3].runtime_start_m);

    // Fixed frame: runtime is independent of the source step.
    spec.cadence.kind = Cadence::Kind::FixedFrame;
    spec.cadence.frame_ms = 80.0;
    const SamplingPlan fixed = plan_sampling(rig_model(), spec);
    CHECK(*fixed.samples[3].runtime_start_ms == 240.0);
    CHECK(*fixed.runtime_cycle_ms == 640.0);
    CHECK(*fixed.playback_rate == doctest::Approx(1.25));

    // Distance: one cycle covers 1.6 m; at 4 m/s that is 0.4 s per cycle,
    // twice as fast as the 0.8 s source clip.
    spec.cadence.kind = Cadence::Kind::Distance;
    spec.cadence.cycle_length_m = 1.6;
    spec.cadence.reference_speed_mps = 4.0;
    const SamplingPlan distance = plan_sampling(rig_model(), spec);
    CHECK(*distance.samples[3].runtime_start_m == doctest::Approx(0.6));
    CHECK(*distance.samples[3].runtime_distance_m == doctest::Approx(0.2));
    CHECK(*distance.samples[3].runtime_duration_ms == doctest::Approx(50.0));
    CHECK(*distance.runtime_cycle_m == 1.6);
    CHECK(*distance.runtime_cycle_ms == doctest::Approx(400.0));
    CHECK(*distance.playback_rate == doctest::Approx(2.0));

    // Without a reference speed there are no durations, only distances.
    spec.cadence.reference_speed_mps = 0.0;
    const SamplingPlan no_speed = plan_sampling(rig_model(), spec);
    CHECK(!no_speed.samples[3].runtime_duration_ms);
    CHECK(!no_speed.runtime_cycle_ms);

    // Whatever the cadence, the sampled poses are the same.
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(source.samples[i].source_time_s == fixed.samples[i].source_time_s);
        CHECK(source.samples[i].source_time_s == distance.samples[i].source_time_s);
        CHECK(source.samples[i].pose_index == static_cast<int>(i));
    }

    // Including the end point spreads the cycle over one interval fewer.
    spec.pose_count = 9;
    spec.endpoint = EndPoint::Include;
    const SamplingPlan included = plan_sampling(rig_model(), spec);
    CHECK(*included.samples[8].runtime_start_m == doctest::Approx(1.6));

    const auto json = nlohmann::json::parse(plan_to_json(distance));
    CHECK(json["action"] == "run");
    CHECK(json["source_clock"]["unit"] == "s");
    CHECK(json["pose_clock"]["end_point"] == "exclude");
    CHECK(json["runtime_clock"]["cadence"] == "distance");
    CHECK(json["samples"].size() == 8);
    CHECK(json["samples"][3]["pose_index"] == 3);
    CHECK(json["samples"][3]["source_time_s"].get<double>() == doctest::Approx(0.3));
    CHECK(json["samples"][3]["runtime_start_m"].get<double>() == doctest::Approx(0.6));
}

TEST_CASE("sampling: invalid requests") {
    SamplingSpec spec;
    spec.action = "walk";
    CHECK_THROWS_AS(plan_sampling(rig_model(), spec), ModelError);
    spec.action = "idle";
    spec.cadence.kind = Cadence::Kind::Distance;
    CHECK_THROWS_WITH_AS(plan_sampling(rig_model(), spec), "cycle length must be a positive finite number",
                         SamplingError);
    spec.cadence.kind = Cadence::Kind::FixedFrame;
    spec.cadence.frame_ms = 0.0;
    CHECK_THROWS_AS(plan_sampling(rig_model(), spec), SamplingError);
    spec.cadence.kind = Cadence::Kind::SourceRate;
    spec.cadence.playback_rate = std::nan("");
    CHECK_THROWS_AS(plan_sampling(rig_model(), spec), SamplingError);
}
