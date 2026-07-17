#include "pch.h"

#include <cstdint>
#include <limits>
#include <utility>

namespace TSM::Tests
{
namespace
{

void WriteLittleEndian16(std::ofstream& output, std::uint16_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu)
    };
    output.write(bytes, sizeof(bytes));
}

void WriteLittleEndian32(std::ofstream& output, std::uint32_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
        static_cast<char>((value >> 16u) & 0xffu),
        static_cast<char>((value >> 24u) & 0xffu)
    };
    output.write(bytes, sizeof(bytes));
}

void WriteSilentWave(
    const std::filesystem::path& path,
    float durationSeconds = 10.0f)
{
    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bitsPerSample = 16;
    ASSERT_TRUE(std::isfinite(durationSeconds));
    ASSERT_GT(durationSeconds, 0.0f);
    const std::uint32_t sampleCount = static_cast<std::uint32_t>(std::max(
        std::llround(static_cast<double>(sampleRate) * durationSeconds),
        1ll));
    const std::uint32_t dataSize =
        sampleCount * channels * (bitsPerSample / 8u);

    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output.write("RIFF", 4);
    WriteLittleEndian32(output, 36u + dataSize);
    output.write("WAVEfmt ", 8);
    WriteLittleEndian32(output, 16u);
    WriteLittleEndian16(output, 1u);
    WriteLittleEndian16(output, channels);
    WriteLittleEndian32(output, sampleRate);
    WriteLittleEndian32(
        output, sampleRate * channels * (bitsPerSample / 8u));
    WriteLittleEndian16(output, channels * (bitsPerSample / 8u));
    WriteLittleEndian16(output, bitsPerSample);
    output.write("data", 4);
    WriteLittleEndian32(output, dataSize);
    const std::vector<char> silence(dataSize, 0);
    output.write(silence.data(), static_cast<std::streamsize>(silence.size()));
}

bool IsStopped(FMOD::Channel* channel)
{
    if (!channel) return true;
    bool isPlaying = true;
    const FMOD_RESULT result = channel->isPlaying(&isPlaying);
    return result != FMOD_OK || !isPlaying;
}

MusicAnalysis::TrackAnalysis MakeDeterministicSegmentAnalysis(
    float entrySeconds, float exitSeconds,
    float entryScore = 0.90f, float exitScore = 0.85f)
{
    MusicAnalysis::Profile profile;
    profile.energy = 0.45f;
    profile.loudnessLufs = -16.0f;
    profile.spectralCentroidHz = 1200.0f;
    profile.spectralFlux = 0.20f;
    profile.onsetStrength = 0.55f;

    MusicAnalysis::Candidate entry;
    entry.kind = MusicAnalysis::CandidateKind::Entry;
    entry.timeSeconds = entrySeconds;
    entry.score = entryScore;
    entry.profile = profile;

    MusicAnalysis::Candidate exit;
    exit.kind = MusicAnalysis::CandidateKind::Exit;
    exit.timeSeconds = exitSeconds;
    exit.score = exitScore;
    exit.profile = profile;

    MusicAnalysis::TrackAnalysis analysis;
    analysis.durationSeconds = 10.0f;
    analysis.entryCandidates = {entry};
    analysis.exitCandidates = {exit};
    return analysis;
}

void InjectMusicAnalysis(
    const std::string& soundId, MusicAnalysis::TrackAnalysis analysis)
{
    const auto& sounds = AudioManager::GetInstance().GetAllSounds();
    const auto sound = sounds.find(soundId);
    ASSERT_NE(sound, sounds.end());

    // SoundData deliberately exposes diagnostics through a const view. Tests
    // inject an already-computed result here so the integration path remains
    // deterministic without adding a production-only testing API.
    auto& data = const_cast<AudioManager::SoundData&>(sound->second);
    data.isMusic = true;
    data.loudnessStatus = AudioManager::LoudnessStatus::Ready;
    data.musicAnalysis = std::move(analysis);
    data.musicAnalysisReady = true;
}

void SetNormalizationGain(const std::string& soundId, float gain)
{
    ASSERT_TRUE(std::isfinite(gain));
    ASSERT_GT(gain, 0.0f);
    const auto& sounds = AudioManager::GetInstance().GetAllSounds();
    const auto sound = sounds.find(soundId);
    ASSERT_NE(sound, sounds.end());

    auto& data = const_cast<AudioManager::SoundData&>(sound->second);
    data.isMusic = true;
    data.normalizationGainLinear = gain;
}

bool ContainsReason(
    const SegmentDecisionInfo& decision, const std::string& reason)
{
    return std::find(
        decision.reasons.begin(), decision.reasons.end(), reason) !=
        decision.reasons.end();
}

class PlaylistResumeTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        RuntimeOptions options;
        options.loadConfig = false;
        options.noSound = true;
        ASSERT_TRUE(runtime.Initialize(options, errorMessage)) << errorMessage;
    }

    void TearDown() override
    {
        runtime.Shutdown();
        for (const std::filesystem::path& path : temporaryPaths)
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    }

    void LoadMusic(const std::string& id, float durationSeconds = 10.0f)
    {
        const auto stamp = std::chrono::high_resolution_clock::now()
            .time_since_epoch().count();
        const std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("tsm_resume_" + id + "_" + std::to_string(stamp) + ".wav");
        temporaryPaths.push_back(path);
        WriteSilentWave(path, durationSeconds);
        ASSERT_TRUE(AudioManager::GetInstance().LoadSound(
            id, path.string(), true, AudioManager::SoundKind::Music));
    }

    void CreatePlaylistWithTracks(
        const std::string& playlistName, const std::vector<std::string>& tracks)
    {
        auto& manager = PlaylistManager::GetInstance();
        manager.CreatePlaylist(playlistName);
        for (const std::string& track : tracks)
        {
            LoadMusic(track);
            manager.AddToPlaylist(playlistName, track);
        }
    }

    ApplicationRuntime runtime;
    std::string errorMessage;
    std::vector<std::filesystem::path> temporaryPaths;
};

TEST_F(PlaylistResumeTests, CapturesAndResumesCompleteRandomSegmentState)
{
    constexpr const char* playlistName = "resume_random_playlist";
    CreatePlaylistWithTracks(playlistName, {"resume_track_a", "resume_track_b"});

    auto& manager = PlaylistManager::GetInstance();
    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    playlist->crossfadeDuration = 0.1f;

    PlaylistOptions options;
    options.randomOrder = true;
    options.randomSegment = true;
    options.segmentDuration = 2.0f;
    options.automaticSegmentDuration = true;
    options.minSegmentDuration = 2.0f;
    options.maxSegmentDuration = 4.0f;
    options.loopPlaylist = true;
    manager.Play(playlistName, options);
    manager.Update(0.125f);

    const PlaybackState captured = manager.CapturePlaybackState();
    ASSERT_TRUE(captured.isPlaying);
    ASSERT_EQ(captured.playlistName, playlistName);
    ASSERT_FALSE(captured.trackId.empty());
    ASSERT_GE(captured.trackIndex, 0);
    ASSERT_TRUE(captured.segmentActive);
    EXPECT_EQ(captured.segmentElapsedMs, 125u);
    EXPECT_GE(captured.segmentDurationMs, 2000u);
    EXPECT_LE(captured.segmentDurationMs, 4000u);
    ASSERT_EQ(captured.randomPermutation.size(), 2u);
    ASSERT_GE(captured.randomPermutationIndex, 0);
    EXPECT_EQ(
        captured.randomPermutation[captured.randomPermutationIndex], captured.trackIndex);

    manager.AbortImmediately();
    ASSERT_FALSE(manager.CapturePlaybackState().isPlaying);

    std::string resumeError;
    ASSERT_TRUE(manager.ResumePlaybackState(captured, resumeError)) << resumeError;
    const PlaybackState resumed = manager.CapturePlaybackState();
    ASSERT_TRUE(resumed.isPlaying);
    EXPECT_EQ(resumed.playlistName, captured.playlistName);
    EXPECT_EQ(resumed.trackId, captured.trackId);
    EXPECT_EQ(resumed.trackIndex, captured.trackIndex);
    EXPECT_EQ(resumed.options.randomOrder, captured.options.randomOrder);
    EXPECT_EQ(resumed.options.randomSegment, captured.options.randomSegment);
    EXPECT_EQ(
        resumed.options.automaticSegmentDuration,
        captured.options.automaticSegmentDuration);
    EXPECT_EQ(resumed.options.loopPlaylist, captured.options.loopPlaylist);
    EXPECT_FLOAT_EQ(resumed.options.segmentDuration, captured.options.segmentDuration);
    EXPECT_FLOAT_EQ(
        resumed.options.minSegmentDuration,
        captured.options.minSegmentDuration);
    EXPECT_FLOAT_EQ(
        resumed.options.maxSegmentDuration,
        captured.options.maxSegmentDuration);
    EXPECT_FLOAT_EQ(resumed.crossfadeDuration, captured.crossfadeDuration);
    EXPECT_EQ(resumed.randomPermutation, captured.randomPermutation);
    EXPECT_EQ(resumed.randomPermutationIndex, captured.randomPermutationIndex);
    EXPECT_EQ(resumed.segmentStartMs, captured.segmentStartMs);
    EXPECT_EQ(resumed.segmentElapsedMs, captured.segmentElapsedMs);
    EXPECT_EQ(resumed.segmentDurationMs, captured.segmentDurationMs);
    EXPECT_FLOAT_EQ(
        manager.GetSegmentDuration(),
        static_cast<float>(captured.segmentDurationMs) / 1000.0f);
    EXPECT_EQ(
        manager.GetSegmentDecisionInfo().mode,
        "restored_checkpoint");
    EXPECT_NEAR(
        static_cast<double>(resumed.positionMs),
        static_cast<double>(captured.positionMs), 50.0);
}

TEST_F(PlaylistResumeTests, AutomaticSegmentUsesTheFullTrackWhenRangeExceedsItsDuration)
{
    constexpr const char* playlistName = "short_track_segment_playlist";
    constexpr const char* trackId = "ten_second_track";
    CreatePlaylistWithTracks(playlistName, {trackId});
    InjectMusicAnalysis(
        trackId, MakeDeterministicSegmentAnalysis(1.0f, 5.0f));

    auto& manager = PlaylistManager::GetInstance();
    PlaylistOptions options;
    options.randomSegment = true;
    options.automaticSegmentDuration = true;
    options.minSegmentDuration = 45.0f;
    options.maxSegmentDuration = 240.0f;
    manager.Play(playlistName, options);

    const PlaybackState captured = manager.CapturePlaybackState();
    ASSERT_TRUE(captured.isPlaying);
    ASSERT_TRUE(captured.segmentActive);
    EXPECT_EQ(captured.segmentStartMs, 0u);
    EXPECT_EQ(captured.segmentDurationMs, 10000u);
    EXPECT_FLOAT_EQ(manager.GetSegmentDuration(), 10.0f);

    const SegmentDecisionInfo decision = manager.GetSegmentDecisionInfo();
    EXPECT_TRUE(decision.smartAnalysis);
    EXPECT_TRUE(decision.fullTrack);
    EXPECT_EQ(decision.mode, "smart");
    EXPECT_FLOAT_EQ(decision.startSeconds, 0.0f);
    EXPECT_FLOAT_EQ(decision.endSeconds, 10.0f);
}

TEST_F(PlaylistResumeTests, RejectsInvalidIdentityAndIndicesWithoutMutation)
{
    constexpr const char* playlistName = "resume_validation_playlist";
    CreatePlaylistWithTracks(playlistName, {"validation_track_a", "validation_track_b"});

    auto& manager = PlaylistManager::GetInstance();
    PlaylistOptions options;
    options.randomOrder = false;
    options.randomSegment = false;
    options.segmentDuration = 2.0f;
    manager.Play(playlistName, options);

    const PlaybackState before = manager.CapturePlaybackState();
    FMOD::Channel* const beforeChannel = manager.GetCurrentChannel();
    ASSERT_TRUE(before.isPlaying);
    ASSERT_NE(beforeChannel, nullptr);

    const auto expectRejectedWithoutMutation = [&](PlaybackState invalid) {
        std::string error;
        EXPECT_FALSE(manager.ResumePlaybackState(invalid, error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(manager.GetCurrentChannel(), beforeChannel);
        EXPECT_EQ(manager.GetCurrentTrackName(), before.trackId);
        EXPECT_TRUE(manager.IsPlaylistPlaying(playlistName));
    };

    PlaybackState invalidPlaylist = before;
    invalidPlaylist.playlistName = "missing_playlist";
    expectRejectedWithoutMutation(invalidPlaylist);

    PlaybackState invalidTrack = before;
    invalidTrack.trackId = "wrong_track";
    expectRejectedWithoutMutation(invalidTrack);

    PlaybackState invalidIndex = before;
    invalidIndex.trackIndex = 200;
    expectRejectedWithoutMutation(invalidIndex);

    PlaybackState mismatchedIndex = before;
    mismatchedIndex.trackIndex = 1;
    expectRejectedWithoutMutation(mismatchedIndex);
}

TEST_F(PlaylistResumeTests, ClampsSavedPositionToTheTrackDuration)
{
    constexpr const char* playlistName = "resume_clamp_playlist";
    CreatePlaylistWithTracks(playlistName, {"clamp_track"});

    auto& manager = PlaylistManager::GetInstance();
    PlaylistOptions options;
    options.segmentDuration = 2.0f;
    manager.Play(playlistName, options);
    PlaybackState state = manager.CapturePlaybackState();
    ASSERT_TRUE(state.isPlaying);
    state.positionMs = (std::numeric_limits<std::uint32_t>::max)();

    std::string error;
    ASSERT_TRUE(manager.ResumePlaybackState(state, error)) << error;
    const PlaybackState resumed = manager.CapturePlaybackState();
    ASSERT_TRUE(resumed.isPlaying);
    EXPECT_LT(resumed.positionMs, 10000u);
    EXPECT_GE(resumed.positionMs, 9900u);
}

TEST_F(PlaylistResumeTests, AbortImmediatelyHardStopsBothCrossfadeChannels)
{
    constexpr const char* playlistName = "emergency_abort_playlist";
    CreatePlaylistWithTracks(playlistName, {"abort_track_a", "abort_track_b"});

    auto& manager = PlaylistManager::GetInstance();
    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    playlist->crossfadeDuration = 20.0f;

    PlaylistOptions options;
    options.loopPlaylist = true;
    options.segmentDuration = 2.0f;
    manager.Play(playlistName, options);
    manager.Update(0.0f);
    ASSERT_TRUE(manager.IsInCrossfade());
    FMOD::Channel* const current = manager.GetCurrentChannel();
    FMOD::Channel* const next = manager.GetNextChannel();
    ASSERT_NE(current, nullptr);
    ASSERT_NE(next, nullptr);

    manager.AbortImmediately();

    EXPECT_TRUE(IsStopped(current));
    EXPECT_TRUE(IsStopped(next));
    EXPECT_EQ(manager.GetCurrentChannel(), nullptr);
    EXPECT_EQ(manager.GetNextChannel(), nullptr);
    EXPECT_FALSE(manager.IsPlaylistPlaying(playlistName));
    EXPECT_FALSE(manager.CapturePlaybackState().isPlaying);
}

TEST_F(PlaylistResumeTests, StalePlaylistHandleCannotStopAnotherSound)
{
    constexpr const char* playlistName = "stale_handle_playlist";
    CreatePlaylistWithTracks(playlistName, {"stale_expected_track"});
    LoadMusic("stale_live_track");

    auto& manager = PlaylistManager::GetInstance();
    PlaylistOptions options;
    options.segmentDuration = 2.0f;
    manager.Play(playlistName, options);
    FMOD::Channel* const original = manager.GetCurrentChannel();
    ASSERT_NE(original, nullptr);
    ASSERT_EQ(original->stop(), FMOD_OK);

    auto& audio = AudioManager::GetInstance();
    FMOD::Channel* const live = audio.PlayMusic("stale_live_track");
    ASSERT_NE(live, nullptr);
    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    playlist->currentChannel = live;
    playlist->expectedCurrentSound = audio.GetSound("stale_expected_track");

    manager.AbortImmediately();

    bool isPlaying = false;
    ASSERT_EQ(live->isPlaying(&isPlaying), FMOD_OK);
    EXPECT_TRUE(isPlaying);
    FMOD::Sound* actualSound = nullptr;
    ASSERT_EQ(live->getCurrentSound(&actualSound), FMOD_OK);
    EXPECT_EQ(actualSound, audio.GetSound("stale_live_track"));
}

TEST_F(PlaylistResumeTests, SmartSegmentDecisionIsExposedAndCaptured)
{
    constexpr const char* playlistName = "smart_segment_decision_playlist";
    constexpr const char* trackId = "smart_segment_track";
    CreatePlaylistWithTracks(playlistName, {trackId});
    InjectMusicAnalysis(
        trackId, MakeDeterministicSegmentAnalysis(1.0f, 5.0f));

    PlaylistOptions options;
    options.randomSegment = true;
    options.automaticSegmentDuration = true;
    options.segmentDuration = 4.0f;
    options.minSegmentDuration = 3.0f;
    options.maxSegmentDuration = 6.0f;
    PlaylistManager::GetInstance().Play(playlistName, options);

    const SegmentDecisionInfo decision =
        PlaylistManager::GetInstance().GetSegmentDecisionInfo();
    ASSERT_TRUE(decision.active);
    EXPECT_TRUE(decision.smartAnalysis);
    EXPECT_FALSE(decision.fullTrack);
    EXPECT_EQ(decision.mode, "smart");
    EXPECT_FLOAT_EQ(decision.startSeconds, 1.0f);
    EXPECT_FLOAT_EQ(decision.endSeconds, 5.0f);
    EXPECT_FLOAT_EQ(decision.durationSeconds, 4.0f);
    EXPECT_FLOAT_EQ(decision.entryScore, 0.90f);
    EXPECT_FLOAT_EQ(decision.exitScore, 0.85f);
    EXPECT_GT(decision.totalScore, 0.80f);
    EXPECT_TRUE(ContainsReason(decision, "weighted_top_candidate"));
    EXPECT_TRUE(ContainsReason(decision, "duration_near_target"));
    EXPECT_TRUE(ContainsReason(decision, "strong_entry"));
    EXPECT_TRUE(ContainsReason(decision, "natural_exit"));

    const PlaybackState state =
        PlaylistManager::GetInstance().CapturePlaybackState();
    ASSERT_TRUE(state.isPlaying);
    ASSERT_TRUE(state.segmentActive);
    EXPECT_EQ(state.trackId, trackId);
    EXPECT_EQ(state.segmentStartMs, 1000u);
    EXPECT_EQ(state.segmentDurationMs, 4000u);
    EXPECT_EQ(state.segmentElapsedMs, 0u);
}

TEST_F(PlaylistResumeTests, AutomaticSegmentFallsBackWithinConfiguredBounds)
{
    constexpr const char* playlistName = "bounded_fallback_playlist";
    constexpr const char* trackId = "bounded_fallback_track";
    CreatePlaylistWithTracks(playlistName, {trackId});

    PlaylistOptions options;
    options.randomSegment = true;
    options.automaticSegmentDuration = true;
    options.segmentDuration = 4.0f;
    options.minSegmentDuration = 3.0f;
    options.maxSegmentDuration = 6.0f;
    PlaylistManager::GetInstance().Play(playlistName, options);

    const SegmentDecisionInfo decision =
        PlaylistManager::GetInstance().GetSegmentDecisionInfo();
    ASSERT_TRUE(decision.active);
    EXPECT_FALSE(decision.smartAnalysis);
    EXPECT_FALSE(decision.fullTrack);
    EXPECT_EQ(decision.mode, "bounded_random_fallback");
    EXPECT_GE(decision.durationSeconds, 3.0f);
    EXPECT_LE(decision.durationSeconds, 6.0f);
    EXPECT_GE(decision.startSeconds, 0.0f);
    EXPECT_LE(decision.endSeconds, 10.0f);
    EXPECT_NEAR(
        decision.endSeconds - decision.startSeconds,
        decision.durationSeconds, 0.001f);
    EXPECT_TRUE(ContainsReason(
        decision, "analysis unavailable: bounded random fallback"));

    const PlaybackState state =
        PlaylistManager::GetInstance().CapturePlaybackState();
    ASSERT_TRUE(state.segmentActive);
    EXPECT_NEAR(
        static_cast<float>(state.segmentStartMs) / 1000.0f,
        decision.startSeconds, 0.001f);
    EXPECT_NEAR(
        static_cast<float>(state.segmentDurationMs) / 1000.0f,
        decision.durationSeconds, 0.001f);
}

TEST_F(PlaylistResumeTests, IncomingSegmentElapsedIsCapturedDuringCrossfade)
{
    constexpr const char* playlistName = "crossfade_checkpoint_playlist";
    constexpr const char* firstTrack = "crossfade_checkpoint_first";
    constexpr const char* secondTrack = "crossfade_checkpoint_second";
    CreatePlaylistWithTracks(playlistName, {firstTrack, secondTrack});
    InjectMusicAnalysis(
        firstTrack, MakeDeterministicSegmentAnalysis(1.0f, 5.0f));
    InjectMusicAnalysis(
        secondTrack, MakeDeterministicSegmentAnalysis(2.0f, 6.0f));

    auto& manager = PlaylistManager::GetInstance();
    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    playlist->crossfadeDuration = 10.0f;

    PlaylistOptions options;
    options.randomSegment = true;
    options.automaticSegmentDuration = true;
    options.segmentDuration = 4.0f;
    options.minSegmentDuration = 3.0f;
    options.maxSegmentDuration = 6.0f;
    options.loopPlaylist = true;
    manager.Play(playlistName, options);

    // A four-second segment caps its automatic overlap at one second. Three
    // seconds of elapsed playback therefore enters the anticipated fade.
    manager.Update(3.0f);
    ASSERT_TRUE(manager.IsInCrossfade());
    EXPECT_EQ(manager.GetNextTrackName(), secondTrack);

    // Capture after the equal-power midpoint: the incoming segment is now the
    // least-disruptive checkpoint survivor.
    manager.Update(0.6f);
    ASSERT_TRUE(manager.IsInCrossfade());
    EXPECT_NEAR(manager.GetCrossfadeProgress(), 0.6f, 0.001f);

    const PlaybackState during = manager.CapturePlaybackState();
    ASSERT_TRUE(during.isPlaying);
    ASSERT_TRUE(during.segmentActive);
    EXPECT_EQ(during.trackId, secondTrack);
    EXPECT_EQ(during.segmentStartMs, 2000u);
    EXPECT_EQ(during.segmentDurationMs, 4000u);
    EXPECT_NEAR(
        static_cast<double>(during.segmentElapsedMs), 600.0, 2.0);

    // The public decision still describes the audible outgoing segment until
    // promotion; the durable checkpoint intentionally follows the survivor.
    EXPECT_FLOAT_EQ(manager.GetSegmentDecisionInfo().startSeconds, 1.0f);

    manager.Update(0.41f);
    ASSERT_FALSE(manager.IsInCrossfade());
    EXPECT_EQ(manager.GetCurrentTrackName(), secondTrack);
    EXPECT_FLOAT_EQ(manager.GetSegmentDecisionInfo().startSeconds, 2.0f);

    const PlaybackState promoted = manager.CapturePlaybackState();
    ASSERT_TRUE(promoted.segmentActive);
    EXPECT_EQ(promoted.trackId, secondTrack);
    EXPECT_NEAR(
        static_cast<double>(promoted.segmentElapsedMs), 1010.0, 3.0);
}

TEST_F(PlaylistResumeTests, ManualSkipIsCappedAtTwoSeconds)
{
    constexpr const char* playlistName = "manual_skip_duration_playlist";
    constexpr const char* firstTrack = "manual_skip_first";
    constexpr const char* secondTrack = "manual_skip_second";
    CreatePlaylistWithTracks(playlistName, {firstTrack, secondTrack});

    auto& manager = PlaylistManager::GetInstance();
    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    playlist->crossfadeDuration = 10.0f;

    PlaylistOptions options;
    options.loopPlaylist = true;
    manager.Play(playlistName, options);
    ASSERT_EQ(manager.GetCurrentTrackName(), firstTrack);

    manager.SkipToNextTrack(playlistName);
    ASSERT_TRUE(manager.IsInCrossfade());
    EXPECT_STREQ(manager.GetLastTransitionReason(), "manual skip");

    manager.Update(1.0f);
    ASSERT_TRUE(manager.IsInCrossfade());
    EXPECT_NEAR(manager.GetCrossfadeProgress(), 0.5f, 0.001f);

    manager.Update(1.01f);
    EXPECT_FALSE(manager.IsInCrossfade());
    EXPECT_EQ(manager.GetCurrentTrackName(), secondTrack);
}

TEST_F(
    PlaylistResumeTests,
    CrossfadeEnvelopeTracksCurrentLoudnessGainOnBothChannels)
{
    constexpr const char* playlistName = "dynamic_loudness_crossfade_playlist";
    constexpr const char* outgoingTrack = "dynamic_loudness_outgoing";
    constexpr const char* incomingTrack = "dynamic_loudness_incoming";
    CreatePlaylistWithTracks(
        playlistName, {outgoingTrack, incomingTrack});

    auto& manager = PlaylistManager::GetInstance();
    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    playlist->crossfadeDuration = 10.0f;

    PlaylistOptions options;
    options.loopPlaylist = true;
    manager.Play(playlistName, options);
    // This test measures a steady programme crossfade, not the independent
    // initial fade-in. Put the outgoing channel at its normalized target before
    // changing either track's LUFS gain.
    FMOD::Channel* const initiallyAudible = manager.GetCurrentChannel();
    ASSERT_NE(initiallyAudible, nullptr);
    auto& audio = AudioManager::GetInstance();
    audio.SetChannelVolume(
        initiallyAudible,
        audio.GetNormalizationGainForChannel(initiallyAudible));
    manager.SkipToNextTrack(playlistName);
    ASSERT_TRUE(manager.IsInCrossfade());
    ASSERT_NE(manager.GetCurrentChannel(), nullptr);
    ASSERT_NE(manager.GetNextChannel(), nullptr);

    // Model a completed LUFS analysis arriving after the overlap began. The
    // playlist envelope must use the new gains rather than the values captured
    // when the channels were staged.
    SetNormalizationGain(outgoingTrack, 0.5f);
    SetNormalizationGain(incomingTrack, 2.0f);
    manager.Update(1.0f);

    ASSERT_NEAR(manager.GetCrossfadeProgress(), 0.5f, 0.001f);
    const TransitionLogic::EqualPowerGains gains =
        TransitionLogic::CalculateEqualPowerGains(0.5f);
    float outgoingVolume = 0.0f;
    float incomingVolume = 0.0f;
    ASSERT_EQ(
        manager.GetCurrentChannel()->getVolume(&outgoingVolume), FMOD_OK);
    ASSERT_EQ(
        manager.GetNextChannel()->getVolume(&incomingVolume), FMOD_OK);
    EXPECT_NEAR(outgoingVolume, gains.outgoing * 0.5f, 0.001f);
    EXPECT_NEAR(incomingVolume, gains.incoming * 2.0f, 0.001f);

    manager.Update(1.01f);
    ASSERT_FALSE(manager.IsInCrossfade());
    EXPECT_EQ(manager.GetCurrentTrackName(), incomingTrack);
    float promotedVolume = 0.0f;
    ASSERT_EQ(
        manager.GetCurrentChannel()->getVolume(&promotedVolume), FMOD_OK);
    EXPECT_NEAR(promotedVolume, 2.0f, 0.001f);
}

TEST_F(
    PlaylistResumeTests,
    ShortIncomingTrackCompletesFadeBeforeItsPlaybackBoundary)
{
    constexpr const char* playlistName = "short_incoming_crossfade_playlist";
    constexpr const char* outgoingTrack = "short_incoming_outgoing";
    constexpr const char* incomingTrack = "short_incoming_half_second";

    auto& manager = PlaylistManager::GetInstance();
    manager.CreatePlaylist(playlistName);
    LoadMusic(outgoingTrack);
    LoadMusic(incomingTrack, 0.5f);
    manager.AddToPlaylist(playlistName, outgoingTrack);
    manager.AddToPlaylist(playlistName, incomingTrack);

    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    playlist->crossfadeDuration = 10.0f;

    PlaylistOptions options;
    options.loopPlaylist = true;
    manager.Play(playlistName, options);
    manager.SkipToNextTrack(playlistName);

    ASSERT_TRUE(manager.IsInCrossfade());
    ASSERT_GT(playlist->nextSegment.durationSeconds, 0.0f);
    EXPECT_NEAR(playlist->nextSegment.durationSeconds, 0.5f, 0.002f);
    EXPECT_LE(
        playlist->activeCrossfadeDuration,
        playlist->nextSegment.durationSeconds * 0.5f);
    EXPECT_LT(
        playlist->activeCrossfadeDuration,
        playlist->nextSegment.durationSeconds);

    manager.Update(playlist->activeCrossfadeDuration + 0.001f);
    ASSERT_FALSE(manager.IsInCrossfade());
    EXPECT_EQ(manager.GetCurrentTrackName(), incomingTrack);
    EXPECT_GT(
        playlist->currentSegment.durationSeconds -
            playlist->currentSegment.elapsedSeconds,
        0.0f);
}

TEST_F(PlaylistResumeTests, OutgoingChannelLossStillPromotesPreparedIncomingTrack)
{
    constexpr const char* playlistName = "outgoing_loss_playlist";
    constexpr const char* firstTrack = "outgoing_loss_first";
    constexpr const char* secondTrack = "outgoing_loss_second";
    CreatePlaylistWithTracks(playlistName, {firstTrack, secondTrack});

    auto& manager = PlaylistManager::GetInstance();
    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    playlist->crossfadeDuration = 10.0f;

    PlaylistOptions options;
    options.loopPlaylist = true;
    manager.Play(playlistName, options);
    manager.SkipToNextTrack(playlistName);
    ASSERT_TRUE(manager.IsInCrossfade());

    FMOD::Channel* const outgoing = manager.GetCurrentChannel();
    FMOD::Channel* const incoming = manager.GetNextChannel();
    ASSERT_NE(outgoing, nullptr);
    ASSERT_NE(incoming, nullptr);
    SetNormalizationGain(secondTrack, 0.4f);
    ASSERT_EQ(outgoing->stop(), FMOD_OK);

    manager.Update(2.01f);

    EXPECT_FALSE(manager.IsInCrossfade());
    EXPECT_EQ(manager.GetCurrentTrackName(), secondTrack);
    EXPECT_EQ(manager.GetCurrentChannel(), incoming);
    EXPECT_FALSE(IsStopped(incoming));
    float promotedVolume = 0.0f;
    ASSERT_EQ(incoming->getVolume(&promotedVolume), FMOD_OK);
    EXPECT_NEAR(promotedVolume, 0.4f, 0.001f);
}

TEST_F(PlaylistResumeTests, IncomingChannelLossRecoversWithoutStoppingPlaylist)
{
    constexpr const char* playlistName = "incoming_loss_playlist";
    CreatePlaylistWithTracks(
        playlistName, {"incoming_loss_first", "incoming_loss_second"});

    auto& manager = PlaylistManager::GetInstance();
    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    playlist->crossfadeDuration = 10.0f;

    PlaylistOptions options;
    options.loopPlaylist = true;
    manager.Play(playlistName, options);
    manager.SkipToNextTrack(playlistName);
    ASSERT_TRUE(manager.IsInCrossfade());

    FMOD::Channel* const failedIncoming = manager.GetNextChannel();
    ASSERT_NE(failedIncoming, nullptr);
    SetNormalizationGain("incoming_loss_first", 0.5f);
    ASSERT_EQ(failedIncoming->stop(), FMOD_OK);

    // A failed staged channel cancels the overlap and restores the still-live
    // programme channel immediately; it must not fade into silence.
    manager.Update(2.01f);

    EXPECT_TRUE(manager.IsPlaylistPlaying(playlistName));
    EXPECT_FALSE(manager.IsInCrossfade());
    EXPECT_STREQ(manager.GetLastTransitionReason(), "playback failure");
    ASSERT_NE(manager.GetCurrentChannel(), nullptr);
    EXPECT_FALSE(IsStopped(manager.GetCurrentChannel()));
    float restoredVolume = 0.0f;
    ASSERT_EQ(
        manager.GetCurrentChannel()->getVolume(&restoredVolume), FMOD_OK);
    EXPECT_NEAR(restoredVolume, 0.5f, 0.001f);
}

TEST_F(PlaylistResumeTests, ZeroDurationCrossfadePromotesAudibleTrackImmediately)
{
    constexpr const char* playlistName = "zero_crossfade_playlist";
    CreatePlaylistWithTracks(
        playlistName, {"zero_crossfade_first", "zero_crossfade_second"});

    auto& manager = PlaylistManager::GetInstance();
    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    playlist->crossfadeDuration = 0.0f;

    PlaylistOptions options;
    options.loopPlaylist = true;
    manager.Play(playlistName, options);
    manager.SkipToNextTrack(playlistName);

    EXPECT_FALSE(manager.IsInCrossfade());
    EXPECT_EQ(manager.GetCurrentTrackName(), "zero_crossfade_second");
    FMOD::Channel* current = manager.GetCurrentChannel();
    ASSERT_NE(current, nullptr);
    float volume = 0.0f;
    ASSERT_EQ(current->getVolume(&volume), FMOD_OK);
    EXPECT_GT(volume, 0.0f);
}

TEST_F(PlaylistResumeTests, ActiveRuntimeOptionsRestartAtomicallyAndRemainResumable)
{
    constexpr const char* playlistName = "live_options_playlist";
    CreatePlaylistWithTracks(
        playlistName, {"live_options_first", "live_options_second"});

    auto& manager = PlaylistManager::GetInstance();
    PlaylistOptions initial;
    initial.loopPlaylist = true;
    manager.Play(playlistName, initial);
    ASSERT_TRUE(manager.IsPlaylistPlaying(playlistName));

    PlaylistOptions updated = initial;
    updated.randomOrder = true;
    updated.randomSegment = true;
    updated.segmentDuration = 0.05f;
    updated.minSegmentDuration = 0.02f;
    updated.maxSegmentDuration = 0.08f;
    ASSERT_TRUE(manager.ConfigurePlaylistPlayback(
        playlistName, updated, 0.0f, true));

    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    EXPECT_TRUE(playlist->isPlaying);
    EXPECT_TRUE(playlist->options.randomOrder);
    EXPECT_TRUE(playlist->options.randomSegment);
    EXPECT_FALSE(playlist->randomIndices.empty());
    EXPECT_TRUE(playlist->currentSegment.active);

    const PlaybackState checkpoint = manager.CapturePlaybackState();
    EXPECT_TRUE(checkpoint.isPlaying);
    EXPECT_TRUE(checkpoint.segmentActive);
    EXPECT_TRUE(checkpoint.options.randomSegment);
    manager.AbortImmediately();
    std::string resumeError;
    EXPECT_TRUE(manager.ResumePlaybackState(checkpoint, resumeError))
        << resumeError;
}

TEST_F(
    PlaylistResumeTests,
    SmartRandomSelectionPreservesPermutationCursorAcrossCrossfadeResume)
{
    constexpr const char* playlistName = "smart_permutation_playlist";
    const std::vector<std::string> tracks = {
        "smart_permutation_a",
        "smart_permutation_b",
        "smart_permutation_c"};
    CreatePlaylistWithTracks(playlistName, tracks);
    for (const std::string& track : tracks)
    {
        InjectMusicAnalysis(
            track, MakeDeterministicSegmentAnalysis(1.0f, 5.0f));
    }

    auto& manager = PlaylistManager::GetInstance();
    PlaylistOptions options;
    options.randomOrder = true;
    options.randomSegment = true;
    options.automaticSegmentDuration = true;
    options.segmentDuration = 4.0f;
    options.minSegmentDuration = 3.0f;
    options.maxSegmentDuration = 6.0f;
    manager.Play(playlistName, options);

    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    ASSERT_EQ(playlist->randomIndices.size(), tracks.size());
    const int firstIndex = playlist->currentIndex;
    EXPECT_EQ(firstIndex, playlist->randomIndices[0]);

    manager.SkipToNextTrack(playlistName);
    ASSERT_TRUE(manager.IsInCrossfade());
    ASSERT_EQ(playlist->randomIndexPos, 1);
    ASSERT_EQ(playlist->nextIndex, playlist->randomIndices[1]);
    EXPECT_EQ(playlist->randomIndices[0], firstIndex);

    // This test exercises resume from the incoming half of the overlap. The
    // separate dominant-channel test covers the outgoing half explicitly.
    ASSERT_GT(playlist->activeCrossfadeDuration, 0.0f);
    manager.Update(playlist->activeCrossfadeDuration * 0.75f);
    ASSERT_TRUE(manager.IsInCrossfade());

    std::vector<int> sortedPermutation = playlist->randomIndices;
    std::sort(sortedPermutation.begin(), sortedPermutation.end());
    EXPECT_EQ(sortedPermutation, (std::vector<int>{0, 1, 2}));

    const PlaybackState checkpoint = manager.CapturePlaybackState();
    ASSERT_TRUE(checkpoint.isPlaying);
    EXPECT_EQ(checkpoint.trackIndex, playlist->randomIndices[1]);
    EXPECT_EQ(checkpoint.randomPermutationIndex, 1);
    EXPECT_EQ(checkpoint.randomPermutation, playlist->randomIndices);

    manager.AbortImmediately();
    std::string resumeError;
    ASSERT_TRUE(manager.ResumePlaybackState(checkpoint, resumeError))
        << resumeError;

    playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    EXPECT_EQ(playlist->currentIndex, checkpoint.trackIndex);
    EXPECT_EQ(playlist->randomIndexPos, 1);
    EXPECT_EQ(playlist->randomIndices, checkpoint.randomPermutation);

    manager.SkipToNextTrack(playlistName);
    ASSERT_TRUE(manager.IsInCrossfade());
    EXPECT_EQ(playlist->randomIndexPos, 2);
    EXPECT_EQ(playlist->nextIndex, playlist->randomIndices[2]);
    EXPECT_NE(playlist->nextIndex, firstIndex);
    EXPECT_NE(playlist->nextIndex, checkpoint.trackIndex);
}

TEST_F(
    PlaylistResumeTests,
    SmartTrackSelectionFineScoresAtMostThirtyTwoTracks)
{
    constexpr const char* playlistName = "bounded_smart_selection_playlist";
    std::vector<std::string> tracks;
    tracks.reserve(40u);
    for (std::size_t index = 0u; index < 40u; ++index)
        tracks.push_back("bounded_smart_track_" + std::to_string(index));
    CreatePlaylistWithTracks(playlistName, tracks);
    for (const std::string& track : tracks)
    {
        InjectMusicAnalysis(
            track, MakeDeterministicSegmentAnalysis(1.0f, 5.0f));
    }

    auto& manager = PlaylistManager::GetInstance();
    PlaylistOptions options;
    options.randomOrder = true;
    options.randomSegment = true;
    options.automaticSegmentDuration = true;
    options.segmentDuration = 4.0f;
    options.minSegmentDuration = 3.0f;
    options.maxSegmentDuration = 6.0f;
    manager.Play(playlistName, options);

    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    manager.SkipToNextTrack(playlistName);

    EXPECT_EQ(playlist->lastSmartCandidatePoolSize, 39u);
    EXPECT_EQ(playlist->lastSmartShortlistSize, 32u);
    EXPECT_EQ(playlist->lastSmartFineEvaluationCount, 32u);
    EXPECT_EQ(playlist->lastSmartFineCandidateBudgetPerKind, 128u);
    EXPECT_EQ(playlist->randomIndexPos, 1);
    EXPECT_EQ(playlist->nextIndex, playlist->randomIndices[1]);
}

TEST_F(
    PlaylistResumeTests,
    CrossfadeCheckpointFollowsTheDominantChannelAndRandomCursor)
{
    constexpr const char* playlistName = "dominant_checkpoint_playlist";
    CreatePlaylistWithTracks(
        playlistName,
        {"dominant_checkpoint_a", "dominant_checkpoint_b",
         "dominant_checkpoint_c"});

    auto& manager = PlaylistManager::GetInstance();
    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    playlist->crossfadeDuration = 10.0f;

    PlaylistOptions options;
    options.randomOrder = true;
    options.loopPlaylist = true;
    manager.Play(playlistName, options);
    FMOD::Channel* const initialChannel = manager.GetCurrentChannel();
    ASSERT_NE(initialChannel, nullptr);
    AudioManager::GetInstance().SetChannelVolume(
        initialChannel, 1.0f);
    const int outgoingIndex = playlist->currentIndex;
    manager.SkipToNextTrack(playlistName);
    ASSERT_TRUE(manager.IsInCrossfade());
    const int incomingIndex = playlist->nextIndex;
    ASSERT_NE(outgoingIndex, incomingIndex);

    // Manual transitions are capped at two seconds. Before their midpoint,
    // recovering the outgoing programme loses the least audible content.
    // Deliberately invert the raw FMOD volumes to emulate very different
    // per-track normalization gains: envelope progress must remain decisive.
    manager.Update(0.25f);
    ASSERT_NE(manager.GetCurrentChannel(), nullptr);
    ASSERT_NE(manager.GetNextChannel(), nullptr);
    AudioManager::GetInstance().SetChannelVolume(
        manager.GetCurrentChannel(), 0.01f);
    AudioManager::GetInstance().SetChannelVolume(
        manager.GetNextChannel(), 2.0f);
    const PlaybackState beforeMidpoint = manager.CapturePlaybackState();
    ASSERT_TRUE(beforeMidpoint.isPlaying);
    EXPECT_EQ(beforeMidpoint.trackIndex, outgoingIndex);
    ASSERT_EQ(beforeMidpoint.randomPermutationIndex, 0);
    EXPECT_EQ(
        beforeMidpoint.randomPermutation[beforeMidpoint.randomPermutationIndex],
        outgoingIndex);

    manager.Update(1.0f);
    ASSERT_TRUE(manager.IsInCrossfade());
    // Invert the raw volumes the other way after the midpoint. Recovery must
    // still follow the incoming equal-power envelope.
    AudioManager::GetInstance().SetChannelVolume(
        manager.GetCurrentChannel(), 2.0f);
    AudioManager::GetInstance().SetChannelVolume(
        manager.GetNextChannel(), 0.01f);
    const PlaybackState afterMidpoint = manager.CapturePlaybackState();
    ASSERT_TRUE(afterMidpoint.isPlaying);
    EXPECT_EQ(afterMidpoint.trackIndex, incomingIndex);
    ASSERT_EQ(afterMidpoint.randomPermutationIndex, 1);
    EXPECT_EQ(
        afterMidpoint.randomPermutation[afterMidpoint.randomPermutationIndex],
        incomingIndex);
}

TEST_F(
    PlaylistResumeTests,
    FailedLiveOptionRestartRestoresThePreviousProgrammeChannel)
{
    constexpr const char* playlistName = "atomic_live_options_playlist";
    CreatePlaylistWithTracks(
        playlistName, {"atomic_live_a", "atomic_live_b"});

    auto& manager = PlaylistManager::GetInstance();
    PlaylistOptions initial;
    initial.loopPlaylist = true;
    manager.Play(playlistName, initial);
    FMOD::Channel* const previousChannel = manager.GetCurrentChannel();
    ASSERT_NE(previousChannel, nullptr);

    PlaylistOptions replacement = initial;
    replacement.randomOrder = true;
    replacement.randomSegment = true;
    replacement.segmentDuration = 4.0f;
    replacement.minSegmentDuration = 3.0f;
    replacement.maxSegmentDuration = 6.0f;

    auto& audio = AudioManager::GetInstance();
    audio.SetNormalPlaybackBlocked(true);
    const bool changed = manager.ConfigurePlaylistPlayback(
        playlistName, replacement, 1.0f, true);
    audio.SetNormalPlaybackBlocked(false);

    EXPECT_FALSE(changed);
    auto* playlist = manager.GetPlaylistByName(playlistName);
    ASSERT_NE(playlist, nullptr);
    EXPECT_TRUE(playlist->isPlaying);
    EXPECT_FALSE(playlist->options.randomOrder);
    EXPECT_FALSE(playlist->options.randomSegment);
    EXPECT_EQ(manager.GetCurrentChannel(), previousChannel);
    EXPECT_FALSE(IsStopped(previousChannel));
}

} // namespace
} // namespace TSM::Tests
