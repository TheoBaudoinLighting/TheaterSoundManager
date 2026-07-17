#include "pch.h"

#include "tsm_transition_history.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace TSM::Tests
{
namespace
{

namespace TH = TSM::TransitionHistory;

constexpr std::int64_t DaySeconds = 86400;

nlohmann::json ValidDocument()
{
    return {
        {"schemaVersion", TH::History::CurrentSchemaVersion},
        {"config", {
            {"dailyDecayFactor", 0.95},
            {"fatigueIncrement", 0.25}
        }},
        {"transitions", nlohmann::json::array({{
            {"fromId", "A"},
            {"toId", "B"},
            {"totalCount", std::uint64_t{3}},
            {"fatigue", 0.5},
            {"referenceEpochSeconds", std::int64_t{1000}}
        }})}
    };
}

nlohmann::json TransitionEntry(
    std::string fromId,
    std::string toId,
    std::uint64_t totalCount = 1,
    double fatigue = 0.5,
    std::int64_t referenceEpochSeconds = 1000)
{
    return {
        {"fromId", std::move(fromId)},
        {"toId", std::move(toId)},
        {"totalCount", totalCount},
        {"fatigue", fatigue},
        {"referenceEpochSeconds", referenceEpochSeconds}
    };
}

TEST(TransitionHistoryTests, RepetitionsIncreaseFatigueProgressively)
{
    TH::History once;
    TH::History repeated;
    constexpr std::int64_t now = 1'000'000;

    ASSERT_TRUE(once.RecordTransition("A", "B", now));
    for (int repetition = 0; repetition < 14; ++repetition)
        ASSERT_TRUE(repeated.RecordTransition("A", "B", now));

    const auto onceState = once.GetState("A", "B", now);
    const auto repeatedState = repeated.GetState("A", "B", now);
    ASSERT_TRUE(onceState.has_value());
    ASSERT_TRUE(repeatedState.has_value());
    EXPECT_EQ(onceState->totalCount, 1u);
    EXPECT_EQ(repeatedState->totalCount, 14u);
    EXPECT_DOUBLE_EQ(onceState->fatigue, 0.25);
    EXPECT_NEAR(
        repeatedState->fatigue,
        1.0 - std::pow(0.75, 14.0), 1e-12);
    EXPECT_GT(repeatedState->fatigue, onceState->fatigue);
    EXPECT_LT(
        repeated.GetDiversityScore("A", "B", now),
        once.GetDiversityScore("A", "B", now));
    EXPECT_LT(repeatedState->fatigue, 1.0);
}

TEST(TransitionHistoryTests, DirectionalPairsAreIndependent)
{
    TH::History history;
    ASSERT_TRUE(history.RecordTransition("A", "B", 100));
    ASSERT_TRUE(history.RecordTransition("A", "B", 100));
    ASSERT_TRUE(history.RecordTransition("B", "A", 100));

    ASSERT_TRUE(history.GetState("A", "B", 100).has_value());
    ASSERT_TRUE(history.GetState("B", "A", 100).has_value());
    EXPECT_EQ(history.GetState("A", "B", 100)->totalCount, 2u);
    EXPECT_EQ(history.GetState("B", "A", 100)->totalCount, 1u);
    EXPECT_GT(
        history.GetFatigue("A", "B", 100),
        history.GetFatigue("B", "A", 100));
}

TEST(TransitionHistoryTests, MergeAlignsDecayAndPreservesBothBoundedHistories)
{
    constexpr std::int64_t OneDay = 86'400;
    TH::History durable;
    TH::History pending;
    ASSERT_TRUE(durable.RecordTransition("a", "b", 0));
    ASSERT_TRUE(pending.RecordTransition("a", "b", OneDay));
    ASSERT_TRUE(pending.RecordTransition("c", "d", OneDay));

    durable.MergeFrom(pending);

    const auto merged = durable.GetState("a", "b", OneDay);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(merged->totalCount, 2u);
    const double decayedDurable =
        TH::Config::DefaultFatigueIncrement *
        TH::Config::DefaultDailyDecayFactor;
    const double expectedFatigue = 1.0 -
        (1.0 - decayedDurable) *
        (1.0 - TH::Config::DefaultFatigueIncrement);
    EXPECT_NEAR(merged->fatigue, expectedFatigue, 1e-12);
    EXPECT_EQ(merged->referenceEpochSeconds, OneDay);

    const auto pendingOnly = durable.GetState("c", "d", OneDay);
    ASSERT_TRUE(pendingOnly.has_value());
    EXPECT_EQ(pendingOnly->totalCount, 1u);
    EXPECT_DOUBLE_EQ(
        pendingOnly->fatigue,
        TH::Config::DefaultFatigueIncrement);
    EXPECT_LE(durable.Size(), TH::History::MaximumEntries);
}

TEST(TransitionHistoryTests, FatigueDecaysContinuouslyAcrossSixMonths)
{
    TH::History history;
    constexpr std::int64_t start = 5000;
    constexpr std::int64_t sixMonths = 180 * DaySeconds;
    ASSERT_TRUE(history.RecordTransition("A", "B", start));

    const double expected = 0.25 * std::pow(0.95, 180.0);
    EXPECT_NEAR(
        history.GetFatigue("A", "B", start + sixMonths),
        expected, 1e-12);
    EXPECT_NEAR(
        history.GetDiversityScore("A", "B", start + sixMonths),
        1.0 - expected, 1e-12);

    // Half a day must be the square root of the one-day multiplier.
    EXPECT_NEAR(
        history.GetFatigue("A", "B", start + DaySeconds / 2),
        0.25 * std::sqrt(0.95), 1e-12);
}

TEST(TransitionHistoryTests, RecordDecaysBeforeApplyingSaturatingIncrement)
{
    TH::History history;
    ASSERT_TRUE(history.RecordTransition("A", "B", 0));
    ASSERT_TRUE(history.RecordTransition("A", "B", DaySeconds));

    const double decayed = 0.25 * 0.95;
    const double expected = decayed + 0.25 * (1.0 - decayed);
    EXPECT_NEAR(history.GetFatigue("A", "B", DaySeconds), expected, 1e-12);
    EXPECT_EQ(history.GetState("A", "B", DaySeconds)->totalCount, 2u);
}

TEST(TransitionHistoryTests, ClockRollbackCannotIncreaseFatigueOrMoveReferenceBack)
{
    TH::History history;
    ASSERT_TRUE(history.RecordTransition("A", "B", 200));
    ASSERT_TRUE(history.RecordTransition("A", "B", 100));

    const auto state = history.GetState("A", "B", 100);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->referenceEpochSeconds, 200);
    EXPECT_DOUBLE_EQ(state->fatigue, 0.4375);
}

TEST(TransitionHistoryTests, ExportImportIsDeterministicAndRoundTrips)
{
    TH::History first;
    TH::History second;
    const auto populate = [](TH::History& history) {
        EXPECT_TRUE(history.RecordTransition("Z", "A", 100));
        EXPECT_TRUE(history.RecordTransition("A", "Z", 120));
        EXPECT_TRUE(history.RecordTransition("A", "B", 140));
        EXPECT_TRUE(history.RecordTransition("A", "B", 200));
    };
    populate(first);
    populate(second);

    const nlohmann::json exported = first.ExportState();
    EXPECT_EQ(exported, second.ExportState());
    EXPECT_EQ(exported.dump(), second.ExportState().dump());
    ASSERT_EQ(exported["transitions"].size(), 3u);
    EXPECT_EQ(exported["transitions"][0]["fromId"], "A");
    EXPECT_EQ(exported["transitions"][0]["toId"], "B");

    TH::History imported({0.8, 0.5});
    std::string error;
    ASSERT_TRUE(imported.ImportState(exported, error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(imported.ExportState(), exported);
}

TEST(TransitionHistoryTests, EmptyIdsAndNegativeTimestampsAreRejected)
{
    TH::History history;
    EXPECT_FALSE(history.RecordTransition("", "B", 0));
    EXPECT_FALSE(history.RecordTransition("A", "", 0));
    EXPECT_FALSE(history.RecordTransition("A", "B", -1));
    EXPECT_EQ(history.Size(), 0u);
    EXPECT_FALSE(history.GetState("", "B", 0).has_value());
    EXPECT_DOUBLE_EQ(history.GetFatigue("A", "B", -1), 0.0);
    EXPECT_DOUBLE_EQ(history.GetDiversityScore("A", "B", -1), 1.0);
}

TEST(TransitionHistoryTests, TrackIdsAreBoundedValidUtf8WithoutControls)
{
    TH::History history;
    const std::string maximumId(TH::History::MaximumTrackIdBytes, 'a');
    const std::string oversizedId(
        TH::History::MaximumTrackIdBytes + 1, 'a');
    const std::string invalidUtf8("\xC3\x28", 2);
    const std::string controlId("track\nname");

    ASSERT_TRUE(history.RecordTransition(maximumId, "B", 1));
    EXPECT_FALSE(history.RecordTransition(oversizedId, "B", 1));
    EXPECT_FALSE(history.RecordTransition(invalidUtf8, "B", 1));
    EXPECT_FALSE(history.RecordTransition(controlId, "B", 1));
    EXPECT_EQ(history.Size(), 1u);
    EXPECT_FALSE(history.GetState(oversizedId, "B", 1).has_value());
}

TEST(TransitionHistoryTests, ImportRejectsOversizedIdsTransactionally)
{
    TH::History history;
    ASSERT_TRUE(history.RecordTransition("committed", "pair", 10));
    const nlohmann::json committed = history.ExportState();
    nlohmann::json document = ValidDocument();
    document["transitions"][0]["fromId"] =
        std::string(TH::History::MaximumTrackIdBytes + 1, 'x');

    std::string error;
    EXPECT_FALSE(history.ImportState(document, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(history.ExportState(), committed);
}

TEST(TransitionHistoryTests, ImportRejectsTooManyEntriesTransactionally)
{
    TH::History history;
    ASSERT_TRUE(history.RecordTransition("committed", "pair", 10));
    const nlohmann::json committed = history.ExportState();
    nlohmann::json document = ValidDocument();
    document["transitions"] = nlohmann::json::array();
    for (std::size_t index = 0;
         index <= TH::History::MaximumEntries; ++index)
    {
        document["transitions"].push_back(TransitionEntry(
            "source-" + std::to_string(index), "target"));
    }

    std::string error;
    EXPECT_FALSE(history.ImportState(document, error));
    EXPECT_NE(error.find("maximum entry count"), std::string::npos);
    EXPECT_EQ(history.ExportState(), committed);
}

TEST(TransitionHistoryTests, CapacityEvictionUsesDecayedFatigueAndStableKeyTieBreak)
{
    constexpr std::int64_t now = 180 * DaySeconds;
    nlohmann::json document = ValidDocument();
    document["transitions"] = nlohmann::json::array();
    document["transitions"].push_back(
        TransitionEntry("stale-high", "target", 20, 0.8, 0));
    document["transitions"].push_back(
        TransitionEntry("fresh-low", "target", 1, 0.1, now));
    document["transitions"].push_back(
        TransitionEntry("tie-a", "target", 2, 0.2, now));
    document["transitions"].push_back(
        TransitionEntry("tie-b", "target", 2, 0.2, now));
    while (document["transitions"].size() < TH::History::MaximumEntries)
    {
        const std::size_t index = document["transitions"].size();
        document["transitions"].push_back(TransitionEntry(
            "filler-" + std::to_string(index), "target", 10, 0.9, now));
    }

    TH::History history;
    std::string error;
    ASSERT_TRUE(history.ImportState(document, error)) << error;
    ASSERT_EQ(history.Size(), TH::History::MaximumEntries);

    // Raw fatigue is high, but six months of decay makes this the least
    // relevant transition at the insertion timestamp.
    ASSERT_TRUE(history.RecordTransition("new-1", "target", now));
    EXPECT_EQ(history.Size(), TH::History::MaximumEntries);
    EXPECT_FALSE(history.GetState("stale-high", "target", now).has_value());
    EXPECT_TRUE(history.GetState("fresh-low", "target", now).has_value());

    ASSERT_TRUE(history.RecordTransition("new-2", "target", now));
    EXPECT_EQ(history.Size(), TH::History::MaximumEntries);
    EXPECT_FALSE(history.GetState("fresh-low", "target", now).has_value());

    ASSERT_TRUE(history.RecordTransition("new-3", "target", now));
    EXPECT_EQ(history.Size(), TH::History::MaximumEntries);
    EXPECT_FALSE(history.GetState("tie-a", "target", now).has_value());
    EXPECT_TRUE(history.GetState("tie-b", "target", now).has_value());
}

TEST(TransitionHistoryTests, CapacityEvictionUsesAgeThenCount)
{
    nlohmann::json document = ValidDocument();
    document["config"]["dailyDecayFactor"] = 1.0;
    document["config"]["fatigueIncrement"] = 1.0;
    document["transitions"] = nlohmann::json::array({
        TransitionEntry("age-old", "target", 50, 0.1, 100),
        TransitionEntry("age-new", "target", 1, 0.1, 200),
        TransitionEntry("count-low", "target", 1, 0.2, 300),
        TransitionEntry("count-high", "target", 10, 0.2, 300)});
    while (document["transitions"].size() < TH::History::MaximumEntries)
    {
        const std::size_t index = document["transitions"].size();
        document["transitions"].push_back(TransitionEntry(
            "filler-" + std::to_string(index), "target", 10, 0.9, 1000));
    }

    TH::History history;
    std::string error;
    ASSERT_TRUE(history.ImportState(document, error)) << error;

    ASSERT_TRUE(history.RecordTransition("new-1", "target", 1000));
    EXPECT_FALSE(history.GetState("age-old", "target", 1000).has_value());
    ASSERT_TRUE(history.RecordTransition("new-2", "target", 1000));
    EXPECT_FALSE(history.GetState("age-new", "target", 1000).has_value());
    ASSERT_TRUE(history.RecordTransition("new-3", "target", 1000));
    EXPECT_FALSE(history.GetState("count-low", "target", 1000).has_value());
    EXPECT_TRUE(history.GetState("count-high", "target", 1000).has_value());
    EXPECT_EQ(history.Size(), TH::History::MaximumEntries);
}

TEST(TransitionHistoryTests, PublicLimitsKeepWorstCaseCompactJsonBelowFiveMiB)
{
    nlohmann::json document = ValidDocument();
    document["transitions"] = nlohmann::json::array();
    const std::string escapedTarget(
        TH::History::MaximumTrackIdBytes, '"');
    for (std::size_t index = 0;
         index < TH::History::MaximumEntries; ++index)
    {
        const std::string suffix = std::to_string(index);
        std::string source(
            TH::History::MaximumTrackIdBytes - suffix.size(), '\\');
        source += suffix;
        document["transitions"].push_back(TransitionEntry(
            std::move(source), escapedTarget));
    }

    TH::History history;
    std::string error;
    ASSERT_TRUE(history.ImportState(document, error)) << error;
    const std::string payload = history.ExportState().dump();
    EXPECT_LE(payload.size(), TH::History::MaximumCompactJsonBytes);
    EXPECT_LT(payload.size(), 16U * 1024U * 1024U);
}

TEST(TransitionHistoryTests, InvalidImportsAreTransactional)
{
    TH::History history;
    ASSERT_TRUE(history.RecordTransition("committed", "pair", 10));
    const nlohmann::json committed = history.ExportState();
    std::string error;

    const auto expectRejected = [&](nlohmann::json invalid) {
        error.clear();
        EXPECT_FALSE(history.ImportState(invalid, error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(history.ExportState(), committed);
    };

    nlohmann::json invalid = ValidDocument();
    invalid["schemaVersion"] = 99;
    expectRejected(invalid);

    invalid = ValidDocument();
    invalid["schemaVersion"] = std::uint64_t{4'294'967'297ULL};
    expectRejected(invalid);

    invalid = ValidDocument();
    invalid["config"]["dailyDecayFactor"] = 1.1;
    expectRejected(invalid);

    invalid = ValidDocument();
    invalid["transitions"][0]["fromId"] = "";
    expectRejected(invalid);

    invalid = ValidDocument();
    invalid["transitions"][0]["fromId"] = std::string("\xC3\x28", 2);
    expectRejected(invalid);

    invalid = ValidDocument();
    invalid["transitions"][0]["fromId"] = "track\nname";
    expectRejected(invalid);

    invalid = ValidDocument();
    invalid["transitions"][0]["fatigue"] =
        (std::numeric_limits<double>::quiet_NaN)();
    expectRejected(invalid);

    invalid = ValidDocument();
    invalid["transitions"][0]["referenceEpochSeconds"] = -1;
    expectRejected(invalid);

    invalid = ValidDocument();
    invalid["transitions"].push_back(invalid["transitions"][0]);
    expectRejected(invalid);

    invalid = ValidDocument();
    invalid["unexpected"] = true;
    expectRejected(invalid);

    invalid = ValidDocument();
    invalid["transitions"][0]["unexpected"] = true;
    expectRejected(invalid);
}

TEST(TransitionHistoryTests, ConstructorSanitizesInvalidConfiguration)
{
    TH::Config invalid;
    invalid.dailyDecayFactor =
        (std::numeric_limits<double>::quiet_NaN)();
    invalid.fatigueIncrement = 2.0;
    TH::History history(invalid);

    EXPECT_DOUBLE_EQ(
        history.GetConfig().dailyDecayFactor,
        TH::Config::DefaultDailyDecayFactor);
    EXPECT_DOUBLE_EQ(
        history.GetConfig().fatigueIncrement,
        TH::Config::DefaultFatigueIncrement);
}

TEST(TransitionHistoryTests, CounterSaturatesAtUint64Maximum)
{
    nlohmann::json document = ValidDocument();
    document["transitions"][0]["totalCount"] =
        (std::numeric_limits<std::uint64_t>::max)();
    document["transitions"][0]["fatigue"] = 0.5;

    TH::History history;
    std::string error;
    ASSERT_TRUE(history.ImportState(document, error)) << error;
    ASSERT_TRUE(history.RecordTransition("A", "B", 1000));

    const auto state = history.GetState("A", "B", 1000);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(
        state->totalCount,
        (std::numeric_limits<std::uint64_t>::max)());
    EXPECT_DOUBLE_EQ(state->fatigue, 0.625);
}

} // namespace
} // namespace TSM::Tests
