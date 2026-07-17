#pragma once

#include "tsm_listening_heatmap.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace TSM::MusicAnalysis
{

enum class CandidateKind
{
    Entry,
    Exit
};

// One time-domain analysis sample. Scalar features are normalized to [0, 1]
// by AnalyzeTrack, except loudness / true peak (dB), centroid (Hz), time
// (seconds), and energySlope ([-1, 1]).
struct Frame
{
    float timeSeconds = 0.0f;
    float energy = 0.0f;
    // Linear root-mean-square amplitude normalized to [0, 1].
    float rms = 0.0f;
    float loudnessLufs = -80.0f;
    float truePeakDb = -80.0f;
    float spectralCentroidHz = 0.0f;
    float spectralFlux = 0.0f;
    float onsetStrength = 0.0f;
    float silenceProbability = 1.0f;
    float energySlope = 0.0f;
};

// Local acoustic signature. Generated entry candidates summarize only audible
// frames at/after the boundary; exits summarize only audible frames at/before
// it, so compatibility never leaks audio from the wrong side of a cut.
struct Profile
{
    float energy = 0.0f;
    float loudnessLufs = -80.0f;
    float spectralCentroidHz = 0.0f;
    float spectralFlux = 0.0f;
    float onsetStrength = 0.0f;
};

struct Candidate
{
    CandidateKind kind = CandidateKind::Entry;
    std::size_t frameIndex = 0;
    float timeSeconds = 0.0f;
    float score = 0.0f;
    Profile profile;
};

struct TrackAnalysis
{
    float durationSeconds = 0.0f;
    std::vector<Frame> frames;
    // Candidates are ordered best-first with deterministic time tie-breaking.
    std::vector<Candidate> entryCandidates;
    std::vector<Candidate> exitCandidates;
};

struct CandidateGenerationOptions
{
    // Half of this budget is reserved for quality-ranked local maxima and
    // half for temporal coverage, so multi-hour loops remain explorable.
    std::size_t maxCandidatesPerKind = 512;
    float minCandidateSpacingSeconds = 0.5f;
    std::size_t profileRadiusFrames = 2;
};

struct CompatibilityScore
{
    float total = 0.0f;
    float loudness = 0.0f;
    float energy = 0.0f;
    float texture = 0.0f;
    float onset = 0.0f;
};

enum class SegmentReason
{
    FullTrackShorterThanMinimum,
    DurationNearTarget,
    StrongEntry,
    NaturalExit,
    CompatibleWithPrevious,
    UnderexploredRegion,
    FreshTransitionPair,
    WeightedTopCandidate,
    NoCandidatePairFallback
};

struct SegmentSelectionOptions
{
    float minDurationSeconds = 45.0f;
    float targetDurationSeconds = 150.0f;
    float maxDurationSeconds = 240.0f;
    // The weighted draw is deliberately capped at ten candidates even when a
    // larger value is supplied.
    std::size_t topCandidateCount = 10;
    // Smaller values increasingly favour the highest-scoring pair. A value of
    // zero selects the best pair without consuming the random generator.
    float temperature = 0.2f;
    // Optional durable listening memory. When absent, segment scoring remains
    // purely acoustic/duration based.
    const ListeningHeatmap::State* listeningFatigue = nullptr;
    std::int64_t selectionEpochSeconds = 0;
    // Runtime fine-scoring budget per boundary kind. Half is reserved for the
    // best quality candidates and half for deterministic timeline coverage.
    // Values above 512 are clamped to the hard safety ceiling.
    std::size_t maxCandidatesPerKind = 128u;
};

struct SegmentDecision
{
    float startSeconds = 0.0f;
    float endSeconds = 0.0f;
    float durationSeconds = 0.0f;
    float score = 0.0f;
    float explorationScore = 0.0f;
    float transitionDiversityScore = 1.0f;
    bool fullTrack = false;
    Candidate selectedEntry;
    Candidate selectedExit;
    std::vector<SegmentReason> reasons;
};

Profile BuildProfile(
    const std::vector<Frame>& frames,
    std::size_t frameIndex,
    std::size_t radiusFrames = 2);

// Builds the acoustic signature on the audible side of a transition
// boundary. Entries inspect only frames at/after the boundary; exits inspect
// only frames at/before it.
Profile BuildBoundaryProfile(
    const std::vector<Frame>& frames,
    std::size_t frameIndex,
    CandidateKind kind,
    std::size_t radiusFrames = 2);

float ScoreCandidate(
    const std::vector<Frame>& frames,
    std::size_t frameIndex,
    CandidateKind kind);

std::vector<Candidate> GenerateCandidates(
    const std::vector<Frame>& frames,
    CandidateKind kind,
    const CandidateGenerationOptions& options = {});

TrackAnalysis AnalyzeTrack(
    float durationSeconds,
    std::vector<Frame> frames,
    const CandidateGenerationOptions& options = {});

CompatibilityScore ScoreCompatibility(
    const Profile& outgoingExit,
    const Profile& incomingEntry);

SegmentDecision ChooseSegment(
    const TrackAnalysis& analysis,
    const SegmentSelectionOptions& options,
    std::mt19937& random,
    const std::optional<Profile>& outgoingExit = std::nullopt);

const char* ToString(SegmentReason reason);

} // namespace TSM::MusicAnalysis
