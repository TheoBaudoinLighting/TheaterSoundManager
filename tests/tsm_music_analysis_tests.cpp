#include "pch.h"
#include "tsm_music_analysis.h"

#include <limits>

namespace TSM::Tests
{
namespace
{

namespace Analysis = MusicAnalysis;

Analysis::Frame MakeFrame(
    float timeSeconds,
    float energy,
    float loudnessLufs,
    float centroidHz,
    float flux,
    float onset,
    float silence,
    float slope)
{
    Analysis::Frame frame;
    frame.timeSeconds = timeSeconds;
    frame.energy = energy;
    frame.rms = energy;
    frame.loudnessLufs = loudnessLufs;
    frame.spectralCentroidHz = centroidHz;
    frame.spectralFlux = flux;
    frame.onsetStrength = onset;
    frame.silenceProbability = silence;
    frame.energySlope = slope;
    return frame;
}

Analysis::Candidate MakeCandidate(
    Analysis::CandidateKind kind,
    float timeSeconds,
    float score,
    Analysis::Profile profile = {})
{
    Analysis::Candidate candidate;
    candidate.kind = kind;
    candidate.timeSeconds = timeSeconds;
    candidate.score = score;
    candidate.profile = profile;
    return candidate;
}

bool HasReason(
    const Analysis::SegmentDecision& decision,
    Analysis::SegmentReason reason)
{
    return std::find(
        decision.reasons.begin(), decision.reasons.end(), reason) !=
        decision.reasons.end();
}

} // namespace

TEST(MusicAnalysisTests, ScoresStableEntryAndFadeToSilenceAsBoundaries)
{
    const std::vector<Analysis::Frame> frames = {
        MakeFrame(0.0f, 0.0f, -80.0f, 100.0f, 0.0f, 0.0f, 1.0f, 0.0f),
        MakeFrame(1.0f, 0.8f, -14.0f, 3000.0f, 0.9f, 1.0f, 0.0f, 1.0f),
        MakeFrame(2.0f, 0.5f, -18.0f, 2200.0f, 0.05f, 0.15f, 0.0f, 0.0f),
        MakeFrame(3.0f, 0.3f, -30.0f, 1600.0f, 0.1f, 0.0f, 0.1f, -1.0f),
        MakeFrame(4.0f, 0.0f, -80.0f, 100.0f, 0.0f, 0.0f, 1.0f, -1.0f)
    };

    EXPECT_GT(
        Analysis::ScoreCandidate(frames, 2, Analysis::CandidateKind::Entry),
        Analysis::ScoreCandidate(frames, 1, Analysis::CandidateKind::Entry));
    EXPECT_GT(
        Analysis::ScoreCandidate(frames, 4, Analysis::CandidateKind::Exit),
        Analysis::ScoreCandidate(frames, 1, Analysis::CandidateKind::Exit));

    const Analysis::TrackAnalysis track =
        Analysis::AnalyzeTrack(5.0f, frames);
    EXPECT_TRUE(std::any_of(
        track.entryCandidates.begin(), track.entryCandidates.end(),
        [](const Analysis::Candidate& candidate) {
            return candidate.frameIndex == 2;
        }));
    EXPECT_TRUE(std::any_of(
        track.exitCandidates.begin(), track.exitCandidates.end(),
        [](const Analysis::Candidate& candidate) {
            return candidate.frameIndex == 4;
        }));
}

TEST(MusicAnalysisTests, AnalysisSanitizesAndOrdersFrames)
{
    const float infinity = (std::numeric_limits<float>::infinity)();
    std::vector<Analysis::Frame> frames = {
        MakeFrame(3.0f, 2.0f, 40.0f, -10.0f, 3.0f, -1.0f, 2.0f, 5.0f),
        MakeFrame(1.0f, -2.0f, -200.0f, infinity, -3.0f, 2.0f, -1.0f, -5.0f),
        MakeFrame(infinity, 0.5f, -20.0f, 2000.0f, 0.5f, 0.5f, 0.0f, 0.0f)
    };
    frames[0].rms = 2.0f;
    frames[1].rms = -2.0f;
    frames[2].rms = infinity;

    const Analysis::TrackAnalysis track =
        Analysis::AnalyzeTrack(2.0f, std::move(frames));
    ASSERT_EQ(track.frames.size(), 2u);
    EXPECT_FLOAT_EQ(track.frames[0].timeSeconds, 1.0f);
    EXPECT_FLOAT_EQ(track.frames[1].timeSeconds, 2.0f);
    EXPECT_FLOAT_EQ(track.frames[0].energy, 0.0f);
    EXPECT_FLOAT_EQ(track.frames[1].energy, 1.0f);
    EXPECT_FLOAT_EQ(track.frames[0].rms, 0.0f);
    EXPECT_FLOAT_EQ(track.frames[1].rms, 1.0f);
    EXPECT_FLOAT_EQ(track.frames[0].spectralCentroidHz, 0.0f);
    EXPECT_FLOAT_EQ(track.frames[1].spectralCentroidHz, 0.0f);
    EXPECT_FLOAT_EQ(track.frames[0].energySlope, -1.0f);
    EXPECT_FLOAT_EQ(track.frames[1].energySlope, 1.0f);
}

TEST(MusicAnalysisTests, AnalysisSanitizesLinearRmsAmplitude)
{
    const float infinity = (std::numeric_limits<float>::infinity)();
    std::vector<Analysis::Frame> frames = {
        MakeFrame(0.0f, 0.5f, -20.0f, 1000.0f, 0.2f, 0.2f, 0.0f, 0.0f),
        MakeFrame(1.0f, 0.5f, -20.0f, 1000.0f, 0.2f, 0.2f, 0.0f, 0.0f),
        MakeFrame(2.0f, 0.5f, -20.0f, 1000.0f, 0.2f, 0.2f, 0.0f, 0.0f)
    };
    frames[0].rms = -0.5f;
    frames[1].rms = infinity;
    frames[2].rms = 1.5f;

    const Analysis::TrackAnalysis track =
        Analysis::AnalyzeTrack(2.0f, std::move(frames));
    ASSERT_EQ(track.frames.size(), 3u);
    EXPECT_FLOAT_EQ(track.frames[0].rms, 0.0f);
    EXPECT_FLOAT_EQ(track.frames[1].rms, 0.0f);
    EXPECT_FLOAT_EQ(track.frames[2].rms, 1.0f);
}

TEST(MusicAnalysisTests, CandidateProfilesAreDirectionalAndIgnoreSilence)
{
    const std::vector<Analysis::Frame> entryFrames = {
        MakeFrame(0.0f, 0.0f, -80.0f, 90000.0f, 0.0f, 0.0f, 1.0f, 0.0f),
        MakeFrame(1.0f, 1.0f, -3.0f, 10000.0f, 1.0f, 1.0f, 0.0f, 1.0f),
        MakeFrame(2.0f, 0.45f, -18.0f, 2000.0f, 0.0f, 0.0f, 0.0f, 0.0f),
        MakeFrame(3.0f, 0.0f, -80.0f, 90000.0f, 0.0f, 0.0f, 1.0f, 0.0f),
        MakeFrame(4.0f, 1.0f, -3.0f, 10000.0f, 1.0f, 1.0f, 0.0f, 1.0f)
    };
    const std::vector<Analysis::Frame> exitFrames = {
        MakeFrame(0.0f, 0.0f, -80.0f, 90000.0f, 0.0f, 0.0f, 1.0f, 0.0f),
        MakeFrame(1.0f, 0.6f, -12.0f, 6000.0f, 0.2f, 0.2f, 0.0f, -0.2f),
        MakeFrame(2.0f, 0.2f, -30.0f, 1000.0f, 0.05f, 0.0f, 0.0f, -1.0f),
        MakeFrame(3.0f, 0.0f, -80.0f, 90000.0f, 0.0f, 0.0f, 1.0f, 0.0f),
        MakeFrame(4.0f, 1.0f, -3.0f, 10000.0f, 1.0f, 1.0f, 0.0f, 1.0f)
    };

    Analysis::CandidateGenerationOptions options;
    options.maxCandidatesPerKind = 1u;
    options.profileRadiusFrames = 2u;
    const std::vector<Analysis::Candidate> entries =
        Analysis::GenerateCandidates(
            entryFrames, Analysis::CandidateKind::Entry, options);
    const std::vector<Analysis::Candidate> exits =
        Analysis::GenerateCandidates(
            exitFrames, Analysis::CandidateKind::Exit, options);

    ASSERT_EQ(entries.size(), 1u);
    ASSERT_EQ(exits.size(), 1u);
    ASSERT_EQ(entries.front().frameIndex, 2u);
    ASSERT_EQ(exits.front().frameIndex, 2u);

    // Entry sees only [2, 4], exit only [0, 2]. Silent frames in either
    // directional range do not dilute the acoustic signature.
    EXPECT_NEAR(entries.front().profile.energy, 0.725f, 0.0001f);
    EXPECT_NEAR(entries.front().profile.loudnessLufs, -10.5f, 0.0001f);
    EXPECT_NEAR(entries.front().profile.spectralCentroidHz, 6000.0f, 0.001f);
    EXPECT_NEAR(entries.front().profile.spectralFlux, 0.5f, 0.0001f);
    EXPECT_NEAR(entries.front().profile.onsetStrength, 0.5f, 0.0001f);

    EXPECT_NEAR(exits.front().profile.energy, 0.4f, 0.0001f);
    EXPECT_NEAR(exits.front().profile.loudnessLufs, -21.0f, 0.0001f);
    EXPECT_NEAR(exits.front().profile.spectralCentroidHz, 3500.0f, 0.001f);
    EXPECT_NEAR(exits.front().profile.spectralFlux, 0.125f, 0.0001f);
    EXPECT_NEAR(exits.front().profile.onsetStrength, 0.1f, 0.0001f);

    const Analysis::Profile restoredExit = Analysis::BuildBoundaryProfile(
        exitFrames, 2u, Analysis::CandidateKind::Exit, 2u);
    EXPECT_NEAR(restoredExit.energy, exits.front().profile.energy, 0.0001f);
    EXPECT_NEAR(
        restoredExit.spectralCentroidHz,
        exits.front().profile.spectralCentroidHz,
        0.001f);
}

TEST(MusicAnalysisTests, ShortTrackIsAlwaysPlayedInFull)
{
    Analysis::TrackAnalysis track;
    track.durationSeconds = 30.0f;
    track.frames = {
        MakeFrame(0.0f, 0.2f, -25.0f, 1200.0f, 0.1f, 0.2f, 0.0f, 0.0f),
        MakeFrame(30.0f, 0.0f, -80.0f, 100.0f, 0.0f, 0.0f, 1.0f, -1.0f)
    };

    Analysis::SegmentSelectionOptions options;
    options.minDurationSeconds = 45.0f;
    options.targetDurationSeconds = 150.0f;
    options.maxDurationSeconds = 240.0f;
    std::mt19937 random(1234);
    std::mt19937 untouched(1234);

    const Analysis::SegmentDecision decision =
        Analysis::ChooseSegment(track, options, random);

    EXPECT_TRUE(decision.fullTrack);
    EXPECT_FLOAT_EQ(decision.startSeconds, 0.0f);
    EXPECT_FLOAT_EQ(decision.endSeconds, 30.0f);
    EXPECT_FLOAT_EQ(decision.durationSeconds, 30.0f);
    EXPECT_TRUE(HasReason(
        decision,
        Analysis::SegmentReason::FullTrackShorterThanMinimum));
    EXPECT_EQ(random(), untouched());
}

TEST(MusicAnalysisTests, GaussianDurationScoreFavoursTheTarget)
{
    Analysis::TrackAnalysis track;
    track.durationSeconds = 300.0f;
    track.entryCandidates = {
        MakeCandidate(Analysis::CandidateKind::Entry, 0.0f, 0.9f)
    };
    track.exitCandidates = {
        MakeCandidate(Analysis::CandidateKind::Exit, 60.0f, 0.9f),
        MakeCandidate(Analysis::CandidateKind::Exit, 150.0f, 0.9f),
        MakeCandidate(Analysis::CandidateKind::Exit, 240.0f, 0.9f)
    };

    Analysis::SegmentSelectionOptions options;
    options.minDurationSeconds = 45.0f;
    options.targetDurationSeconds = 150.0f;
    options.maxDurationSeconds = 240.0f;
    options.topCandidateCount = 1;
    options.temperature = 1.0f;
    std::mt19937 random(7);

    const Analysis::SegmentDecision decision =
        Analysis::ChooseSegment(track, options, random);

    EXPECT_FLOAT_EQ(decision.startSeconds, 0.0f);
    EXPECT_FLOAT_EQ(decision.endSeconds, 150.0f);
    EXPECT_FLOAT_EQ(decision.durationSeconds, 150.0f);
    EXPECT_TRUE(HasReason(
        decision, Analysis::SegmentReason::DurationNearTarget));
    EXPECT_TRUE(HasReason(decision, Analysis::SegmentReason::StrongEntry));
    EXPECT_TRUE(HasReason(decision, Analysis::SegmentReason::NaturalExit));
}

TEST(MusicAnalysisTests, WeightedSelectionIsDeterministicForASeed)
{
    Analysis::TrackAnalysis track;
    track.durationSeconds = 300.0f;
    track.entryCandidates = {
        MakeCandidate(Analysis::CandidateKind::Entry, 0.0f, 0.95f),
        MakeCandidate(Analysis::CandidateKind::Entry, 20.0f, 0.85f)
    };
    track.exitCandidates = {
        MakeCandidate(Analysis::CandidateKind::Exit, 100.0f, 0.8f),
        MakeCandidate(Analysis::CandidateKind::Exit, 150.0f, 0.9f),
        MakeCandidate(Analysis::CandidateKind::Exit, 220.0f, 0.85f)
    };

    Analysis::SegmentSelectionOptions options;
    options.minDurationSeconds = 45.0f;
    options.targetDurationSeconds = 140.0f;
    options.maxDurationSeconds = 240.0f;
    options.temperature = 0.5f;
    std::mt19937 firstRandom(2026);
    std::mt19937 secondRandom(2026);

    const Analysis::SegmentDecision first =
        Analysis::ChooseSegment(track, options, firstRandom);
    const Analysis::SegmentDecision second =
        Analysis::ChooseSegment(track, options, secondRandom);

    EXPECT_FLOAT_EQ(first.startSeconds, second.startSeconds);
    EXPECT_FLOAT_EQ(first.endSeconds, second.endSeconds);
    EXPECT_FLOAT_EQ(first.score, second.score);
    EXPECT_TRUE(HasReason(
        first, Analysis::SegmentReason::WeightedTopCandidate));
}

TEST(MusicAnalysisTests, SelectionCapsCandidatesAndRemainsDeterministic)
{
    Analysis::TrackAnalysis track;
    track.durationSeconds = 400.0f;
    track.entryCandidates.reserve(513u);
    for (std::size_t index = 0u; index < 512u; ++index)
    {
        track.entryCandidates.push_back(MakeCandidate(
            Analysis::CandidateKind::Entry, 0.0f, 0.5f));
    }
    // This lower-ranked external candidate would win on its valid 150-second
    // window if selection were allowed to exceed the analysis budget.
    track.entryCandidates.push_back(MakeCandidate(
        Analysis::CandidateKind::Entry, 100.0f, 0.4f));
    track.exitCandidates = {
        MakeCandidate(Analysis::CandidateKind::Exit, 250.0f, 1.0f),
        MakeCandidate(Analysis::CandidateKind::Exit, 150.0f, 0.1f)
    };

    Analysis::SegmentSelectionOptions options;
    options.minDurationSeconds = 100.0f;
    options.targetDurationSeconds = 150.0f;
    options.maxDurationSeconds = 180.0f;
    options.topCandidateCount = 1u;
    options.maxCandidatesPerKind = 512u;
    std::mt19937 firstRandom(91);
    std::mt19937 secondRandom(91);

    const Analysis::SegmentDecision first =
        Analysis::ChooseSegment(track, options, firstRandom);
    const Analysis::SegmentDecision second =
        Analysis::ChooseSegment(track, options, secondRandom);

    EXPECT_FLOAT_EQ(first.startSeconds, 0.0f);
    EXPECT_FLOAT_EQ(first.endSeconds, 150.0f);
    EXPECT_FLOAT_EQ(first.startSeconds, second.startSeconds);
    EXPECT_FLOAT_EQ(first.endSeconds, second.endSeconds);
    EXPECT_FLOAT_EQ(first.score, second.score);
    EXPECT_FALSE(HasReason(
        first, Analysis::SegmentReason::NoCandidatePairFallback));
}

TEST(MusicAnalysisTests, RuntimeBudgetPreservesTimelineCoverage)
{
    Analysis::TrackAnalysis track;
    track.durationSeconds = 400.0f;
    track.entryCandidates.reserve(512u);
    for (std::size_t index = 0u; index < 511u; ++index)
    {
        track.entryCandidates.push_back(MakeCandidate(
            Analysis::CandidateKind::Entry,
            0.0f,
            index < 64u ? 0.95f : 0.50f));
    }
    // The only valid window is deliberately low-ranked at the far end of the
    // quality-ordered vector. The coverage half of the default 128 budget
    // must retain it without expanding the real-time work bound.
    track.entryCandidates.push_back(MakeCandidate(
        Analysis::CandidateKind::Entry, 100.0f, 0.40f));
    track.exitCandidates = {
        MakeCandidate(Analysis::CandidateKind::Exit, 250.0f, 1.0f)
    };

    Analysis::SegmentSelectionOptions options;
    options.minDurationSeconds = 100.0f;
    options.targetDurationSeconds = 150.0f;
    options.maxDurationSeconds = 180.0f;
    options.topCandidateCount = 1u;
    ASSERT_EQ(options.maxCandidatesPerKind, 128u);
    std::mt19937 random(92);

    const Analysis::SegmentDecision decision =
        Analysis::ChooseSegment(track, options, random);

    EXPECT_FLOAT_EQ(decision.startSeconds, 100.0f);
    EXPECT_FLOAT_EQ(decision.endSeconds, 250.0f);
    EXPECT_FALSE(HasReason(
        decision, Analysis::SegmentReason::NoCandidatePairFallback));
}

TEST(MusicAnalysisTests, CompatibilityScoresLoudnessEnergyTextureAndOnset)
{
    const Analysis::Profile outgoing = {
        0.40f, -18.0f, 2000.0f, 0.25f, 0.05f
    };
    const Analysis::Profile compatibleIncoming = {
        0.42f, -17.0f, 2100.0f, 0.28f, 0.10f
    };
    const Analysis::Profile incompatibleIncoming = {
        0.95f, -3.0f, 12000.0f, 1.0f, 1.0f
    };

    const Analysis::CompatibilityScore compatible =
        Analysis::ScoreCompatibility(outgoing, compatibleIncoming);
    const Analysis::CompatibilityScore incompatible =
        Analysis::ScoreCompatibility(outgoing, incompatibleIncoming);

    EXPECT_GT(compatible.total, incompatible.total);
    EXPECT_GT(compatible.loudness, incompatible.loudness);
    EXPECT_GT(compatible.energy, incompatible.energy);
    EXPECT_GT(compatible.texture, incompatible.texture);
    EXPECT_GT(compatible.onset, incompatible.onset);
    EXPECT_GE(compatible.total, 0.0f);
    EXPECT_LE(compatible.total, 1.0f);
}

TEST(MusicAnalysisTests, SegmentSelectionCanPreferACompatibleEntry)
{
    const Analysis::Profile outgoing = {
        0.40f, -18.0f, 2000.0f, 0.25f, 0.05f
    };
    const Analysis::Profile incompatibleEntry = {
        0.95f, -3.0f, 12000.0f, 1.0f, 0.0f
    };
    const Analysis::Profile compatibleEntry = {
        0.42f, -17.0f, 2100.0f, 0.28f, 0.10f
    };

    Analysis::TrackAnalysis track;
    track.durationSeconds = 200.0f;
    track.entryCandidates = {
        MakeCandidate(
            Analysis::CandidateKind::Entry, 0.0f, 0.75f,
            incompatibleEntry),
        MakeCandidate(
            Analysis::CandidateKind::Entry, 10.0f, 0.75f,
            compatibleEntry)
    };
    track.exitCandidates = {
        MakeCandidate(Analysis::CandidateKind::Exit, 100.0f, 0.8f),
        MakeCandidate(Analysis::CandidateKind::Exit, 110.0f, 0.8f)
    };

    Analysis::SegmentSelectionOptions options;
    options.minDurationSeconds = 45.0f;
    options.targetDurationSeconds = 100.0f;
    options.maxDurationSeconds = 150.0f;
    options.topCandidateCount = 1;
    std::mt19937 random(1);

    const Analysis::SegmentDecision decision =
        Analysis::ChooseSegment(track, options, random, outgoing);

    EXPECT_FLOAT_EQ(decision.startSeconds, 10.0f);
    EXPECT_FLOAT_EQ(decision.durationSeconds, 100.0f);
    EXPECT_TRUE(HasReason(
        decision, Analysis::SegmentReason::CompatibleWithPrevious));
}

TEST(MusicAnalysisTests, SegmentSelectionPrefersAnUnderexploredGoodWindow)
{
    Analysis::TrackAnalysis track;
    track.durationSeconds = 260.0f;
    track.entryCandidates = {
        MakeCandidate(Analysis::CandidateKind::Entry, 0.0f, 0.85f),
        MakeCandidate(Analysis::CandidateKind::Entry, 100.0f, 0.85f)
    };
    track.exitCandidates = {
        MakeCandidate(Analysis::CandidateKind::Exit, 150.0f, 0.85f),
        MakeCandidate(Analysis::CandidateKind::Exit, 250.0f, 0.85f)
    };

    ListeningHeatmap::State fatigue = ListeningHeatmap::CreateState(260.0);
    ASSERT_TRUE(ListeningHeatmap::RecordCoverage(
        fatigue, 0.0, 100.0, 1.0, 1000));

    Analysis::SegmentSelectionOptions options;
    options.minDurationSeconds = 140.0f;
    options.targetDurationSeconds = 150.0f;
    options.maxDurationSeconds = 160.0f;
    options.topCandidateCount = 1;
    options.listeningFatigue = &fatigue;
    options.selectionEpochSeconds = 1000;
    std::mt19937 random(9);

    const Analysis::SegmentDecision decision =
        Analysis::ChooseSegment(track, options, random);

    EXPECT_FLOAT_EQ(decision.startSeconds, 100.0f);
    EXPECT_FLOAT_EQ(decision.endSeconds, 250.0f);
    EXPECT_GT(decision.explorationScore, 0.60f);
    EXPECT_TRUE(HasReason(
        decision, Analysis::SegmentReason::UnderexploredRegion));
}

TEST(MusicAnalysisTests, MissingCandidatePairUsesBoundedFallback)
{
    Analysis::TrackAnalysis track;
    track.durationSeconds = 100.0f;

    Analysis::SegmentSelectionOptions options;
    options.minDurationSeconds = 45.0f;
    options.targetDurationSeconds = 60.0f;
    options.maxDurationSeconds = 80.0f;
    std::mt19937 random(42);

    const Analysis::SegmentDecision decision =
        Analysis::ChooseSegment(track, options, random);

    EXPECT_FLOAT_EQ(decision.startSeconds, 0.0f);
    EXPECT_FLOAT_EQ(decision.endSeconds, 60.0f);
    EXPECT_GE(decision.durationSeconds, options.minDurationSeconds);
    EXPECT_LE(decision.durationSeconds, options.maxDurationSeconds);
    EXPECT_TRUE(HasReason(
        decision, Analysis::SegmentReason::NoCandidatePairFallback));
    EXPECT_STREQ(
        Analysis::ToString(Analysis::SegmentReason::NoCandidatePairFallback),
        "no_candidate_pair_fallback");
}

} // namespace TSM::Tests
