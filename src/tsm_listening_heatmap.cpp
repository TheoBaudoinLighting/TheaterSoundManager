#include "tsm_listening_heatmap.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace TSM::ListeningHeatmap
{
namespace
{

constexpr double SecondsPerDay = 24.0 * 60.0 * 60.0;
constexpr double MinimumBucketDurationSeconds = 0.1;
constexpr double MaximumBucketDurationSeconds = 60.0 * 60.0;
constexpr double MaximumTrackDurationSeconds = 24.0 * 60.0 * 60.0;
constexpr std::size_t MaximumBucketCount = 1000000;
constexpr double MaximumCoverageWeight = 1000000.0;

double SanitizeTrackDuration(double value)
{
    if (!std::isfinite(value) || value <= 0.0) return 0.0;
    return std::min(value, MaximumTrackDurationSeconds);
}

double SanitizeBucketDuration(double value)
{
    if (!std::isfinite(value) || value <= 0.0)
        return DefaultBucketDurationSeconds;
    return std::clamp(
        value,
        MinimumBucketDurationSeconds,
        MaximumBucketDurationSeconds);
}

double SanitizeDailyDecayFactor(double value)
{
    if (!std::isfinite(value) || value <= 0.0)
        return DefaultDailyDecayFactor;
    return std::min(value, 1.0);
}

std::int64_t SanitizeEpoch(std::int64_t value)
{
    return std::max<std::int64_t>(value, 0);
}

double SanitizeFatigue(double value)
{
    if (!std::isfinite(value)) return 0.0;
    return std::clamp(value, 0.0, 1.0);
}

std::size_t ExpectedBucketCount(
    double trackDurationSeconds,
    double bucketDurationSeconds)
{
    if (trackDurationSeconds <= 0.0 || bucketDurationSeconds <= 0.0)
        return 0;
    const long double ratio =
        static_cast<long double>(trackDurationSeconds) /
        static_cast<long double>(bucketDurationSeconds);
    const long double count = std::ceil(std::nextafter(
        ratio, static_cast<long double>(0.0)));
    if (count <= 0.0L) return 1;
    if (count >= static_cast<long double>(MaximumBucketCount))
        return MaximumBucketCount;
    return static_cast<std::size_t>(count);
}

State NormalizeState(State state)
{
    state.schemaVersion = StateSchemaVersion;
    state.trackDurationSeconds =
        SanitizeTrackDuration(state.trackDurationSeconds);
    state.bucketDurationSeconds =
        SanitizeBucketDuration(state.bucketDurationSeconds);
    state.dailyDecayFactor =
        SanitizeDailyDecayFactor(state.dailyDecayFactor);
    state.referenceEpochSeconds =
        SanitizeEpoch(state.referenceEpochSeconds);
    state.bucketFatigue.resize(ExpectedBucketCount(
        state.trackDurationSeconds,
        state.bucketDurationSeconds));
    for (double& fatigue : state.bucketFatigue)
        fatigue = SanitizeFatigue(fatigue);
    return state;
}

double DecayFactor(
    double dailyDecayFactor,
    std::int64_t fromEpochSeconds,
    std::int64_t toEpochSeconds)
{
    if (toEpochSeconds <= fromEpochSeconds) return 1.0;
    const double safeDailyDecayFactor =
        SanitizeDailyDecayFactor(dailyDecayFactor);
    if (safeDailyDecayFactor >= 1.0) return 1.0;
    const long double elapsedSeconds =
        static_cast<long double>(toEpochSeconds) -
        static_cast<long double>(fromEpochSeconds);
    const long double elapsedDays = elapsedSeconds / SecondsPerDay;
    if (elapsedDays > 1000000000.0L) return 0.0;
    return std::pow(
        safeDailyDecayFactor,
        static_cast<double>(elapsedDays));
}

bool ClampInterval(
    const State& state,
    double startSeconds,
    double endSeconds,
    double& start,
    double& end)
{
    if (state.trackDurationSeconds <= 0.0 ||
        std::isnan(startSeconds) || std::isnan(endSeconds))
    {
        return false;
    }
    start = std::clamp(
        startSeconds, 0.0, state.trackDurationSeconds);
    end = std::clamp(
        endSeconds, 0.0, state.trackDurationSeconds);
    return end > start;
}

template <typename Callback>
void ForEachOverlap(
    const State& state,
    double startSeconds,
    double endSeconds,
    Callback&& callback)
{
    if (state.bucketFatigue.empty()) return;
    std::size_t index = static_cast<std::size_t>(
        std::floor(startSeconds / state.bucketDurationSeconds));
    index = std::min(index, state.bucketFatigue.size() - 1);
    for (; index < state.bucketFatigue.size(); ++index)
    {
        const double bucketStart =
            static_cast<double>(index) * state.bucketDurationSeconds;
        if (bucketStart >= endSeconds) break;
        const double bucketEnd = std::min(
            bucketStart + state.bucketDurationSeconds,
            state.trackDurationSeconds);
        const double overlapStart = std::max(startSeconds, bucketStart);
        const double overlapEnd = std::min(endSeconds, bucketEnd);
        const double overlap = std::max(overlapEnd - overlapStart, 0.0);
        const double bucketLength = bucketEnd - bucketStart;
        if (overlap > 0.0 && bucketLength > 0.0)
            callback(index, overlap, bucketLength);
    }
}

double ViewDecayFactor(const State& state, std::int64_t atEpochSeconds)
{
    return DecayFactor(
        state.dailyDecayFactor,
        state.referenceEpochSeconds,
        SanitizeEpoch(atEpochSeconds));
}

} // namespace

State CreateState(
    double trackDurationSeconds,
    const Config& config)
{
    State state;
    state.trackDurationSeconds = trackDurationSeconds;
    state.bucketDurationSeconds = config.bucketDurationSeconds;
    state.dailyDecayFactor = config.dailyDecayFactor;
    return NormalizeState(std::move(state));
}

bool RecordCoverage(
    State& state,
    double startSeconds,
    double endSeconds,
    double weight,
    std::int64_t observedAtEpochSeconds)
{
    state = NormalizeState(std::move(state));
    double start = 0.0;
    double end = 0.0;
    if (!ClampInterval(state, startSeconds, endSeconds, start, end) ||
        !std::isfinite(weight) || weight <= 0.0)
    {
        return false;
    }

    const std::int64_t observedAt = SanitizeEpoch(observedAtEpochSeconds);
    if (observedAt > state.referenceEpochSeconds)
        DecayTo(state, observedAt);
    const double observationDecay = DecayFactor(
        state.dailyDecayFactor,
        observedAt,
        state.referenceEpochSeconds);
    const double effectiveWeight =
        std::min(weight, MaximumCoverageWeight) * observationDecay;
    if (effectiveWeight <= 0.0) return false;

    bool changed = false;
    ForEachOverlap(
        state, start, end,
        [&](std::size_t index, double overlap, double bucketLength) {
            const double contribution = std::clamp(
                effectiveWeight * overlap / bucketLength, 0.0, 1.0);
            if (contribution <= 0.0) return;
            state.bucketFatigue[index] = std::min(
                1.0, state.bucketFatigue[index] + contribution);
            changed = true;
        });
    return changed;
}

void DecayTo(State& state, std::int64_t epochSeconds)
{
    state = NormalizeState(std::move(state));
    const std::int64_t targetEpoch = SanitizeEpoch(epochSeconds);
    if (targetEpoch <= state.referenceEpochSeconds) return;

    const double factor = DecayFactor(
        state.dailyDecayFactor,
        state.referenceEpochSeconds,
        targetEpoch);
    for (double& fatigue : state.bucketFatigue)
        fatigue = SanitizeFatigue(fatigue * factor);
    state.referenceEpochSeconds = targetEpoch;
}

double SegmentAverageFatigue(
    const State& source,
    double startSeconds,
    double endSeconds,
    std::int64_t atEpochSeconds)
{
    const State state = NormalizeState(source);
    double start = 0.0;
    double end = 0.0;
    if (!ClampInterval(state, startSeconds, endSeconds, start, end))
        return 0.0;

    const double decay = ViewDecayFactor(state, atEpochSeconds);
    double weightedFatigue = 0.0;
    double coveredDuration = 0.0;
    ForEachOverlap(
        state, start, end,
        [&](std::size_t index, double overlap, double) {
            weightedFatigue += state.bucketFatigue[index] * decay * overlap;
            coveredDuration += overlap;
        });
    if (coveredDuration <= 0.0) return 0.0;
    return std::clamp(weightedFatigue / coveredDuration, 0.0, 1.0);
}

double SegmentExplorationScore(
    const State& state,
    double startSeconds,
    double endSeconds,
    std::int64_t atEpochSeconds)
{
    return std::clamp(
        1.0 - SegmentAverageFatigue(
            state, startSeconds, endSeconds, atEpochSeconds),
        0.0, 1.0);
}

Summary Summarize(
    const State& source,
    std::int64_t atEpochSeconds,
    double fatigueCoverageThreshold)
{
    const State state = NormalizeState(source);
    Summary summary;
    if (state.trackDurationSeconds <= 0.0) return summary;

    const double threshold = std::isfinite(fatigueCoverageThreshold)
        ? std::clamp(fatigueCoverageThreshold, 0.0, 1.0)
        : 0.01;
    const double decay = ViewDecayFactor(state, atEpochSeconds);
    double fatigueDuration = 0.0;
    double coveredDuration = 0.0;
    for (std::size_t index = 0; index < state.bucketFatigue.size(); ++index)
    {
        const double bucketStart =
            static_cast<double>(index) * state.bucketDurationSeconds;
        const double bucketEnd = std::min(
            bucketStart + state.bucketDurationSeconds,
            state.trackDurationSeconds);
        const double duration = std::max(bucketEnd - bucketStart, 0.0);
        const double fatigue =
            SanitizeFatigue(state.bucketFatigue[index] * decay);
        fatigueDuration += fatigue * duration;
        if (fatigue >= threshold) coveredDuration += duration;
        summary.maxFatigue = std::max(summary.maxFatigue, fatigue);
    }
    summary.coverage = std::clamp(
        coveredDuration / state.trackDurationSeconds, 0.0, 1.0);
    summary.meanFatigue = std::clamp(
        fatigueDuration / state.trackDurationSeconds, 0.0, 1.0);
    return summary;
}

State ExportState(const State& state)
{
    return NormalizeState(state);
}

bool ImportState(const State& exported, State& destination)
{
    if (exported.schemaVersion != StateSchemaVersion ||
        !std::isfinite(exported.trackDurationSeconds) ||
        exported.trackDurationSeconds < 0.0 ||
        exported.trackDurationSeconds > MaximumTrackDurationSeconds ||
        !std::isfinite(exported.bucketDurationSeconds) ||
        exported.bucketDurationSeconds < MinimumBucketDurationSeconds ||
        exported.bucketDurationSeconds > MaximumBucketDurationSeconds ||
        !std::isfinite(exported.dailyDecayFactor) ||
        exported.dailyDecayFactor <= 0.0 ||
        exported.dailyDecayFactor > 1.0)
    {
        return false;
    }
    const std::size_t expectedCount = ExpectedBucketCount(
        exported.trackDurationSeconds,
        exported.bucketDurationSeconds);
    if (exported.bucketFatigue.size() != expectedCount) return false;

    destination = NormalizeState(exported);
    return true;
}

} // namespace TSM::ListeningHeatmap
