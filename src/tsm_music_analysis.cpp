#include "tsm_music_analysis.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace TSM::MusicAnalysis
{
namespace
{

constexpr float ScoreEpsilon = 0.000001f;
constexpr float MaximumLoudnessLufs = 12.0f;
constexpr float MinimumLoudnessLufs = -100.0f;
constexpr float MaximumCentroidHz = 96000.0f;
constexpr std::size_t MaximumSelectionCandidatesPerKind = 512u;
constexpr std::size_t MaximumTopCandidateCount = 10u;

float FiniteOr(float value, float fallback)
{
    return std::isfinite(value) ? value : fallback;
}

float Unit(float value)
{
    return std::clamp(FiniteOr(value, 0.0f), 0.0f, 1.0f);
}

float Normalize(float value, float minimum, float maximum)
{
    if (maximum <= minimum) return 0.0f;
    return Unit((FiniteOr(value, minimum) - minimum) / (maximum - minimum));
}

float GaussianDifference(float difference, float standardDeviation)
{
    const float sigma = std::max(standardDeviation, 0.0001f);
    const float normalized = FiniteOr(difference, sigma * 10.0f) / sigma;
    return std::exp(-0.5f * normalized * normalized);
}

Frame SanitizeFrame(Frame frame)
{
    frame.timeSeconds = std::max(FiniteOr(frame.timeSeconds, 0.0f), 0.0f);
    frame.energy = Unit(frame.energy);
    frame.rms = Unit(frame.rms);
    frame.loudnessLufs = std::clamp(
        FiniteOr(frame.loudnessLufs, -80.0f),
        MinimumLoudnessLufs, MaximumLoudnessLufs);
    frame.truePeakDb = std::clamp(
        FiniteOr(frame.truePeakDb, -80.0f),
        MinimumLoudnessLufs, MaximumLoudnessLufs);
    frame.spectralCentroidHz = std::clamp(
        FiniteOr(frame.spectralCentroidHz, 0.0f),
        0.0f, MaximumCentroidHz);
    frame.spectralFlux = Unit(frame.spectralFlux);
    frame.onsetStrength = Unit(frame.onsetStrength);
    frame.silenceProbability = Unit(frame.silenceProbability);
    frame.energySlope = std::clamp(
        FiniteOr(frame.energySlope, 0.0f), -1.0f, 1.0f);
    return frame;
}

Profile SanitizeProfile(Profile profile)
{
    profile.energy = Unit(profile.energy);
    profile.loudnessLufs = std::clamp(
        FiniteOr(profile.loudnessLufs, -80.0f),
        MinimumLoudnessLufs, MaximumLoudnessLufs);
    profile.spectralCentroidHz = std::clamp(
        FiniteOr(profile.spectralCentroidHz, 0.0f),
        0.0f, MaximumCentroidHz);
    profile.spectralFlux = Unit(profile.spectralFlux);
    profile.onsetStrength = Unit(profile.onsetStrength);
    return profile;
}

bool IsAudibleProfileFrame(const Frame& frame)
{
    return frame.silenceProbability < 0.95f &&
        (frame.energy > 0.001f || frame.rms > 0.001f ||
         frame.loudnessLufs > -70.0f);
}

Profile BuildProfileRange(
    const std::vector<Frame>& frames,
    std::size_t first,
    std::size_t last,
    bool audibleOnly)
{
    if (frames.empty()) return {};

    first = std::min(first, frames.size() - 1u);
    last = std::min(std::max(last, first), frames.size() - 1u);

    double energy = 0.0;
    double loudness = 0.0;
    double centroid = 0.0;
    double flux = 0.0;
    double onset = 0.0;
    std::size_t count = 0u;
    for (std::size_t index = first; index <= last; ++index)
    {
        const Frame frame = SanitizeFrame(frames[index]);
        if (audibleOnly && !IsAudibleProfileFrame(frame)) continue;
        energy += frame.energy;
        loudness += frame.loudnessLufs;
        centroid += frame.spectralCentroidHz;
        flux += frame.spectralFlux;
        onset += frame.onsetStrength;
        ++count;
    }

    if (count == 0u) return {};
    const double divisor = static_cast<double>(count);
    Profile profile;
    profile.energy = static_cast<float>(energy / divisor);
    profile.loudnessLufs = static_cast<float>(loudness / divisor);
    profile.spectralCentroidHz = static_cast<float>(centroid / divisor);
    profile.spectralFlux = static_cast<float>(flux / divisor);
    profile.onsetStrength = static_cast<float>(onset / divisor);
    return SanitizeProfile(profile);
}

Profile BuildDirectionalProfile(
    const std::vector<Frame>& frames,
    std::size_t frameIndex,
    CandidateKind kind,
    std::size_t radiusFrames)
{
    if (frames.empty()) return {};

    frameIndex = std::min(frameIndex, frames.size() - 1u);
    if (kind == CandidateKind::Entry)
    {
        const std::size_t availableAfter = frames.size() - 1u - frameIndex;
        const std::size_t last = radiusFrames > availableAfter
            ? frames.size() - 1u
            : frameIndex + radiusFrames;
        return BuildProfileRange(
            frames, frameIndex, last, true);
    }

    const std::size_t first = frameIndex > radiusFrames
        ? frameIndex - radiusFrames
        : 0u;
    return BuildProfileRange(frames, first, frameIndex, true);
}

bool BetterCandidate(const Candidate& left, const Candidate& right)
{
    if (std::fabs(left.score - right.score) > ScoreEpsilon)
        return left.score > right.score;
    if (left.kind == CandidateKind::Entry)
        return left.timeSeconds < right.timeSeconds;
    return left.timeSeconds > right.timeSeconds;
}

std::vector<std::size_t> SelectCandidateIndices(
    const std::vector<Candidate>& candidates,
    std::size_t requestedBudget)
{
    const std::size_t sourceCount = std::min(
        candidates.size(), MaximumSelectionCandidatesPerKind);
    const std::size_t budget = std::min(requestedBudget, sourceCount);
    std::vector<std::size_t> selected;
    selected.reserve(budget);
    if (budget == 0u) return selected;

    std::vector<bool> used(sourceCount, false);
    const std::size_t qualityBudget = (budget + 1u) / 2u;
    for (std::size_t index = 0u; index < qualityBudget; ++index)
    {
        selected.push_back(index);
        used[index] = true;
    }

    const std::size_t coverageBudget = budget - selected.size();
    if (coverageBudget == 0u) return selected;

    std::vector<std::size_t> timeOrder(sourceCount);
    std::iota(timeOrder.begin(), timeOrder.end(), 0u);
    std::stable_sort(
        timeOrder.begin(), timeOrder.end(),
        [&](std::size_t left, std::size_t right) {
            const float leftTime = FiniteOr(
                candidates[left].timeSeconds, 0.0f);
            const float rightTime = FiniteOr(
                candidates[right].timeSeconds, 0.0f);
            if (std::fabs(leftTime - rightTime) > ScoreEpsilon)
                return leftTime < rightTime;
            return left < right;
        });

    const auto addNearestUnused = [&](std::size_t requestedPosition) {
        for (std::size_t distance = 0u; distance < sourceCount; ++distance)
        {
            if (requestedPosition >= distance)
            {
                const std::size_t position = requestedPosition - distance;
                const std::size_t index = timeOrder[position];
                if (!used[index])
                {
                    used[index] = true;
                    selected.push_back(index);
                    return true;
                }
            }
            if (distance > 0u &&
                requestedPosition + distance < sourceCount)
            {
                const std::size_t position = requestedPosition + distance;
                const std::size_t index = timeOrder[position];
                if (!used[index])
                {
                    used[index] = true;
                    selected.push_back(index);
                    return true;
                }
            }
        }
        return false;
    };

    for (std::size_t slot = 0u;
         slot < coverageBudget && selected.size() < budget;
         ++slot)
    {
        const std::size_t position = coverageBudget <= 1u
            ? (sourceCount - 1u) / 2u
            : slot * (sourceCount - 1u) / (coverageBudget - 1u);
        (void)addNearestUnused(position);
    }
    for (std::size_t position = 0u;
         position < sourceCount && selected.size() < budget;
         ++position)
    {
        const std::size_t index = timeOrder[position];
        if (used[index]) continue;
        used[index] = true;
        selected.push_back(index);
    }
    return selected;
}

std::size_t NearestFrameIndex(
    const std::vector<Frame>& frames, float timeSeconds)
{
    if (frames.empty()) return 0;
    const auto iterator = std::lower_bound(
        frames.begin(), frames.end(), timeSeconds,
        [](const Frame& frame, float time) {
            return frame.timeSeconds < time;
        });
    if (iterator == frames.begin()) return 0;
    if (iterator == frames.end()) return frames.size() - 1;

    const std::size_t next = static_cast<std::size_t>(
        std::distance(frames.begin(), iterator));
    const std::size_t previous = next - 1;
    return std::fabs(frames[previous].timeSeconds - timeSeconds) <=
            std::fabs(frames[next].timeSeconds - timeSeconds)
        ? previous
        : next;
}

Candidate CandidateAtTime(
    const TrackAnalysis& analysis, CandidateKind kind, float timeSeconds)
{
    Candidate candidate;
    candidate.kind = kind;
    candidate.timeSeconds = std::clamp(
        FiniteOr(timeSeconds, 0.0f), 0.0f,
        std::max(analysis.durationSeconds, 0.0f));
    if (!analysis.frames.empty())
    {
        candidate.frameIndex = NearestFrameIndex(
            analysis.frames, candidate.timeSeconds);
        candidate.score = ScoreCandidate(
            analysis.frames, candidate.frameIndex, kind);
        candidate.profile = BuildDirectionalProfile(
            analysis.frames, candidate.frameIndex, kind, 2u);
    }
    return candidate;
}

struct SegmentPair
{
    Candidate entry;
    Candidate exit;
    float durationSeconds = 0.0f;
    float durationScore = 0.0f;
    CompatibilityScore compatibility;
    float explorationScore = 0.0f;
    float score = 0.0f;
};

class FatigueWindowScorer final
{
public:
    FatigueWindowScorer(
        const ListeningHeatmap::State& source,
        std::int64_t epochSeconds)
        : m_state(ListeningHeatmap::ExportState(source))
    {
        ListeningHeatmap::DecayTo(m_state, epochSeconds);
        m_prefixFatigueDuration.resize(
            m_state.bucketFatigue.size() + 1u, 0.0);
        for (std::size_t index = 0; index < m_state.bucketFatigue.size(); ++index)
        {
            const double bucketStart =
                index * m_state.bucketDurationSeconds;
            const double bucketEnd = std::min(
                bucketStart + m_state.bucketDurationSeconds,
                m_state.trackDurationSeconds);
            const double duration = std::max(bucketEnd - bucketStart, 0.0);
            m_prefixFatigueDuration[index + 1u] =
                m_prefixFatigueDuration[index] +
                m_state.bucketFatigue[index] * duration;
        }
    }

    float ExplorationScore(float requestedStart, float requestedEnd) const
    {
        if (m_state.bucketFatigue.empty() ||
            m_state.bucketDurationSeconds <= 0.0)
            return 1.0f;
        const double start = std::clamp(
            static_cast<double>(requestedStart),
            0.0, m_state.trackDurationSeconds);
        const double end = std::clamp(
            static_cast<double>(requestedEnd),
            0.0, m_state.trackDurationSeconds);
        if (end <= start) return 1.0f;

        const std::size_t first = std::min(
            static_cast<std::size_t>(
                std::floor(start / m_state.bucketDurationSeconds)),
            m_state.bucketFatigue.size() - 1u);
        const std::size_t last = std::min(
            static_cast<std::size_t>(
                std::floor(std::nextafter(end, start) /
                           m_state.bucketDurationSeconds)),
            m_state.bucketFatigue.size() - 1u);
        double fatigueDuration = 0.0;
        if (first == last)
        {
            fatigueDuration = m_state.bucketFatigue[first] * (end - start);
        }
        else
        {
            const double firstEnd = std::min(
                (first + 1u) * m_state.bucketDurationSeconds,
                m_state.trackDurationSeconds);
            const double lastStart = last * m_state.bucketDurationSeconds;
            fatigueDuration =
                m_state.bucketFatigue[first] * (firstEnd - start) +
                m_state.bucketFatigue[last] * (end - lastStart);
            if (last > first + 1u)
            {
                fatigueDuration +=
                    m_prefixFatigueDuration[last] -
                    m_prefixFatigueDuration[first + 1u];
            }
        }
        const double average = std::clamp(
            fatigueDuration / (end - start), 0.0, 1.0);
        return static_cast<float>(1.0 - average);
    }

    double BucketDurationSeconds() const
    {
        return m_state.bucketDurationSeconds;
    }

private:
    ListeningHeatmap::State m_state;
    std::vector<double> m_prefixFatigueDuration;
};

bool BetterPair(
    const SegmentPair& left,
    const SegmentPair& right,
    float targetDurationSeconds)
{
    if (std::fabs(left.score - right.score) > ScoreEpsilon)
        return left.score > right.score;

    const float leftTargetDistance =
        std::fabs(left.durationSeconds - targetDurationSeconds);
    const float rightTargetDistance =
        std::fabs(right.durationSeconds - targetDurationSeconds);
    if (std::fabs(leftTargetDistance - rightTargetDistance) > ScoreEpsilon)
        return leftTargetDistance < rightTargetDistance;
    if (std::fabs(left.entry.timeSeconds - right.entry.timeSeconds) >
        ScoreEpsilon)
    {
        return left.entry.timeSeconds < right.entry.timeSeconds;
    }
    return left.exit.timeSeconds < right.exit.timeSeconds;
}

bool ContainsReason(
    const std::vector<SegmentReason>& reasons, SegmentReason reason)
{
    return std::find(reasons.begin(), reasons.end(), reason) != reasons.end();
}

void AddReason(std::vector<SegmentReason>& reasons, SegmentReason reason)
{
    if (!ContainsReason(reasons, reason)) reasons.push_back(reason);
}

} // namespace

Profile BuildProfile(
    const std::vector<Frame>& frames,
    std::size_t frameIndex,
    std::size_t radiusFrames)
{
    if (frames.empty()) return {};

    frameIndex = std::min(frameIndex, frames.size() - 1u);
    const std::size_t first = frameIndex > radiusFrames
        ? frameIndex - radiusFrames
        : 0u;
    const std::size_t availableAfter = frames.size() - 1u - frameIndex;
    const std::size_t last = radiusFrames > availableAfter
        ? frames.size() - 1u
        : frameIndex + radiusFrames;
    // Keep the original public, symmetric profile API for callers that need a
    // general neighbourhood. Entry/exit candidates use directional profiles
    // internally so a transition never incorporates audio outside its side.
    return BuildProfileRange(frames, first, last, false);
}

Profile BuildBoundaryProfile(
    const std::vector<Frame>& frames,
    std::size_t frameIndex,
    CandidateKind kind,
    std::size_t radiusFrames)
{
    return BuildDirectionalProfile(
        frames, frameIndex, kind, radiusFrames);
}

float ScoreCandidate(
    const std::vector<Frame>& frames,
    std::size_t frameIndex,
    CandidateKind kind)
{
    if (frames.empty() || frameIndex >= frames.size()) return 0.0f;

    const Frame current = SanitizeFrame(frames[frameIndex]);
    const Frame previous = SanitizeFrame(
        frames[frameIndex == 0 ? 0 : frameIndex - 1]);
    const Frame next = SanitizeFrame(
        frames[std::min(frameIndex + 1, frames.size() - 1)]);
    const float observedSlope = std::clamp(
        0.5f * current.energySlope +
            0.5f * (next.energy - previous.energy),
        -1.0f, 1.0f);
    const float loudnessPresence = Normalize(
        current.loudnessLufs, -60.0f, -8.0f);

    if (kind == CandidateKind::Entry)
    {
        const float stability = Unit(
            1.0f - 0.5f * std::fabs(observedSlope) -
                0.5f * current.spectralFlux);
        const float absenceOfSilence = 1.0f - current.silenceProbability;
        const float lowBrutalChange =
            1.0f - std::max(current.onsetStrength, current.spectralFlux);
        const float suitableEnergy = GaussianDifference(
            current.energy - 0.45f, 0.30f);
        const float phraseStart = Unit(
            previous.silenceProbability *
            GaussianDifference(current.onsetStrength - 0.35f, 0.25f));
        const float score =
            0.30f * stability +
            0.25f * absenceOfSilence +
            0.20f * lowBrutalChange +
            0.15f * suitableEnergy +
            0.10f * phraseStart;
        return Unit(score);
    }

    const float fallingEnergy = std::max(-observedSlope, 0.0f);
    const float boundaryBonus = frameIndex + 1 == frames.size() ? 1.0f : 0.0f;
    const float centroidRelease =
        0.5f * (1.0f - Normalize(
            current.spectralCentroidHz, 100.0f, 8000.0f)) +
        0.5f * Unit(
            (current.spectralCentroidHz - next.spectralCentroidHz) /
            6000.0f);
    const float score =
        0.22f * next.silenceProbability +
        0.17f * fallingEnergy +
        0.13f * (1.0f - current.onsetStrength) +
        0.12f * (1.0f - current.energy) +
        0.09f * (1.0f - current.spectralFlux) +
        0.08f * current.silenceProbability +
        0.08f * (1.0f - loudnessPresence) +
        0.06f * centroidRelease +
        0.05f * boundaryBonus;
    return Unit(score);
}

std::vector<Candidate> GenerateCandidates(
    const std::vector<Frame>& frames,
    CandidateKind kind,
    const CandidateGenerationOptions& options)
{
    if (frames.empty() || options.maxCandidatesPerKind == 0) return {};

    std::vector<float> scores(frames.size());
    for (std::size_t index = 0; index < frames.size(); ++index)
        scores[index] = ScoreCandidate(frames, index, kind);

    std::vector<Candidate> candidates;
    candidates.reserve(frames.size());
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        const bool localMaximum =
            (index == 0 || scores[index] + ScoreEpsilon >= scores[index - 1]) &&
            (index + 1 == frames.size() ||
                scores[index] + ScoreEpsilon >= scores[index + 1]);
        const bool requiredBoundary =
            (kind == CandidateKind::Entry && index == 0) ||
            (kind == CandidateKind::Exit && index + 1 == frames.size());
        if (!localMaximum && !requiredBoundary) continue;

        Candidate candidate;
        candidate.kind = kind;
        candidate.frameIndex = index;
        candidate.timeSeconds = std::max(
            FiniteOr(frames[index].timeSeconds, 0.0f), 0.0f);
        candidate.score = scores[index];
        candidate.profile = BuildDirectionalProfile(
            frames, index, kind, options.profileRadiusFrames);
        candidates.push_back(candidate);
    }

    std::stable_sort(candidates.begin(), candidates.end(), BetterCandidate);
    const float spacing = std::max(
        FiniteOr(options.minCandidateSpacingSeconds, 0.0f), 0.0f);
    std::vector<Candidate> selected;
    selected.reserve(std::min(
        frames.size(), options.maxCandidatesPerKind));
    const auto trySelect = [&](const Candidate& candidate) {
        const bool spaced = std::all_of(
            selected.begin(), selected.end(), [&](const Candidate& existing) {
                return std::fabs(
                    existing.timeSeconds - candidate.timeSeconds) +
                    ScoreEpsilon >= spacing;
            });
        if (!spaced) return false;
        selected.push_back(candidate);
        return true;
    };

    const std::size_t qualityBudget = std::max<std::size_t>(
        1u, options.maxCandidatesPerKind / 2u);
    for (const Candidate& candidate : candidates)
    {
        (void)trySelect(candidate);
        if (selected.size() >= qualityBudget) break;
    }

    // Reserve candidates across the complete timeline rather than allowing a
    // three-hour loop's globally strongest minute to consume the whole cache.
    const std::size_t coverageBudget = options.maxCandidatesPerKind -
        std::min(selected.size(), options.maxCandidatesPerKind);
    if (coverageBudget > 0u)
    {
        const double framesPerSlot = static_cast<double>(frames.size()) /
            static_cast<double>(coverageBudget);
        for (std::size_t slot = 0;
             slot < coverageBudget &&
             selected.size() < options.maxCandidatesPerKind;
             ++slot)
        {
            const std::size_t first = std::min(
                static_cast<std::size_t>(std::floor(slot * framesPerSlot)),
                frames.size() - 1u);
            const std::size_t last = std::min(
                static_cast<std::size_t>(
                    std::ceil((slot + 1u) * framesPerSlot)),
                frames.size());
            std::size_t bestIndex = first;
            for (std::size_t index = first + 1u; index < last; ++index)
            {
                if (scores[index] > scores[bestIndex] + ScoreEpsilon)
                    bestIndex = index;
            }
            Candidate candidate;
            candidate.kind = kind;
            candidate.frameIndex = bestIndex;
            candidate.timeSeconds = frames[bestIndex].timeSeconds;
            candidate.score = scores[bestIndex];
            candidate.profile = BuildDirectionalProfile(
                frames, bestIndex, kind, options.profileRadiusFrames);
            (void)trySelect(candidate);
        }
    }

    if (selected.size() < options.maxCandidatesPerKind)
    {
        for (const Candidate& candidate : candidates)
        {
            (void)trySelect(candidate);
            if (selected.size() == options.maxCandidatesPerKind) break;
        }
    }
    std::stable_sort(selected.begin(), selected.end(), BetterCandidate);
    return selected;
}

TrackAnalysis AnalyzeTrack(
    float durationSeconds,
    std::vector<Frame> frames,
    const CandidateGenerationOptions& options)
{
    frames.erase(
        std::remove_if(frames.begin(), frames.end(), [](const Frame& frame) {
            return !std::isfinite(frame.timeSeconds);
        }),
        frames.end());
    for (Frame& frame : frames) frame = SanitizeFrame(frame);
    std::stable_sort(
        frames.begin(), frames.end(), [](const Frame& left, const Frame& right) {
            return left.timeSeconds < right.timeSeconds;
        });

    float safeDuration = std::max(FiniteOr(durationSeconds, 0.0f), 0.0f);
    if (safeDuration <= 0.0f && !frames.empty())
        safeDuration = frames.back().timeSeconds;
    for (Frame& frame : frames)
        frame.timeSeconds = std::min(frame.timeSeconds, safeDuration);

    TrackAnalysis analysis;
    analysis.durationSeconds = safeDuration;
    analysis.frames = std::move(frames);
    analysis.entryCandidates = GenerateCandidates(
        analysis.frames, CandidateKind::Entry, options);
    analysis.exitCandidates = GenerateCandidates(
        analysis.frames, CandidateKind::Exit, options);
    return analysis;
}

CompatibilityScore ScoreCompatibility(
    const Profile& outgoingExit,
    const Profile& incomingEntry)
{
    const Profile outgoing = SanitizeProfile(outgoingExit);
    const Profile incoming = SanitizeProfile(incomingEntry);

    CompatibilityScore score;
    score.loudness = GaussianDifference(
        outgoing.loudnessLufs - incoming.loudnessLufs, 6.0f);
    score.energy = GaussianDifference(
        outgoing.energy - incoming.energy, 0.25f);

    constexpr float CentroidFloorHz = 80.0f;
    const float octaveDifference = std::log2(
        (outgoing.spectralCentroidHz + CentroidFloorHz) /
        (incoming.spectralCentroidHz + CentroidFloorHz));
    const float centroidCompatibility = GaussianDifference(
        octaveDifference, 1.25f);
    const float fluxCompatibility = GaussianDifference(
        outgoing.spectralFlux - incoming.spectralFlux, 0.30f);
    score.texture =
        0.70f * centroidCompatibility + 0.30f * fluxCompatibility;

    score.onset = GaussianDifference(
        outgoing.onsetStrength - incoming.onsetStrength, 0.30f);
    score.total =
        0.35f * score.loudness +
        0.25f * score.energy +
        0.25f * score.texture +
        0.15f * score.onset;
    score.total = Unit(score.total);
    return score;
}

SegmentDecision ChooseSegment(
    const TrackAnalysis& analysis,
    const SegmentSelectionOptions& options,
    std::mt19937& random,
    const std::optional<Profile>& outgoingExit)
{
    const float trackDuration = std::max(
        FiniteOr(analysis.durationSeconds, 0.0f), 0.0f);
    const float minimum = std::max(
        FiniteOr(options.minDurationSeconds, 45.0f), 0.001f);
    const float maximum = std::max(
        FiniteOr(options.maxDurationSeconds, 240.0f), minimum);
    const float effectiveMaximum = std::min(maximum, trackDuration);
    const float target = std::clamp(
        FiniteOr(options.targetDurationSeconds, 150.0f),
        minimum, maximum);

    SegmentDecision decision;
    const std::optional<FatigueWindowScorer> fatigueScorer =
        options.listeningFatigue
        ? std::optional<FatigueWindowScorer>(std::in_place,
              *options.listeningFatigue,
              options.selectionEpochSeconds)
        : std::nullopt;
    if (trackDuration < minimum || trackDuration <= ScoreEpsilon)
    {
        decision.endSeconds = trackDuration;
        decision.durationSeconds = trackDuration;
        decision.score = 1.0f;
        decision.fullTrack = true;
        decision.selectedEntry = CandidateAtTime(
            analysis, CandidateKind::Entry, 0.0f);
        decision.selectedExit = CandidateAtTime(
            analysis, CandidateKind::Exit, trackDuration);
        if (fatigueScorer)
        {
            decision.explorationScore =
                fatigueScorer->ExplorationScore(0.0f, trackDuration);
        }
        AddReason(
            decision.reasons,
            SegmentReason::FullTrackShorterThanMinimum);
        return decision;
    }

    const float effectiveTarget = std::clamp(
        target, minimum, effectiveMaximum);
    const float durationSigma = std::max(
        (effectiveMaximum - minimum) * 0.25f, 0.25f);

    const std::size_t requestedTopCount = std::max<std::size_t>(
        options.topCandidateCount, 1u);
    const std::size_t topPairCapacity = std::min(
        requestedTopCount, MaximumTopCandidateCount);

    // Generated analyses keep up to 512 boundaries for long-term coverage,
    // while a playback decision uses a smaller explicit real-time budget.
    // Preserve both the best quality half and a timeline-distributed half.
    const std::vector<std::size_t> entryIndices = SelectCandidateIndices(
        analysis.entryCandidates, options.maxCandidatesPerKind);
    const std::vector<std::size_t> exitIndices = SelectCandidateIndices(
        analysis.exitCandidates, options.maxCandidatesPerKind);

    std::vector<Candidate> exitsByTime;
    exitsByTime.reserve(exitIndices.size());
    for (const std::size_t index : exitIndices)
    {
        Candidate exit = analysis.exitCandidates[index];
        exit.kind = CandidateKind::Exit;
        exit.timeSeconds = std::clamp(
            FiniteOr(exit.timeSeconds, 0.0f), 0.0f, trackDuration);
        exit.score = Unit(exit.score);
        exit.profile = SanitizeProfile(exit.profile);
        exitsByTime.push_back(exit);
    }
    std::stable_sort(
        exitsByTime.begin(), exitsByTime.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.timeSeconds < right.timeSeconds;
        });

    // Retain only the exact best K pairs needed by the weighted draw. This is
    // equivalent to sorting every valid pair and taking its prefix, without
    // allocating or sorting the entries x exits Cartesian product.
    std::vector<SegmentPair> pairs;
    pairs.reserve(topPairCapacity);
    const auto considerPair = [&](SegmentPair pair) {
        const auto insertion = std::find_if(
            pairs.begin(), pairs.end(), [&](const SegmentPair& existing) {
                return BetterPair(pair, existing, effectiveTarget);
            });
        if (insertion == pairs.end())
        {
            if (pairs.size() < topPairCapacity)
                pairs.push_back(std::move(pair));
            return;
        }
        pairs.insert(insertion, std::move(pair));
        if (pairs.size() > topPairCapacity) pairs.pop_back();
    };

    for (const std::size_t entryIndex : entryIndices)
    {
        Candidate entry = analysis.entryCandidates[entryIndex];
        entry.kind = CandidateKind::Entry;
        entry.timeSeconds = std::clamp(
            FiniteOr(entry.timeSeconds, 0.0f), 0.0f, trackDuration);
        entry.score = Unit(entry.score);
        entry.profile = SanitizeProfile(entry.profile);

        const float earliestExit =
            entry.timeSeconds + minimum - ScoreEpsilon;
        const float latestExit =
            entry.timeSeconds + effectiveMaximum + ScoreEpsilon;
        const auto firstExit = std::lower_bound(
            exitsByTime.begin(), exitsByTime.end(), earliestExit,
            [](const Candidate& candidate, float timeSeconds) {
                return candidate.timeSeconds < timeSeconds;
            });
        const auto afterLastExit = std::upper_bound(
            firstExit, exitsByTime.end(), latestExit,
            [](float timeSeconds, const Candidate& candidate) {
                return timeSeconds < candidate.timeSeconds;
            });
        const CompatibilityScore entryCompatibility = outgoingExit
            ? ScoreCompatibility(*outgoingExit, entry.profile)
            : CompatibilityScore{};
        for (auto exitIterator = firstExit;
             exitIterator != afterLastExit;
             ++exitIterator)
        {
            const Candidate& exit = *exitIterator;
            const float duration = exit.timeSeconds - entry.timeSeconds;

            SegmentPair pair;
            pair.entry = entry;
            pair.exit = exit;
            pair.durationSeconds = duration;
            pair.durationScore = GaussianDifference(
                duration - effectiveTarget, durationSigma);
            if (fatigueScorer)
            {
                pair.explorationScore = fatigueScorer->ExplorationScore(
                    entry.timeSeconds, exit.timeSeconds);
            }
            if (outgoingExit.has_value())
            {
                pair.compatibility = entryCompatibility;
                pair.score =
                    0.25f * entry.score +
                    0.25f * exit.score +
                    0.25f * pair.durationScore +
                    0.25f * pair.compatibility.total;
            }
            else
            {
                pair.score =
                    0.35f * entry.score +
                    0.35f * exit.score +
                    0.30f * pair.durationScore;
            }
            pair.score = Unit(pair.score);
            if (fatigueScorer)
            {
                // Novelty is deliberately a bonus, not a licence to choose a
                // musically destructive boundary.
                pair.score = Unit(
                    0.85f * pair.score + 0.15f * pair.explorationScore);
            }
            considerPair(std::move(pair));
        }
    }

    if (pairs.empty())
    {
        float start = 0.0f;
        if (!entryIndices.empty())
        {
            start = std::clamp(
                FiniteOr(
                    analysis.entryCandidates[entryIndices.front()].timeSeconds,
                    0.0f),
                0.0f, trackDuration - minimum);
        }
        const float duration = std::clamp(
            effectiveTarget, minimum,
            std::min(effectiveMaximum, trackDuration - start));
        if (fatigueScorer && duration > 0.0f)
        {
            const double step = std::max(
                fatigueScorer->BucketDurationSeconds(), 0.5);
            float bestStart = start;
            float bestExploration = -1.0f;
            for (double candidateStart = 0.0;
                 candidateStart + duration <= trackDuration + ScoreEpsilon;
                 candidateStart += step)
            {
                const float exploration = fatigueScorer->ExplorationScore(
                    static_cast<float>(candidateStart),
                    static_cast<float>(candidateStart + duration));
                if (exploration > bestExploration + ScoreEpsilon)
                {
                    bestExploration = exploration;
                    bestStart = static_cast<float>(candidateStart);
                }
            }
            start = std::clamp(
                bestStart, 0.0f, std::max(trackDuration - duration, 0.0f));
            decision.explorationScore = std::max(bestExploration, 0.0f);
        }
        const float end = std::min(start + duration, trackDuration);

        decision.startSeconds = start;
        decision.endSeconds = end;
        decision.durationSeconds = end - start;
        decision.fullTrack =
            start <= ScoreEpsilon &&
            end + ScoreEpsilon >= trackDuration;
        decision.selectedEntry = CandidateAtTime(
            analysis, CandidateKind::Entry, start);
        decision.selectedExit = CandidateAtTime(
            analysis, CandidateKind::Exit, end);
        const float durationScore = GaussianDifference(
            decision.durationSeconds - effectiveTarget, durationSigma);
        decision.score =
            0.35f * decision.selectedEntry.score +
            0.35f * decision.selectedExit.score +
            0.30f * durationScore;
        if (fatigueScorer)
        {
            decision.score = Unit(
                0.85f * decision.score +
                0.15f * decision.explorationScore);
            if (decision.explorationScore >= 0.70f)
                AddReason(
                    decision.reasons,
                    SegmentReason::UnderexploredRegion);
        }
        AddReason(
            decision.reasons, SegmentReason::NoCandidatePairFallback);
        if (durationScore >= 0.75f)
            AddReason(decision.reasons, SegmentReason::DurationNearTarget);
        return decision;
    }

    const std::size_t topCount = pairs.size();
    std::size_t chosenIndex = 0;
    const float temperature = std::max(
        FiniteOr(options.temperature, 0.0f), 0.0f);
    if (topCount > 1 && temperature > ScoreEpsilon)
    {
        std::vector<double> weights(topCount);
        const float bestScore = pairs.front().score;
        std::transform(
            pairs.begin(), pairs.begin() + topCount, weights.begin(),
            [&](const SegmentPair& pair) {
                return std::exp(static_cast<double>(
                    (pair.score - bestScore) / temperature));
            });
        std::discrete_distribution<std::size_t> distribution(
            weights.begin(), weights.end());
        chosenIndex = distribution(random);
    }

    const SegmentPair& chosen = pairs[chosenIndex];
    decision.startSeconds = chosen.entry.timeSeconds;
    decision.endSeconds = chosen.exit.timeSeconds;
    decision.durationSeconds = chosen.durationSeconds;
    decision.score = chosen.score;
    decision.explorationScore = chosen.explorationScore;
    decision.fullTrack =
        decision.startSeconds <= ScoreEpsilon &&
        decision.endSeconds + ScoreEpsilon >= trackDuration;
    decision.selectedEntry = chosen.entry;
    decision.selectedExit = chosen.exit;
    AddReason(decision.reasons, SegmentReason::WeightedTopCandidate);
    if (chosen.durationScore >= 0.75f)
        AddReason(decision.reasons, SegmentReason::DurationNearTarget);
    if (chosen.entry.score >= 0.65f)
        AddReason(decision.reasons, SegmentReason::StrongEntry);
    if (chosen.exit.score >= 0.65f)
        AddReason(decision.reasons, SegmentReason::NaturalExit);
    if (outgoingExit.has_value() &&
        chosen.compatibility.total >= 0.70f)
    {
        AddReason(
            decision.reasons, SegmentReason::CompatibleWithPrevious);
    }
    if (fatigueScorer && chosen.explorationScore >= 0.70f)
    {
        AddReason(
            decision.reasons, SegmentReason::UnderexploredRegion);
    }
    return decision;
}

const char* ToString(SegmentReason reason)
{
    switch (reason)
    {
        case SegmentReason::FullTrackShorterThanMinimum:
            return "full_track_shorter_than_minimum";
        case SegmentReason::DurationNearTarget:
            return "duration_near_target";
        case SegmentReason::StrongEntry:
            return "strong_entry";
        case SegmentReason::NaturalExit:
            return "natural_exit";
        case SegmentReason::CompatibleWithPrevious:
            return "compatible_with_previous";
        case SegmentReason::UnderexploredRegion:
            return "underexplored_region";
        case SegmentReason::FreshTransitionPair:
            return "fresh_transition_pair";
        case SegmentReason::WeightedTopCandidate:
            return "weighted_top_candidate";
        case SegmentReason::NoCandidatePairFallback:
            return "no_candidate_pair_fallback";
    }
    return "unknown";
}

} // namespace TSM::MusicAnalysis
