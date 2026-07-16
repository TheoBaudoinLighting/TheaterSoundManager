#include "pch.h"
#include "tsm_atomic_file.h"

#include <cstdint>
#include <sstream>

namespace TSM::Tests
{
namespace
{

void WriteCinemaLittleEndian16(std::ofstream& output, std::uint16_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu)
    };
    output.write(bytes, sizeof(bytes));
}

void WriteCinemaLittleEndian32(std::ofstream& output, std::uint32_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
        static_cast<char>((value >> 16u) & 0xffu),
        static_cast<char>((value >> 24u) & 0xffu)
    };
    output.write(bytes, sizeof(bytes));
}

void WriteCinemaSilentWave(const std::filesystem::path& path)
{
    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bitsPerSample = 16;
    constexpr std::uint32_t sampleCount = sampleRate * 10u;
    constexpr std::uint32_t dataSize =
        sampleCount * channels * (bitsPerSample / 8u);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output.write("RIFF", 4);
    WriteCinemaLittleEndian32(output, 36u + dataSize);
    output.write("WAVEfmt ", 8);
    WriteCinemaLittleEndian32(output, 16u);
    WriteCinemaLittleEndian16(output, 1u);
    WriteCinemaLittleEndian16(output, channels);
    WriteCinemaLittleEndian32(output, sampleRate);
    WriteCinemaLittleEndian32(
        output, sampleRate * channels * (bitsPerSample / 8u));
    WriteCinemaLittleEndian16(output, channels * (bitsPerSample / 8u));
    WriteCinemaLittleEndian16(output, bitsPerSample);
    output.write("data", 4);
    WriteCinemaLittleEndian32(output, dataSize);
    const std::vector<char> silence(dataSize, 0);
    output.write(silence.data(), static_cast<std::streamsize>(silence.size()));
    ASSERT_TRUE(output.good());
}

bool ChannelIsPlaying(FMOD::Channel* channel)
{
    bool playing = false;
    return channel && channel->isPlaying(&playing) == FMOD_OK && playing;
}

class CinemaManagerTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto stamp = std::chrono::high_resolution_clock::now()
            .time_since_epoch().count();
        stateDirectory = std::filesystem::temp_directory_path() /
            ("tsm_cinema_manager_" + std::to_string(stamp));
        ASSERT_TRUE(std::filesystem::create_directories(stateDirectory));

        RuntimeOptions options;
        options.loadConfig = false;
        options.noSound = true;
        options.stateDirectory = stateDirectory.string();
        ASSERT_TRUE(runtime.Initialize(options, error)) << error;

        musicPath = stateDirectory / "music.wav";
        emergencyPath = stateDirectory / "evacuation.wav";
        WriteCinemaSilentWave(musicPath);
        WriteCinemaSilentWave(emergencyPath);
    }

    void TearDown() override
    {
        runtime.Shutdown();
        std::error_code removeError;
        std::filesystem::remove_all(stateDirectory, removeError);
    }

    void CreatePlaylist()
    {
        ASSERT_TRUE(AudioManager::GetInstance().LoadSound(
            "cinema_music", musicPath.string(), true,
            AudioManager::SoundKind::Music));
        auto& playlists = PlaylistManager::GetInstance();
        playlists.CreatePlaylist("cinema_playlist");
        playlists.AddToPlaylist("cinema_playlist", "cinema_music");
        auto* playlist = playlists.GetPlaylistByName("cinema_playlist");
        ASSERT_NE(playlist, nullptr);
        playlist->options.loopPlaylist = true;
    }

    CinemaConfig BaseCinemaConfig() const
    {
        CinemaConfig config;
        config.enabled = true;
        config.resume.enabled = true;
        config.resume.automatic = true;
        config.resume.maxAgeMinutes = 60;
        config.resume.checkpointIntervalSeconds = 1;
        CinemaSchedule schedule;
        schedule.id = "always";
        schedule.playlist = "cinema_playlist";
        schedule.period.kind = CinemaPeriodKind::Always;
        config.schedules.push_back(schedule);
        return config;
    }

    void EnableInterlockSafety(CinemaConfig& config)
    {
        ASSERT_TRUE(AudioManager::GetInstance().LoadEmergencySound(
            "evacuation", emergencyPath.string()));
        config.safety.enabled = true;
        config.safety.evacuationAnnouncementId = "evacuation";
        config.safety.interlock.enabled = true;
        config.safety.interlock.expectedHeartbeatSource = "fire-panel";
        config.safety.interlock.heartbeatTimeoutMilliseconds = 1000;
        config.safety.interlock.startupGraceMilliseconds = 10000;
        config.safety.interlock.safeStableMilliseconds = 0;
    }

    static SafetyHeartbeat Heartbeat(
        std::uint64_t sequence,
        InterlockSignal signal = InterlockSignal::Safe)
    {
        SafetyHeartbeat heartbeat;
        heartbeat.sourceId = "fire-panel";
        heartbeat.sessionId = "boot-1";
        heartbeat.sequence = sequence;
        heartbeat.signal = signal;
        return heartbeat;
    }

    ApplicationRuntime runtime;
    std::string error;
    std::filesystem::path stateDirectory;
    std::filesystem::path musicPath;
    std::filesystem::path emergencyPath;
    const CinemaManager::SystemClock::time_point systemEpoch =
        CinemaManager::SystemClock::time_point{std::chrono::seconds(1'800'000'000)};
    const CinemaManager::SteadyClock::time_point steadyEpoch{};
};

} // namespace

TEST_F(CinemaManagerTests, CalendarStartsOnceAndPersistsCleanShutdown)
{
    CreatePlaylist();
    CinemaManager manager;
    ASSERT_TRUE(manager.Initialize(
        BaseCinemaConfig(), stateDirectory, "fingerprint-a", error,
        systemEpoch, steadyEpoch)) << error;

    auto& playlists = PlaylistManager::GetInstance();
    ASSERT_TRUE(playlists.IsPlaylistPlaying("cinema_playlist"));
    FMOD::Channel* firstChannel = playlists.GetCurrentChannel();
    ASSERT_TRUE(ChannelIsPlaying(firstChannel));

    manager.Tick(systemEpoch + std::chrono::minutes(1), steadyEpoch + std::chrono::minutes(1));
    EXPECT_EQ(playlists.GetCurrentChannel(), firstChannel);
    const CinemaStatus status = manager.GetStatus(systemEpoch, steadyEpoch);
    EXPECT_EQ(status.activeScheduleId, "always");
    EXPECT_EQ(status.activePlaylist, "cinema_playlist");

    manager.Shutdown(systemEpoch + std::chrono::minutes(2));
    const JsonFileReadResult state = ReadJsonFileStrict(
        stateDirectory / "playback_state.json");
    ASSERT_TRUE(state.IsSuccess()) << state.error;
    EXPECT_TRUE(state.document.at("cleanShutdown").get<bool>());
    EXPECT_EQ(state.document.at("schemaVersion"), 1);
}

TEST_F(CinemaManagerTests, ManualLibraryPlaybackTemporarilyOverridesCalendar)
{
    CreatePlaylist();
    ASSERT_TRUE(AudioManager::GetInstance().LoadSound(
        "manual_library_music", musicPath.string(), true,
        AudioManager::SoundKind::Music));

    CinemaManager manager;
    ASSERT_TRUE(manager.Initialize(
        BaseCinemaConfig(), stateDirectory, "fingerprint-manual-library", error,
        systemEpoch, steadyEpoch)) << error;

    auto& playlists = PlaylistManager::GetInstance();
    ASSERT_TRUE(playlists.IsPlaylistPlaying("cinema_playlist"));
    PlaylistOptions options;
    options.loopPlaylist = true;
    ASSERT_TRUE(playlists.PlayLibrary(options, 0.0f));
    ASSERT_TRUE(playlists.IsLibraryPlaying());
    ASSERT_FALSE(playlists.IsPlaylistPlaying("cinema_playlist"));

    manager.Tick(
        systemEpoch + std::chrono::seconds(1),
        steadyEpoch + std::chrono::seconds(1));
    EXPECT_TRUE(playlists.IsLibraryPlaying());
    EXPECT_FALSE(playlists.IsPlaylistPlaying("cinema_playlist"));
    const CinemaStatus overridden = manager.GetStatus(
        systemEpoch + std::chrono::seconds(1),
        steadyEpoch + std::chrono::seconds(1));
    EXPECT_TRUE(overridden.activePlaylist.empty());
    EXPECT_EQ(overridden.selectedPlaylist, "cinema_playlist");

    playlists.StopLibrary();
    manager.Tick(
        systemEpoch + std::chrono::seconds(2),
        steadyEpoch + std::chrono::seconds(2));
    EXPECT_FALSE(playlists.IsLibraryPlaying());
    EXPECT_TRUE(playlists.IsPlaylistPlaying("cinema_playlist"));
}

TEST_F(CinemaManagerTests, FireAlarmStopsProgrammeAndResetNeverResumesImplicitly)
{
    CreatePlaylist();
    CinemaConfig config = BaseCinemaConfig();
    EnableInterlockSafety(config);

    CinemaManager manager;
    ASSERT_TRUE(manager.Initialize(
        config, stateDirectory, "fingerprint-b", error,
        systemEpoch, steadyEpoch)) << error;
    EXPECT_FALSE(manager.CanPlayNormalAudio());

    ASSERT_TRUE(manager.ObserveHeartbeat(
        Heartbeat(1), steadyEpoch, systemEpoch).ok);
    manager.Tick(systemEpoch, steadyEpoch);
    ASSERT_TRUE(manager.CanPlayNormalAudio());
    ASSERT_TRUE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema_playlist"));

    const CinemaResult alarm = manager.ObserveHeartbeat(
        Heartbeat(2, InterlockSignal::Alarm),
        steadyEpoch + std::chrono::milliseconds(10), systemEpoch);
    ASSERT_TRUE(alarm.ok) << alarm.message;
    EXPECT_FALSE(manager.CanPlayNormalAudio());
    EXPECT_TRUE(AudioManager::GetInstance().IsNormalPlaybackBlocked());
    EXPECT_FALSE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema_playlist"));
    EXPECT_TRUE(ChannelIsPlaying(
        AudioManager::GetInstance().GetLastChannelOfSound("evacuation")));

    AudioManager::GetInstance().StopSound("evacuation");
    ASSERT_FALSE(ChannelIsPlaying(
        AudioManager::GetInstance().GetLastChannelOfSound("evacuation")));
    manager.Tick(
        systemEpoch + std::chrono::milliseconds(15),
        steadyEpoch + std::chrono::milliseconds(15));
    EXPECT_TRUE(ChannelIsPlaying(
        AudioManager::GetInstance().GetLastChannelOfSound("evacuation")));

    ASSERT_TRUE(manager.ObserveHeartbeat(
        Heartbeat(3), steadyEpoch + std::chrono::milliseconds(20), systemEpoch).ok);
    const SafetySnapshot latched = manager.GetSafetySnapshot(
        steadyEpoch + std::chrono::milliseconds(20));
    ASSERT_EQ(latched.mode, SafetyMode::Latched);
    ASSERT_TRUE(latched.incident.has_value());

    const CinemaResult reset = manager.Reset(
        latched.incident->id, "operator-1", "Fire panel verified safe",
        steadyEpoch + std::chrono::milliseconds(20), systemEpoch);
    ASSERT_TRUE(reset.ok) << reset.message;
    EXPECT_TRUE(manager.CanPlayNormalAudio());
    EXPECT_FALSE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema_playlist"));
    EXPECT_FALSE(ChannelIsPlaying(
        AudioManager::GetInstance().GetLastChannelOfSound("evacuation")));

    manager.Tick(systemEpoch + std::chrono::seconds(1),
                 steadyEpoch + std::chrono::seconds(1));
    EXPECT_FALSE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema_playlist"));
    EXPECT_TRUE(manager.GetStatus(systemEpoch, steadyEpoch).automationSuspended);

    const CinemaResult resumed = manager.ResumePending(
        systemEpoch + std::chrono::seconds(1),
        steadyEpoch + std::chrono::seconds(1));
    ASSERT_TRUE(resumed.ok) << resumed.message;
    EXPECT_TRUE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema_playlist"));
}

TEST_F(CinemaManagerTests, InterlockFaultIsFailClosedAndNeverPlaysEvacuation)
{
    CreatePlaylist();
    CinemaConfig config = BaseCinemaConfig();
    EnableInterlockSafety(config);
    CinemaManager manager;
    ASSERT_TRUE(manager.Initialize(
        config, stateDirectory, "fingerprint-c", error,
        systemEpoch, steadyEpoch)) << error;
    ASSERT_TRUE(manager.ObserveHeartbeat(
        Heartbeat(1), steadyEpoch, systemEpoch).ok);
    manager.Tick(systemEpoch, steadyEpoch);
    ASSERT_TRUE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema_playlist"));

    ASSERT_TRUE(manager.ObserveHeartbeat(
        Heartbeat(2, InterlockSignal::Fault),
        steadyEpoch + std::chrono::milliseconds(1), systemEpoch).ok);
    EXPECT_FALSE(manager.CanPlayNormalAudio());
    EXPECT_FALSE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema_playlist"));
    EXPECT_FALSE(ChannelIsPlaying(
        AudioManager::GetInstance().GetLastChannelOfSound("evacuation")));
}

TEST_F(CinemaManagerTests, MissingStartupHeartbeatLatchesSilentlyAfterGracePeriod)
{
    CreatePlaylist();
    CinemaConfig config = BaseCinemaConfig();
    EnableInterlockSafety(config);
    config.safety.interlock.startupGraceMilliseconds = 500;

    CinemaManager manager;
    ASSERT_TRUE(manager.Initialize(
        config, stateDirectory, "fingerprint-startup-timeout", error,
        systemEpoch, steadyEpoch)) << error;

    manager.Tick(
        systemEpoch + std::chrono::milliseconds(499),
        steadyEpoch + std::chrono::milliseconds(499));
    EXPECT_EQ(
        manager.GetSafetySnapshot(steadyEpoch + std::chrono::milliseconds(499)).mode,
        SafetyMode::Inhibited);

    manager.Tick(
        systemEpoch + std::chrono::milliseconds(500),
        steadyEpoch + std::chrono::milliseconds(500));
    const SafetySnapshot latched = manager.GetSafetySnapshot(
        steadyEpoch + std::chrono::milliseconds(500));
    ASSERT_EQ(latched.mode, SafetyMode::Latched);
    ASSERT_TRUE(latched.incident.has_value());
    EXPECT_NE(
        std::find(
            latched.incident->causes.begin(), latched.incident->causes.end(),
            "heartbeat_startup_timeout"),
        latched.incident->causes.end());
    EXPECT_FALSE(manager.CanPlayNormalAudio());
    EXPECT_FALSE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema_playlist"));
    EXPECT_FALSE(ChannelIsPlaying(
        AudioManager::GetInstance().GetLastChannelOfSound("evacuation")));
}

TEST_F(CinemaManagerTests, LostRuntimeHeartbeatStopsProgrammeWithoutEvacuation)
{
    CreatePlaylist();
    CinemaConfig config = BaseCinemaConfig();
    EnableInterlockSafety(config);

    CinemaManager manager;
    ASSERT_TRUE(manager.Initialize(
        config, stateDirectory, "fingerprint-runtime-timeout", error,
        systemEpoch, steadyEpoch)) << error;
    ASSERT_TRUE(manager.ObserveHeartbeat(
        Heartbeat(1), steadyEpoch, systemEpoch).ok);
    manager.Tick(systemEpoch, steadyEpoch);
    ASSERT_TRUE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema_playlist"));

    manager.Tick(
        systemEpoch + std::chrono::milliseconds(1001),
        steadyEpoch + std::chrono::milliseconds(1001));
    const SafetySnapshot latched = manager.GetSafetySnapshot(
        steadyEpoch + std::chrono::milliseconds(1001));
    ASSERT_EQ(latched.mode, SafetyMode::Latched);
    ASSERT_TRUE(latched.incident.has_value());
    EXPECT_NE(
        std::find(
            latched.incident->causes.begin(), latched.incident->causes.end(),
            "heartbeat_timeout@fire-panel"),
        latched.incident->causes.end());
    EXPECT_FALSE(manager.CanPlayNormalAudio());
    EXPECT_FALSE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema_playlist"));
    EXPECT_FALSE(ChannelIsPlaying(
        AudioManager::GetInstance().GetLastChannelOfSound("evacuation")));
}

TEST_F(CinemaManagerTests, CorruptPlaybackStateRequiresExplicitDiscard)
{
    CreatePlaylist();
    {
        std::ofstream corrupt(
            stateDirectory / "playback_state.json",
            std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(corrupt.is_open());
        corrupt << R"({"schemaVersion":1,"unexpected":true})";
    }

    CinemaManager manager;
    ASSERT_TRUE(manager.Initialize(
        BaseCinemaConfig(), stateDirectory, "fingerprint-d", error,
        systemEpoch, steadyEpoch)) << error;
    EXPECT_FALSE(manager.CanPlayNormalAudio());
    EXPECT_TRUE(manager.GetStatus(systemEpoch, steadyEpoch).operationalInhibit);
    EXPECT_TRUE(AudioManager::GetInstance().IsNormalPlaybackBlocked());

    const CinemaResult discarded = manager.DiscardPending(systemEpoch);
    ASSERT_TRUE(discarded.ok) << discarded.message;
    manager.Tick(systemEpoch, steadyEpoch);
    EXPECT_TRUE(manager.CanPlayNormalAudio());
    EXPECT_FALSE(AudioManager::GetInstance().IsNormalPlaybackBlocked());
    EXPECT_FALSE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema_playlist"));
}

TEST_F(CinemaManagerTests, DirtyFreshCheckpointResumesAutomaticallyAfterPowerLoss)
{
    CreatePlaylist();
    const std::filesystem::path checkpoint = stateDirectory / "playback_state.json";
    std::string dirtyCheckpoint;
    {
        CinemaManager first;
        ASSERT_TRUE(first.Initialize(
            BaseCinemaConfig(), stateDirectory, "fingerprint-e", error,
            systemEpoch, steadyEpoch)) << error;
        ASSERT_TRUE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema_playlist"));
        first.Tick(
            systemEpoch + std::chrono::seconds(2),
            steadyEpoch + std::chrono::seconds(2));

        std::ifstream input(checkpoint, std::ios::binary);
        ASSERT_TRUE(input.is_open());
        std::ostringstream payload;
        payload << input.rdbuf();
        dirtyCheckpoint = payload.str();
        ASSERT_FALSE(dirtyCheckpoint.empty());
        input.close();
        first.Shutdown(systemEpoch + std::chrono::seconds(2));
    }
    PlaylistManager::GetInstance().AbortImmediately();
    {
        std::ofstream output(checkpoint, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output << dirtyCheckpoint;
        ASSERT_TRUE(output.good());
    }

    CinemaManager restarted;
    ASSERT_TRUE(restarted.Initialize(
        BaseCinemaConfig(), stateDirectory, "fingerprint-e", error,
        systemEpoch + std::chrono::seconds(3),
        steadyEpoch + std::chrono::seconds(3))) << error;
    EXPECT_TRUE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema_playlist"));
    const CinemaStatus status = restarted.GetStatus(
        systemEpoch + std::chrono::seconds(3),
        steadyEpoch + std::chrono::seconds(3));
    EXPECT_FALSE(status.resume.pending);
    EXPECT_EQ(status.resume.status, "resumed_automatically");
}

} // namespace TSM::Tests
