#include "pch.h"

#include <cstdint>
#include <limits>

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

void WriteSilentWave(const std::filesystem::path& path)
{
    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bitsPerSample = 16;
    constexpr std::uint32_t sampleCount = sampleRate * 10u;
    constexpr std::uint32_t dataSize =
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

    void LoadMusic(const std::string& id)
    {
        const auto stamp = std::chrono::high_resolution_clock::now()
            .time_since_epoch().count();
        const std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("tsm_resume_" + id + "_" + std::to_string(stamp) + ".wav");
        temporaryPaths.push_back(path);
        WriteSilentWave(path);
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
    EXPECT_EQ(resumed.options.loopPlaylist, captured.options.loopPlaylist);
    EXPECT_FLOAT_EQ(resumed.options.segmentDuration, captured.options.segmentDuration);
    EXPECT_FLOAT_EQ(resumed.crossfadeDuration, captured.crossfadeDuration);
    EXPECT_EQ(resumed.randomPermutation, captured.randomPermutation);
    EXPECT_EQ(resumed.randomPermutationIndex, captured.randomPermutationIndex);
    EXPECT_EQ(resumed.segmentStartMs, captured.segmentStartMs);
    EXPECT_EQ(resumed.segmentElapsedMs, captured.segmentElapsedMs);
    EXPECT_NEAR(
        static_cast<double>(resumed.positionMs),
        static_cast<double>(captured.positionMs), 50.0);
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

} // namespace
} // namespace TSM::Tests
