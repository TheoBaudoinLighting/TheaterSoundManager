#pragma once

#include <cstdint>
#include <vector>

namespace TSM::ListeningHeatmap
{

inline constexpr std::uint32_t StateSchemaVersion = 1;
inline constexpr double DefaultBucketDurationSeconds = 10.0;
inline constexpr double DefaultDailyDecayFactor = 0.95;

struct Config
{
    double bucketDurationSeconds = DefaultBucketDurationSeconds;
    // Continuous decay is dailyDecayFactor^(elapsedSeconds / 86400).
    double dailyDecayFactor = DefaultDailyDecayFactor;
};

// Plain state intended for straightforward persistence by a caller. Each
// bucket fatigue is bounded to [0, 1].
struct State
{
    std::uint32_t schemaVersion = StateSchemaVersion;
    double trackDurationSeconds = 0.0;
    double bucketDurationSeconds = DefaultBucketDurationSeconds;
    double dailyDecayFactor = DefaultDailyDecayFactor;
    std::int64_t referenceEpochSeconds = 0;
    std::vector<double> bucketFatigue;
};

struct Summary
{
    // Duration-weighted part of the track at or above the requested fatigue
    // threshold.
    double coverage = 0.0;
    double meanFatigue = 0.0;
    double maxFatigue = 0.0;
};

State CreateState(
    double trackDurationSeconds,
    const Config& config = {});

// Adds weight in proportion to the part of each bucket covered by [start, end).
// Out-of-order observations are aged to the state's current reference time.
// Returns false when the interval or weight carries no usable information.
bool RecordCoverage(
    State& state,
    double startSeconds,
    double endSeconds,
    double weight,
    std::int64_t observedAtEpochSeconds);

// Advances the state's reference time and applies continuous daily-factor decay.
// Requests older than the current reference never reverse decay.
void DecayTo(State& state, std::int64_t epochSeconds);

double SegmentAverageFatigue(
    const State& state,
    double startSeconds,
    double endSeconds,
    std::int64_t atEpochSeconds);

// Returns 1 - SegmentAverageFatigue in [0, 1].
double SegmentExplorationScore(
    const State& state,
    double startSeconds,
    double endSeconds,
    std::int64_t atEpochSeconds);

Summary Summarize(
    const State& state,
    std::int64_t atEpochSeconds,
    double fatigueCoverageThreshold = 0.01);

// Export returns a normalized, self-consistent copy. Import accepts only the
// supported schema and exact bucket count; unsafe fatigue values are cleared.
State ExportState(const State& state);
bool ImportState(const State& exported, State& destination);

} // namespace TSM::ListeningHeatmap
