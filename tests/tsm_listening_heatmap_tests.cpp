#include "pch.h"
#include "tsm_listening_heatmap.h"

#include <cmath>
#include <limits>

namespace TSM::Tests
{
namespace
{

namespace Heatmap = ListeningHeatmap;
constexpr std::int64_t DaySeconds = 24 * 60 * 60;

} // namespace

TEST(ListeningHeatmapTests, ShortTrackUsesOneBucketAndPartialCoverageAccumulates)
{
    Heatmap::State state = Heatmap::CreateState(6.0);
    ASSERT_EQ(state.bucketFatigue.size(), 1u);

    EXPECT_TRUE(Heatmap::RecordCoverage(state, 0.0, 3.0, 1.0, 1000));
    EXPECT_NEAR(state.bucketFatigue[0], 0.5, 0.000001);
    EXPECT_TRUE(Heatmap::RecordCoverage(state, 3.0, 6.0, 1.0, 1000));
    EXPECT_DOUBLE_EQ(state.bucketFatigue[0], 1.0);
    EXPECT_DOUBLE_EQ(
        Heatmap::SegmentAverageFatigue(state, 0.0, 6.0, 1000), 1.0);
    EXPECT_DOUBLE_EQ(
        Heatmap::SegmentExplorationScore(state, 0.0, 6.0, 1000), 0.0);
}

TEST(ListeningHeatmapTests, OverlapIsProportionalAndSegmentAverageIsWeighted)
{
    Heatmap::State state = Heatmap::CreateState(30.0);
    ASSERT_EQ(state.bucketFatigue.size(), 3u);

    ASSERT_TRUE(Heatmap::RecordCoverage(state, 5.0, 25.0, 1.0, 2000));
    EXPECT_DOUBLE_EQ(state.bucketFatigue[0], 0.5);
    EXPECT_DOUBLE_EQ(state.bucketFatigue[1], 1.0);
    EXPECT_DOUBLE_EQ(state.bucketFatigue[2], 0.5);
    EXPECT_NEAR(
        Heatmap::SegmentAverageFatigue(state, 0.0, 30.0, 2000),
        2.0 / 3.0,
        0.000001);
    EXPECT_NEAR(
        Heatmap::SegmentAverageFatigue(state, 5.0, 15.0, 2000),
        0.75,
        0.000001);

    const Heatmap::Summary summary = Heatmap::Summarize(state, 2000, 0.6);
    EXPECT_NEAR(summary.coverage, 1.0 / 3.0, 0.000001);
    EXPECT_NEAR(summary.meanFatigue, 2.0 / 3.0, 0.000001);
    EXPECT_DOUBLE_EQ(summary.maxFatigue, 1.0);
}

TEST(ListeningHeatmapTests, DailyDecayIsContinuousAndExactlyConfigurable)
{
    Heatmap::State state = Heatmap::CreateState(10.0);
    ASSERT_TRUE(Heatmap::RecordCoverage(state, 0.0, 10.0, 1.0, 1000));

    EXPECT_NEAR(
        Heatmap::SegmentAverageFatigue(
            state, 0.0, 10.0, 1000 + DaySeconds / 2),
        std::sqrt(0.95),
        0.000001);
    EXPECT_NEAR(
        Heatmap::SegmentAverageFatigue(
            state, 0.0, 10.0, 1000 + DaySeconds),
        0.95,
        0.000001);
    EXPECT_NEAR(
        Heatmap::SegmentExplorationScore(
            state, 0.0, 10.0, 1000 + DaySeconds),
        0.05,
        0.000001);
    EXPECT_LT(
        Heatmap::SegmentAverageFatigue(
            state, 0.0, 10.0, 1000 + 180 * DaySeconds),
        0.0001);

    Heatmap::DecayTo(state, 1000 + DaySeconds);
    EXPECT_EQ(state.referenceEpochSeconds, 1000 + DaySeconds);
    ASSERT_EQ(state.bucketFatigue.size(), 1u);
    EXPECT_NEAR(state.bucketFatigue[0], 0.95, 0.000001);
}

TEST(ListeningHeatmapTests, BucketSizeAndDailyDecayFactorAreConfigurable)
{
    Heatmap::Config config;
    config.bucketDurationSeconds = 4.0;
    config.dailyDecayFactor = 0.8;
    Heatmap::State state = Heatmap::CreateState(25.0, config);
    ASSERT_EQ(state.bucketFatigue.size(), 7u);

    ASSERT_TRUE(Heatmap::RecordCoverage(state, 0.0, 4.0, 1.0, 1500));
    EXPECT_NEAR(
        Heatmap::SegmentAverageFatigue(
            state, 0.0, 4.0, 1500 + DaySeconds),
        0.8,
        0.000001);
    EXPECT_DOUBLE_EQ(
        Heatmap::SegmentAverageFatigue(
            state, 4.0, 8.0, 1500 + DaySeconds),
        0.0);
}

TEST(ListeningHeatmapTests, OutOfOrderTimestampsProduceTheSameFatigue)
{
    Heatmap::State chronological = Heatmap::CreateState(10.0);
    Heatmap::State reversed = Heatmap::CreateState(10.0);

    ASSERT_TRUE(Heatmap::RecordCoverage(
        chronological, 0.0, 10.0, 0.2, 1000));
    ASSERT_TRUE(Heatmap::RecordCoverage(
        chronological, 0.0, 10.0, 0.2, 1000 + DaySeconds));
    ASSERT_TRUE(Heatmap::RecordCoverage(
        reversed, 0.0, 10.0, 0.2, 1000 + DaySeconds));
    ASSERT_TRUE(Heatmap::RecordCoverage(
        reversed, 0.0, 10.0, 0.2, 1000));

    ASSERT_EQ(chronological.bucketFatigue.size(), 1u);
    ASSERT_EQ(reversed.bucketFatigue.size(), 1u);
    EXPECT_NEAR(chronological.bucketFatigue[0], 0.39, 0.000001);
    EXPECT_NEAR(
        chronological.bucketFatigue[0],
        reversed.bucketFatigue[0],
        0.000001);
    EXPECT_EQ(
        chronological.referenceEpochSeconds,
        reversed.referenceEpochSeconds);
}

TEST(ListeningHeatmapTests, ThreeHourTrackKeepsFirstAndLastBucketsIndependent)
{
    constexpr double ThreeHours = 3.0 * 60.0 * 60.0;
    Heatmap::State state = Heatmap::CreateState(ThreeHours);
    ASSERT_EQ(state.bucketFatigue.size(), 1080u);

    ASSERT_TRUE(Heatmap::RecordCoverage(
        state, ThreeHours - 10.0, ThreeHours, 1.0, 3000));
    EXPECT_DOUBLE_EQ(
        Heatmap::SegmentAverageFatigue(state, 0.0, 10.0, 3000), 0.0);
    EXPECT_DOUBLE_EQ(
        Heatmap::SegmentAverageFatigue(
            state, ThreeHours - 10.0, ThreeHours, 3000),
        1.0);
    EXPECT_DOUBLE_EQ(
        Heatmap::SegmentExplorationScore(state, 0.0, 10.0, 3000),
        1.0);
    EXPECT_DOUBLE_EQ(
        Heatmap::SegmentExplorationScore(
            state, ThreeHours - 10.0, ThreeHours, 3000),
        0.0);
}

TEST(ListeningHeatmapTests, InvalidNumbersAreSanitizedWithoutEscapingBounds)
{
    const double nan = (std::numeric_limits<double>::quiet_NaN)();
    const double infinity = (std::numeric_limits<double>::infinity)();
    Heatmap::Config config;
    config.bucketDurationSeconds = nan;
    config.dailyDecayFactor = infinity;
    Heatmap::State state = Heatmap::CreateState(30.0, config);

    EXPECT_DOUBLE_EQ(
        state.bucketDurationSeconds,
        Heatmap::DefaultBucketDurationSeconds);
    EXPECT_DOUBLE_EQ(
        state.dailyDecayFactor,
        Heatmap::DefaultDailyDecayFactor);
    ASSERT_EQ(state.bucketFatigue.size(), 3u);
    EXPECT_FALSE(Heatmap::RecordCoverage(state, nan, 10.0, 1.0, 4000));
    EXPECT_FALSE(Heatmap::RecordCoverage(state, 20.0, 10.0, 1.0, 4000));
    EXPECT_FALSE(Heatmap::RecordCoverage(state, 0.0, 10.0, nan, 4000));
    EXPECT_FALSE(Heatmap::RecordCoverage(state, 0.0, 10.0, -1.0, 4000));

    EXPECT_TRUE(Heatmap::RecordCoverage(
        state, -infinity, infinity, 2.0, 4000));
    EXPECT_TRUE(std::all_of(
        state.bucketFatigue.begin(), state.bucketFatigue.end(),
        [](double fatigue) {
            return fatigue >= 0.0 && fatigue <= 1.0;
        }));
    EXPECT_DOUBLE_EQ(
        Heatmap::SegmentAverageFatigue(state, -infinity, infinity, 4000),
        1.0);

    const Heatmap::State invalidDuration = Heatmap::CreateState(nan);
    EXPECT_TRUE(invalidDuration.bucketFatigue.empty());
}

TEST(ListeningHeatmapTests, PlainStateRoundTripsAndUnsafeFatigueIsCleared)
{
    Heatmap::State source = Heatmap::CreateState(25.0);
    ASSERT_TRUE(Heatmap::RecordCoverage(source, 0.0, 10.0, 0.5, 5000));
    Heatmap::State exported = Heatmap::ExportState(source);

    Heatmap::State imported;
    ASSERT_TRUE(Heatmap::ImportState(exported, imported));
    EXPECT_EQ(imported.schemaVersion, Heatmap::StateSchemaVersion);
    EXPECT_DOUBLE_EQ(imported.trackDurationSeconds, 25.0);
    EXPECT_EQ(imported.referenceEpochSeconds, 5000);
    EXPECT_EQ(imported.bucketFatigue, exported.bucketFatigue);

    exported.bucketFatigue[0] =
        (std::numeric_limits<double>::quiet_NaN)();
    exported.bucketFatigue[1] = -4.0;
    exported.bucketFatigue[2] =
        (std::numeric_limits<double>::infinity)();
    ASSERT_TRUE(Heatmap::ImportState(exported, imported));
    EXPECT_EQ(imported.bucketFatigue, (std::vector<double>{0.0, 0.0, 0.0}));

    const Heatmap::State preserved = imported;
    exported.bucketFatigue.pop_back();
    EXPECT_FALSE(Heatmap::ImportState(exported, imported));
    EXPECT_EQ(imported.bucketFatigue, preserved.bucketFatigue);
}

} // namespace TSM::Tests
