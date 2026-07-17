#include "tsm_cinema_manager.h"

#include "tsm_announcement_manager.h"
#include "tsm_atomic_file.h"
#include "tsm_audio_manager.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace TSM
{
namespace
{

constexpr int PlaybackStateSchemaVersion = 2;
constexpr std::size_t MaximumStateBytes = 1024U * 1024U;
constexpr std::size_t MaximumIdentityLength = 256;
constexpr std::size_t MaximumRandomPermutationSize = 10000;
constexpr auto MaximumFutureClockSkew = std::chrono::minutes(5);

struct PersistedPlayback
{
    std::uint64_t generation = 0;
    bool cleanShutdown = true;
    std::int64_t capturedAtUnixMilliseconds = 0;
    std::string configFingerprint;
    PlaybackState playback;
};

std::int64_t UnixMilliseconds(CinemaManager::SystemClock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

bool HasExactKeys(
    const nlohmann::json& value,
    std::initializer_list<std::string_view> expected)
{
    if (!value.is_object() || value.size() != expected.size()) return false;
    for (const std::string_view key : expected)
    {
        if (!value.contains(std::string(key))) return false;
    }
    return true;
}

bool ReadBoundedString(
    const nlohmann::json& value,
    std::string& result,
    bool allowEmpty = true)
{
    if (!value.is_string()) return false;
    const std::string parsed = value.get<std::string>();
    if ((!allowEmpty && parsed.empty()) || parsed.size() > MaximumIdentityLength)
        return false;
    if (std::any_of(parsed.begin(), parsed.end(), [](unsigned char character) {
            return character < 0x20 || character == 0x7f;
        }))
        return false;
    result = parsed;
    return true;
}

bool ReadUnsigned64(const nlohmann::json& value, std::uint64_t& result)
{
    if (value.is_number_unsigned())
    {
        result = value.get<std::uint64_t>();
        return true;
    }
    if (!value.is_number_integer()) return false;
    const std::int64_t parsed = value.get<std::int64_t>();
    if (parsed < 0) return false;
    result = static_cast<std::uint64_t>(parsed);
    return true;
}

bool ReadUnsigned32(const nlohmann::json& value, std::uint32_t& result)
{
    std::uint64_t parsed = 0;
    if (!ReadUnsigned64(value, parsed) ||
        parsed > (std::numeric_limits<std::uint32_t>::max)())
        return false;
    result = static_cast<std::uint32_t>(parsed);
    return true;
}

bool ReadInt(const nlohmann::json& value, int minimum, int maximum, int& result)
{
    if (value.is_number_unsigned())
    {
        const std::uint64_t parsed = value.get<std::uint64_t>();
        if (minimum > 0 && parsed < static_cast<std::uint64_t>(minimum)) return false;
        if (parsed > static_cast<std::uint64_t>(maximum)) return false;
        result = static_cast<int>(parsed);
        return true;
    }
    if (!value.is_number_integer()) return false;
    const std::int64_t parsed = value.get<std::int64_t>();
    if (parsed < minimum || parsed > maximum) return false;
    result = static_cast<int>(parsed);
    return true;
}

bool ReadFiniteFloat(
    const nlohmann::json& value,
    float minimum,
    float maximum,
    float& result)
{
    if (!value.is_number()) return false;
    const double parsed = value.get<double>();
    if (!std::isfinite(parsed) || parsed < minimum || parsed > maximum) return false;
    result = static_cast<float>(parsed);
    return true;
}

nlohmann::json SerializePlaybackState(const PlaybackState& state)
{
    return {
        {"isPlaying", state.isPlaying},
        {"playlistName", state.playlistName},
        {"trackId", state.trackId},
        {"trackIndex", state.trackIndex},
        {"positionMs", state.positionMs},
        {"options", {
            {"randomOrder", state.options.randomOrder},
            {"randomSegment", state.options.randomSegment},
            {"segmentDuration", state.options.segmentDuration},
            {"automaticSegmentDuration", state.options.automaticSegmentDuration},
            {"minSegmentDuration", state.options.minSegmentDuration},
            {"maxSegmentDuration", state.options.maxSegmentDuration},
            {"loopPlaylist", state.options.loopPlaylist}
        }},
        {"crossfadeDuration", state.crossfadeDuration},
        {"randomPermutation", state.randomPermutation},
        {"randomPermutationIndex", state.randomPermutationIndex},
        {"segmentActive", state.segmentActive},
        {"segmentStartMs", state.segmentStartMs},
        {"segmentElapsedMs", state.segmentElapsedMs},
        {"segmentDurationMs", state.segmentDurationMs}
    };
}

bool ParsePlaybackState(
    const nlohmann::json& value,
    int schemaVersion,
    PlaybackState& state,
    std::string& errorMessage)
{
    const bool fieldsMatch = schemaVersion == 1
        ? HasExactKeys(
              value,
              {"isPlaying", "playlistName", "trackId", "trackIndex", "positionMs",
               "options", "crossfadeDuration", "randomPermutation",
               "randomPermutationIndex", "segmentActive", "segmentStartMs",
               "segmentElapsedMs"})
        : HasExactKeys(
              value,
              {"isPlaying", "playlistName", "trackId", "trackIndex", "positionMs",
               "options", "crossfadeDuration", "randomPermutation",
               "randomPermutationIndex", "segmentActive", "segmentStartMs",
               "segmentElapsedMs", "segmentDurationMs"});
    if (!fieldsMatch)
    {
        errorMessage = "Playback state fields do not match schema version " +
            std::to_string(schemaVersion) + ".";
        return false;
    }
    if (!value["isPlaying"].is_boolean() ||
        !ReadBoundedString(value["playlistName"], state.playlistName) ||
        !ReadBoundedString(value["trackId"], state.trackId) ||
        !ReadInt(value["trackIndex"], -1, 1000000, state.trackIndex) ||
        !ReadUnsigned32(value["positionMs"], state.positionMs) ||
        !ReadFiniteFloat(value["crossfadeDuration"], 0.0f, 3600.0f, state.crossfadeDuration) ||
        !value["segmentActive"].is_boolean() ||
        !ReadUnsigned32(value["segmentStartMs"], state.segmentStartMs) ||
        !ReadUnsigned32(value["segmentElapsedMs"], state.segmentElapsedMs) ||
        (schemaVersion >= 2 &&
         !ReadUnsigned32(value["segmentDurationMs"], state.segmentDurationMs)))
    {
        errorMessage = "Playback state contains an invalid scalar value.";
        return false;
    }
    state.isPlaying = value["isPlaying"].get<bool>();
    state.segmentActive = value["segmentActive"].get<bool>();

    const auto& options = value["options"];
    const bool optionFieldsMatch = schemaVersion == 1
        ? HasExactKeys(
              options,
              {"randomOrder", "randomSegment", "segmentDuration", "loopPlaylist"})
        : HasExactKeys(
              options,
              {"randomOrder", "randomSegment", "segmentDuration",
               "automaticSegmentDuration", "minSegmentDuration",
               "maxSegmentDuration", "loopPlaylist"});
    if (!optionFieldsMatch ||
        !options["randomOrder"].is_boolean() ||
        !options["randomSegment"].is_boolean() ||
        !options["loopPlaylist"].is_boolean() ||
        !ReadFiniteFloat(
            options["segmentDuration"], 0.001f, 86400.0f,
            state.options.segmentDuration) ||
        (schemaVersion >= 2 &&
         (!options["automaticSegmentDuration"].is_boolean() ||
          !ReadFiniteFloat(
              options["minSegmentDuration"], 0.001f, 86400.0f,
              state.options.minSegmentDuration) ||
          !ReadFiniteFloat(
              options["maxSegmentDuration"], 0.001f, 86400.0f,
              state.options.maxSegmentDuration))))
    {
        errorMessage = "Playback options are invalid.";
        return false;
    }
    state.options.randomOrder = options["randomOrder"].get<bool>();
    state.options.randomSegment = options["randomSegment"].get<bool>();
    state.options.loopPlaylist = options["loopPlaylist"].get<bool>();
    if (schemaVersion >= 2)
    {
        state.options.automaticSegmentDuration =
            options["automaticSegmentDuration"].get<bool>();
        if (state.options.minSegmentDuration > state.options.maxSegmentDuration)
        {
            errorMessage = "Playback automatic segment range is invalid.";
            return false;
        }
    }
    else
    {
        state.options.automaticSegmentDuration = false;
        state.options.minSegmentDuration =
            PlaylistOptions::DefaultMinimumSegmentDuration;
        state.options.maxSegmentDuration =
            PlaylistOptions::DefaultMaximumSegmentDuration;
        state.segmentDurationMs = state.segmentActive
            ? static_cast<std::uint32_t>(std::llround(
                  static_cast<double>(state.options.segmentDuration) * 1000.0))
            : 0;
    }

    const auto& permutation = value["randomPermutation"];
    if (!permutation.is_array() ||
        permutation.size() > MaximumRandomPermutationSize ||
        !ReadInt(
            value["randomPermutationIndex"], -1,
            static_cast<int>(MaximumRandomPermutationSize),
            state.randomPermutationIndex))
    {
        errorMessage = "Playback random-order state is invalid.";
        return false;
    }
    state.randomPermutation.clear();
    state.randomPermutation.reserve(permutation.size());
    for (const auto& item : permutation)
    {
        int index = 0;
        if (!ReadInt(item, 0, 1000000, index))
        {
            errorMessage = "Playback permutation contains an invalid index.";
            return false;
        }
        state.randomPermutation.push_back(index);
    }

    if (state.isPlaying &&
        (state.playlistName.empty() || state.trackId.empty() || state.trackIndex < 0))
    {
        errorMessage = "Active playback has no playlist or track identity.";
        return false;
    }
    if (!state.isPlaying &&
        (!state.playlistName.empty() || !state.trackId.empty() || state.trackIndex != -1))
    {
        errorMessage = "Inactive playback contains an active track identity.";
        return false;
    }
    if ((!state.segmentActive &&
         (state.segmentStartMs != 0 || state.segmentElapsedMs != 0 ||
          state.segmentDurationMs != 0)) ||
        (state.segmentActive &&
         (state.segmentDurationMs == 0 ||
          state.segmentElapsedMs > state.segmentDurationMs)))
    {
        errorMessage = "Playback segment progress is invalid.";
        return false;
    }
    return true;
}

nlohmann::json SerializeCheckpoint(
    std::uint64_t generation,
    bool cleanShutdown,
    std::int64_t capturedAtUnixMilliseconds,
    const std::string& fingerprint,
    const PlaybackState& playback)
{
    return {
        {"schemaVersion", PlaybackStateSchemaVersion},
        {"generation", generation},
        {"cleanShutdown", cleanShutdown},
        {"capturedAtUnixMs", capturedAtUnixMilliseconds},
        {"configFingerprint", fingerprint},
        {"playback", SerializePlaybackState(playback)}
    };
}

bool ParseCheckpoint(
    const nlohmann::json& value,
    PersistedPlayback& result,
    std::string& errorMessage)
{
    if (!HasExactKeys(
            value,
            {"schemaVersion", "generation", "cleanShutdown", "capturedAtUnixMs",
             "configFingerprint", "playback"}) ||
        !value["schemaVersion"].is_number_integer())
    {
        errorMessage = "Playback checkpoint header is invalid.";
        return false;
    }
    const int schemaVersion = value["schemaVersion"].get<int>();
    if ((schemaVersion != 1 && schemaVersion != PlaybackStateSchemaVersion) ||
        !ReadUnsigned64(value["generation"], result.generation) ||
        !value["cleanShutdown"].is_boolean() ||
        !value["capturedAtUnixMs"].is_number_integer() ||
        !ReadBoundedString(value["configFingerprint"], result.configFingerprint, false))
    {
        errorMessage = "Playback checkpoint header uses an unsupported schema.";
        return false;
    }
    result.cleanShutdown = value["cleanShutdown"].get<bool>();
    result.capturedAtUnixMilliseconds = value["capturedAtUnixMs"].get<std::int64_t>();
    if (result.capturedAtUnixMilliseconds < 0)
    {
        errorMessage = "Playback checkpoint timestamp is invalid.";
        return false;
    }
    return ParsePlaybackState(
        value["playback"], schemaVersion, result.playback, errorMessage);
}

bool CauseStartsWith(const std::string& persistedCause, std::string_view expected)
{
    return persistedCause == expected ||
        (persistedCause.size() > expected.size() &&
         persistedCause.compare(0, expected.size(), expected) == 0 &&
         persistedCause[expected.size()] == '@');
}

} // namespace

CinemaResult CinemaResult::Success(std::string code, std::string message)
{
    CinemaResult result;
    result.ok = true;
    result.code = std::move(code);
    result.message = std::move(message);
    return result;
}

CinemaResult CinemaResult::Failure(std::string code, std::string message)
{
    CinemaResult result;
    result.code = std::move(code);
    result.message = std::move(message);
    return result;
}

CinemaManager::~CinemaManager()
{
    Shutdown();
}

bool CinemaManager::Initialize(
    CinemaConfig config,
    std::filesystem::path stateDirectory,
    std::string configFingerprint,
    std::string& errorMessage,
    SystemClock::time_point systemNow,
    SteadyClock::time_point steadyNow)
{
    errorMessage.clear();
    if (m_initialized)
    {
        errorMessage = "Cinema manager is already initialized.";
        return false;
    }

    m_config = std::move(config);
    m_configFingerprint = std::move(configFingerprint);
    m_stateEnabled = m_config.enabled || m_config.safety.enabled;
    m_shutdown = false;
    m_operationalInhibit = false;
    m_operationalInhibitReason.clear();
    m_persistenceError.clear();
    m_automationSuspended = false;
    m_everReady = false;
    m_emergencyPlaying = false;
    m_automationOwnsPlayback = false;
    m_automaticResumeAttempted = false;
    m_activeScheduleId.clear();
    m_activePlaylist.clear();
    m_selectedScheduleId.clear();
    m_selectedPlaylist.clear();
    m_resume = {};
    m_pendingPlayback.reset();
    m_pendingCapturedAtUnixMilliseconds = 0;
    m_generation = 0;
    m_startedAt = steadyNow;
    m_nextCheckpoint = steadyNow +
        std::chrono::seconds(m_config.resume.checkpointIntervalSeconds);

    if (m_stateEnabled)
    {
        if (stateDirectory.empty())
        {
            errorMessage = "Cinema state directory is empty.";
            return false;
        }
        if (m_configFingerprint.empty() ||
            m_configFingerprint.size() > MaximumIdentityLength)
        {
            errorMessage = "Cinema configuration fingerprint is invalid.";
            return false;
        }
        std::error_code directoryError;
        std::filesystem::create_directories(stateDirectory, directoryError);
        if (directoryError ||
            !std::filesystem::is_directory(stateDirectory, directoryError) ||
            directoryError)
        {
            errorMessage = "Unable to create the cinema state directory: " +
                directoryError.message();
            return false;
        }
        m_playbackStatePath = stateDirectory / "playback_state.json";
    }

    if (m_config.safety.enabled)
    {
        const auto sound = AudioManager::GetInstance().GetAllSounds().find(
            m_config.safety.evacuationAnnouncementId);
        if (m_config.safety.evacuationAnnouncementId.empty() ||
            sound == AudioManager::GetInstance().GetAllSounds().end() ||
            sound->second.kind != AudioManager::SoundKind::Emergency)
        {
            errorMessage =
                "The configured evacuation preset is missing or is not protected emergency audio.";
            return false;
        }
    }

    SafetyConfig safetyConfig;
    safetyConfig.enabled = m_config.safety.enabled;
    safetyConfig.heartbeatRequired = m_config.safety.interlock.enabled;
    safetyConfig.expectedHeartbeatSource =
        m_config.safety.interlock.expectedHeartbeatSource;
    safetyConfig.heartbeatTimeout = std::chrono::milliseconds(
        m_config.safety.interlock.heartbeatTimeoutMilliseconds);
    safetyConfig.healthyBeforeReady = std::chrono::milliseconds(
        m_config.safety.interlock.safeStableMilliseconds);
    safetyConfig.healthyBeforeReset = safetyConfig.healthyBeforeReady;
    const SafetyResult safetyInitialization = m_safety.Initialize(
        std::move(safetyConfig),
        m_stateEnabled ? stateDirectory / "safety_state.json" : std::filesystem::path{},
        steadyNow);
    m_previousSafetyMode = m_safety.GetMode();

    m_initialized = true;
    if (m_stateEnabled)
    {
        std::string checkpointError;
        if (!LoadPreviousCheckpoint(systemNow, checkpointError))
        {
            const std::string cause = checkpointError == "state_missing"
                ? "playback_state_missing"
                : "playback_state_invalid";
            if (checkpointError != "state_missing")
            {
                SetOperationalInhibit(cause, checkpointError);
            }
        }

        if (!m_operationalInhibit && !PersistCheckpoint(false, systemNow))
            SetOperationalInhibit("playback_state_write_failed", m_persistenceError);
    }

    if (!safetyInitialization.ok)
    {
        spdlog::error(
            "Cinema safety initialized fail-closed: {}: {}",
            safetyInitialization.code, safetyInitialization.message);
    }
    ApplyContainment(systemNow, steadyNow);

    if (CanPlayNormalAudio() && m_pendingPlayback && m_resume.automaticEligible)
    {
        const CinemaResult automatic = ResumePendingInternal(systemNow, steadyNow, true);
        if (!automatic.ok)
            spdlog::warn("Automatic cinema resume was refused: {}", automatic.message);
    }
    if (CanPlayNormalAudio() && !m_automationSuspended) UpdateSchedule(systemNow);
    return true;
}

bool CinemaManager::LoadPreviousCheckpoint(
    SystemClock::time_point systemNow,
    std::string& errorMessage)
{
    errorMessage.clear();
    const JsonFileReadResult read = ReadJsonFileStrict(
        m_playbackStatePath, MaximumStateBytes);
    if (read.status == JsonFileReadStatus::Missing)
    {
        m_resume.status = "none";
        errorMessage = "state_missing";
        return false;
    }
    if (!read.IsSuccess())
    {
        m_resume.status = read.status == JsonFileReadStatus::Invalid
            ? "corrupt" : "io_error";
        errorMessage = read.error;
        return false;
    }

    PersistedPlayback persisted;
    if (!ParseCheckpoint(read.document, persisted, errorMessage))
    {
        m_resume.status = "corrupt";
        return false;
    }
    m_generation = persisted.generation;
    m_resume.previousShutdownClean = persisted.cleanShutdown;
    m_resume.capturedAtUnixMilliseconds = persisted.capturedAtUnixMilliseconds;

    if (!m_config.resume.enabled)
    {
        m_resume.status = "disabled";
        return true;
    }
    if (persisted.cleanShutdown || !persisted.playback.isPlaying)
    {
        m_resume.status = persisted.cleanShutdown ? "clean_shutdown" : "no_playback";
        return true;
    }
    if (persisted.configFingerprint != m_configFingerprint)
    {
        m_resume.status = "configuration_changed";
        return true;
    }

    const std::int64_t nowMilliseconds = UnixMilliseconds(systemNow);
    const std::int64_t futureAllowance =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            MaximumFutureClockSkew).count();
    if (persisted.capturedAtUnixMilliseconds > nowMilliseconds + futureAllowance)
    {
        m_resume.status = "clock_invalid";
        return true;
    }
    const std::int64_t ageMilliseconds = std::max<std::int64_t>(
        0, nowMilliseconds - persisted.capturedAtUnixMilliseconds);
    const std::int64_t maximumAge =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::minutes(m_config.resume.maxAgeMinutes)).count();
    if (ageMilliseconds > maximumAge)
    {
        m_resume.status = "stale";
        return true;
    }

    m_pendingPlayback = persisted.playback;
    m_pendingCapturedAtUnixMilliseconds = persisted.capturedAtUnixMilliseconds;
    if (!PendingMatchesCurrentSchedule(systemNow))
    {
        m_pendingPlayback.reset();
        m_pendingCapturedAtUnixMilliseconds = 0;
        m_resume.status = "schedule_changed";
        return true;
    }

    m_resume.pending = true;
    m_resume.automaticEligible = m_config.resume.automatic;
    m_resume.playlist = persisted.playback.playlistName;
    m_resume.track = persisted.playback.trackId;
    m_resume.positionMilliseconds = persisted.playback.positionMs;
    m_resume.status = "pending";
    m_automationSuspended = true;
    return true;
}

void CinemaManager::Tick(
    SystemClock::time_point systemNow,
    SteadyClock::time_point steadyNow)
{
    if (!m_initialized || m_shutdown) return;

    (void)m_safety.Tick(steadyNow);
    if (m_config.safety.enabled && m_config.safety.interlock.enabled &&
        m_safety.GetMode() == SafetyMode::Inhibited)
    {
        const SafetySnapshot snapshot = m_safety.GetSnapshot(steadyNow);
        const auto startupGrace = std::chrono::milliseconds(
            m_config.safety.interlock.startupGraceMilliseconds);
        if (!snapshot.hasFreshHeartbeat && steadyNow >= m_startedAt &&
            steadyNow - m_startedAt >= startupGrace)
        {
            (void)m_safety.Trip("heartbeat_startup_timeout", {});
        }
    }

    ApplyContainment(systemNow, steadyNow);
    if (CanPlayNormalAudio())
    {
        m_everReady = true;
        if (m_pendingPlayback && m_resume.automaticEligible &&
            !m_automaticResumeAttempted)
        {
            const CinemaResult automatic =
                ResumePendingInternal(systemNow, steadyNow, true);
            if (!automatic.ok)
                spdlog::warn("Automatic cinema resume was refused: {}", automatic.message);
        }
        if (!m_automationSuspended) UpdateSchedule(systemNow);
    }

    if (m_stateEnabled && steadyNow >= m_nextCheckpoint)
    {
        if (!PersistCheckpoint(false, systemNow))
        {
            SetOperationalInhibit("playback_state_write_failed", m_persistenceError);
            ApplyContainment(systemNow, steadyNow);
        }
        m_nextCheckpoint = steadyNow +
            std::chrono::seconds(m_config.resume.checkpointIntervalSeconds);
    }
}

void CinemaManager::ApplyContainment(
    SystemClock::time_point systemNow,
    SteadyClock::time_point steadyNow)
{
    const SafetySnapshot snapshot = m_safety.GetSnapshot(steadyNow);
    const bool ready = snapshot.safeToPlay && !m_operationalInhibit;
    if (!ready)
    {
        const PlaybackState playback = PlaylistManager::GetInstance().CapturePlaybackState();
        if (playback.isPlaying && !m_pendingPlayback)
            CaptureInterruptedPlayback(systemNow);

        if ((m_previousSafetyMode == SafetyMode::Ready && m_everReady) ||
            snapshot.mode == SafetyMode::Latched || m_operationalInhibit)
            m_automationSuspended = true;

        auto& audio = AudioManager::GetInstance();
        audio.SetNormalPlaybackBlocked(true);
        PlaylistManager::GetInstance().AbortImmediately();
        AnnouncementManager::GetInstance().StopAnnouncement();
        audio.StopAllNonEmergencyImmediately();
        EnsureEmergencyState(steadyNow);
    }
    else
    {
        AudioManager::GetInstance().SetNormalPlaybackBlocked(false);
        StopEmergency();
    }
    m_previousSafetyMode = snapshot.mode;
}

void CinemaManager::CaptureInterruptedPlayback(SystemClock::time_point systemNow)
{
    if (!m_config.resume.enabled) return;
    const PlaybackState playback = PlaylistManager::GetInstance().CapturePlaybackState();
    if (!playback.isPlaying) return;
    m_pendingPlayback = playback;
    m_pendingCapturedAtUnixMilliseconds = UnixMilliseconds(systemNow);
    m_resume.pending = true;
    m_resume.automaticEligible = false;
    m_resume.previousShutdownClean = false;
    m_resume.capturedAtUnixMilliseconds = m_pendingCapturedAtUnixMilliseconds;
    m_resume.playlist = playback.playlistName;
    m_resume.track = playback.trackId;
    m_resume.positionMilliseconds = playback.positionMs;
    m_resume.status = "interrupted";
    if (m_stateEnabled && !PersistCheckpoint(false, systemNow))
        m_persistenceError = m_persistenceError.empty()
            ? "Unable to persist interrupted playback." : m_persistenceError;
}

void CinemaManager::UpdateSchedule(SystemClock::time_point systemNow)
{
    if (!m_config.enabled) return;
    const CinemaScheduleResolution resolution = ResolveCinemaSchedule(
        m_config.schedules, GetLocalCinemaTime(systemNow));
    m_selectedScheduleId = resolution.selected ? resolution.selected->id : std::string{};
    m_selectedPlaylist = resolution.selected ? resolution.selected->playlist : std::string{};

    auto& playlists = PlaylistManager::GetInstance();
    if (!resolution.selected)
    {
        const bool stoppedOwnedPlayback =
            m_automationOwnsPlayback && !m_activePlaylist.empty();
        if (m_automationOwnsPlayback && !m_activePlaylist.empty())
            playlists.Stop(m_activePlaylist);
        m_automationOwnsPlayback = false;
        m_activeScheduleId.clear();
        m_activePlaylist.clear();
        if (stoppedOwnedPlayback && !PersistCheckpoint(false, systemNow))
            SetOperationalInhibit(
                "playback_state_write_failed", m_persistenceError);
        return;
    }

    if (m_automationOwnsPlayback && m_activePlaylist == resolution.selected->playlist &&
        playlists.IsPlaylistPlaying(m_activePlaylist))
    {
        // A different schedule selecting the same playlist does not restart it;
        // this also makes repeated civil hours during the DST fold idempotent.
        m_activeScheduleId = resolution.selected->id;
        return;
    }

    const bool selectedPlaylistIsPlaying =
        playlists.IsPlaylistPlaying(resolution.selected->playlist);
    const auto* currentSource = playlists.GetActivePlaylist();
    const bool currentSourceBelongsToPreviousSchedule =
        currentSource && m_automationOwnsPlayback && !m_activePlaylist.empty() &&
        currentSource->name == m_activePlaylist;
    if (!selectedPlaylistIsPlaying && currentSource &&
        !currentSourceBelongsToPreviousSchedule)
    {
        if (m_automationOwnsPlayback)
        {
            spdlog::info(
                "Cinema calendar yielded to manual programme source '{}'.",
                currentSource->name);
        }
        m_automationOwnsPlayback = false;
        m_activeScheduleId.clear();
        m_activePlaylist.clear();
        return;
    }

    if (m_automationOwnsPlayback && !m_activePlaylist.empty() &&
        m_activePlaylist != resolution.selected->playlist)
        playlists.Stop(m_activePlaylist);

    const auto* playlist = playlists.GetPlaylistByName(resolution.selected->playlist);
    if (!playlist)
    {
        SetOperationalInhibit("scheduled_playlist_missing");
        return;
    }
    if (!playlists.IsPlaylistPlaying(resolution.selected->playlist))
        playlists.Play(resolution.selected->playlist, playlist->options);
    if (!playlists.IsPlaylistPlaying(resolution.selected->playlist))
    {
        SetOperationalInhibit("scheduled_playlist_failed");
        return;
    }

    m_automationOwnsPlayback = true;
    m_activeScheduleId = resolution.selected->id;
    m_activePlaylist = resolution.selected->playlist;
    if (!PersistCheckpoint(false, systemNow))
        SetOperationalInhibit("playback_state_write_failed", m_persistenceError);
}

bool CinemaManager::PersistCheckpoint(
    bool cleanShutdown,
    SystemClock::time_point systemNow)
{
    if (!m_stateEnabled) return true;
    PlaybackState playback;
    std::int64_t capturedAt = UnixMilliseconds(systemNow);
    if (m_pendingPlayback)
    {
        playback = *m_pendingPlayback;
        capturedAt = m_pendingCapturedAtUnixMilliseconds;
    }
    else
    {
        playback = PlaylistManager::GetInstance().CapturePlaybackState();
    }

    const std::uint64_t nextGeneration = m_generation + 1;
    const nlohmann::json document = SerializeCheckpoint(
        nextGeneration, cleanShutdown, capturedAt, m_configFingerprint, playback);
    std::string writeError;
    if (!WriteJsonFileAtomically(m_playbackStatePath, document, writeError))
    {
        m_persistenceError = writeError;
        return false;
    }
    m_generation = nextGeneration;
    m_persistenceError.clear();
    return true;
}

CinemaResult CinemaManager::ObserveHeartbeat(
    const SafetyHeartbeat& heartbeat,
    SteadyClock::time_point steadyNow,
    SystemClock::time_point systemNow)
{
    if (!m_initialized)
        return CinemaResult::Failure("not_initialized", "Cinema manager is not initialized.");
    const SafetyResult observed = m_safety.ObserveHeartbeat(heartbeat, steadyNow);
    if (observed.ok) (void)m_safety.Tick(steadyNow);
    ApplyContainment(systemNow, steadyNow);
    if (!observed.ok)
        return CinemaResult::Failure(observed.code, observed.message);
    return CinemaResult::Success(observed.code, observed.message);
}

CinemaResult CinemaManager::Trip(
    const std::string& cause,
    const std::string& source,
    bool playEvacuation,
    SystemClock::time_point systemNow,
    SteadyClock::time_point steadyNow)
{
    if (!m_initialized)
        return CinemaResult::Failure("not_initialized", "Cinema manager is not initialized.");
    SafetyResult tripped = m_safety.Trip(cause, source);
    if (tripped.ok && playEvacuation)
    {
        const SafetyResult evacuation = m_safety.Trip("evacuation_requested", source);
        if (!evacuation.ok) tripped = evacuation;
    }
    ApplyContainment(systemNow, steadyNow);
    if (!tripped.ok) return CinemaResult::Failure(tripped.code, tripped.message);
    return CinemaResult::Success(tripped.code, tripped.message);
}

CinemaResult CinemaManager::Reset(
    const std::string& incidentId,
    const std::string& operatorId,
    const std::string& reason,
    SteadyClock::time_point steadyNow,
    SystemClock::time_point systemNow)
{
    if (!m_initialized)
        return CinemaResult::Failure("not_initialized", "Cinema manager is not initialized.");
    const SafetyResult reset = m_safety.Reset(
        incidentId, operatorId, reason, steadyNow);
    if (!reset.ok) return CinemaResult::Failure(reset.code, reset.message);

    // A reset only re-arms the safety chain. It never restarts programme audio.
    m_automationSuspended = true;
    m_automaticResumeAttempted = true;
    StopEmergency();
    ApplyContainment(systemNow, steadyNow);
    if (m_stateEnabled && !PersistCheckpoint(false, systemNow))
    {
        SetOperationalInhibit("playback_state_write_failed", m_persistenceError);
        ApplyContainment(systemNow, steadyNow);
        return CinemaResult::Failure(
            "state_persistence_failed", m_persistenceError);
    }
    return CinemaResult::Success("reset", "Safety incident reset; playback remains suspended.");
}

CinemaResult CinemaManager::ResumePending(
    SystemClock::time_point systemNow,
    SteadyClock::time_point steadyNow)
{
    return ResumePendingInternal(systemNow, steadyNow, false);
}

CinemaResult CinemaManager::ResumePendingInternal(
    SystemClock::time_point systemNow,
    SteadyClock::time_point steadyNow,
    bool automatic)
{
    if (!m_initialized)
        return CinemaResult::Failure("not_initialized", "Cinema manager is not initialized.");
    if (automatic) m_automaticResumeAttempted = true;
    if (!CanPlayNormalAudio())
        return CinemaResult::Failure(
            "playback_inhibited", "Safety and operational gates are not ready.");

    if (m_pendingPlayback)
    {
        if (!PendingMatchesCurrentSchedule(systemNow))
        {
            m_resume.status = "schedule_changed";
            return CinemaResult::Failure(
                "schedule_changed",
                "The active cinema schedule no longer matches the interrupted playlist.");
        }
        std::string resumeError;
        if (!PlaylistManager::GetInstance().ResumePlaybackState(
                *m_pendingPlayback, resumeError))
        {
            m_resume.status = "resume_failed";
            return CinemaResult::Failure("resume_failed", resumeError);
        }

        const CinemaScheduleResolution selected = ResolveCinemaSchedule(
            m_config.schedules, GetLocalCinemaTime(systemNow));
        m_activeScheduleId = selected.selected ? selected.selected->id : std::string{};
        m_activePlaylist = m_pendingPlayback->playlistName;
        m_automationOwnsPlayback = true;
        m_pendingPlayback.reset();
        m_pendingCapturedAtUnixMilliseconds = 0;
        m_resume.pending = false;
        m_resume.automaticEligible = false;
        m_resume.status = automatic ? "resumed_automatically" : "resumed_by_operator";
    }

    m_automationSuspended = false;
    UpdateSchedule(systemNow);
    if (m_stateEnabled && !PersistCheckpoint(false, systemNow))
    {
        SetOperationalInhibit("playback_state_write_failed", m_persistenceError);
        ApplyContainment(systemNow, steadyNow);
        return CinemaResult::Failure("state_persistence_failed", m_persistenceError);
    }
    return CinemaResult::Success(
        automatic ? "resumed_automatically" : "resumed",
        m_resume.status);
}

CinemaResult CinemaManager::DiscardPending(SystemClock::time_point systemNow)
{
    if (!m_initialized)
        return CinemaResult::Failure("not_initialized", "Cinema manager is not initialized.");
    m_pendingPlayback.reset();
    m_pendingCapturedAtUnixMilliseconds = 0;
    m_resume.pending = false;
    m_resume.automaticEligible = false;
    m_resume.playlist.clear();
    m_resume.track.clear();
    m_resume.positionMilliseconds = 0;
    m_resume.status = "discarded";

    if (m_stateEnabled && !PersistCheckpoint(false, systemNow))
    {
        SetOperationalInhibit("playback_state_write_failed", m_persistenceError);
        return CinemaResult::Failure("state_persistence_failed", m_persistenceError);
    }
    if (m_operationalInhibitReason == "playback_state_invalid" ||
        m_operationalInhibitReason == "playback_state_write_failed")
        ClearOperationalInhibit();
    return CinemaResult::Success(
        "discarded", "Interrupted playback was discarded; automation remains suspended.");
}

void CinemaManager::SetOperationalInhibit(
    std::string reason,
    std::string persistenceError)
{
    m_operationalInhibit = true;
    m_operationalInhibitReason = std::move(reason);
    if (!persistenceError.empty()) m_persistenceError = std::move(persistenceError);
    m_automationSuspended = true;
    if (m_config.safety.enabled)
        (void)m_safety.Trip("operational_inhibit", {});
    auto& audio = AudioManager::GetInstance();
    audio.SetNormalPlaybackBlocked(true);
    PlaylistManager::GetInstance().AbortImmediately();
    AnnouncementManager::GetInstance().StopAnnouncement();
    audio.StopAllNonEmergencyImmediately();
}

void CinemaManager::ClearOperationalInhibit()
{
    m_operationalInhibit = false;
    m_operationalInhibitReason.clear();
    m_persistenceError.clear();
}

bool CinemaManager::IncidentRequestsEvacuation(
    SteadyClock::time_point steadyNow) const
{
    const SafetySnapshot snapshot = m_safety.GetSnapshot(steadyNow);
    if (snapshot.mode != SafetyMode::Latched || !snapshot.incident) return false;
    return std::any_of(
        snapshot.incident->causes.begin(), snapshot.incident->causes.end(),
        [](const std::string& cause) {
            return CauseStartsWith(cause, "fire_alarm") ||
                CauseStartsWith(cause, "evacuation_requested");
        });
}

void CinemaManager::EnsureEmergencyState(SteadyClock::time_point steadyNow)
{
    if (!IncidentRequestsEvacuation(steadyNow))
    {
        StopEmergency();
        return;
    }

    auto& audio = AudioManager::GetInstance();
    if (m_emergencyPlaying &&
        audio.GetLastChannelOfSound(m_config.safety.evacuationAnnouncementId))
        return;

    // FMOD channels can be lost independently of the safety latch. Re-check
    // liveness on every containment tick so an active evacuation request is
    // reasserted instead of trusting a stale bookkeeping flag.
    m_emergencyPlaying = false;
    m_emergencyPlaying = audio.PlaySound(
        m_config.safety.evacuationAnnouncementId, true, 1.0f, 1.0f) != nullptr;
    if (!m_emergencyPlaying)
        spdlog::critical("Unable to play the configured emergency evacuation preset.");
}

void CinemaManager::StopEmergency()
{
    if (!m_config.safety.evacuationAnnouncementId.empty())
        AudioManager::GetInstance().StopSound(
            m_config.safety.evacuationAnnouncementId);
    m_emergencyPlaying = false;
}

bool CinemaManager::PendingMatchesCurrentSchedule(
    SystemClock::time_point systemNow) const
{
    if (!m_pendingPlayback || !m_config.enabled) return false;
    const CinemaScheduleResolution resolution = ResolveCinemaSchedule(
        m_config.schedules, GetLocalCinemaTime(systemNow));
    return resolution.selected &&
        resolution.selected->playlist == m_pendingPlayback->playlistName;
}

CinemaStatus CinemaManager::GetStatus(
    SystemClock::time_point systemNow,
    SteadyClock::time_point steadyNow) const
{
    CinemaStatus status;
    status.enabled = m_config.enabled;
    status.safetyEnabled = m_config.safety.enabled;
    status.interlockEnabled = m_config.safety.interlock.enabled;
    status.safeToPlay = CanPlayNormalAudio();
    status.automationSuspended = m_automationSuspended;
    status.emergencyPlaying = m_emergencyPlaying;
    status.operationalInhibit = m_operationalInhibit;
    status.operationalInhibitReason = m_operationalInhibitReason;
    status.activeScheduleId = m_activeScheduleId;
    status.activePlaylist = m_activePlaylist;
    const CinemaScheduleResolution resolution = ResolveCinemaSchedule(
        m_config.schedules, GetLocalCinemaTime(systemNow));
    status.selectedScheduleId =
        resolution.selected ? resolution.selected->id : std::string{};
    status.selectedPlaylist =
        resolution.selected ? resolution.selected->playlist : std::string{};
    status.persistenceError = m_persistenceError;
    status.resume = m_resume;
    (void)steadyNow;
    return status;
}

SafetySnapshot CinemaManager::GetSafetySnapshot(
    SteadyClock::time_point steadyNow) const
{
    return m_safety.GetSnapshot(steadyNow);
}

CinemaScheduleResolution CinemaManager::Preview(
    const CinemaCivilTime& localTime) const
{
    return ResolveCinemaSchedule(m_config.schedules, localTime);
}

bool CinemaManager::CanPlayNormalAudio() const
{
    return m_initialized && !m_shutdown && !m_operationalInhibit &&
        m_safety.CanPlayNormalAudio();
}

bool CinemaManager::IsReloadAllowed() const
{
    return !m_initialized ||
        (m_safety.GetMode() != SafetyMode::Latched && !m_emergencyPlaying);
}

void CinemaManager::Shutdown(SystemClock::time_point systemNow)
{
    if (!m_initialized || m_shutdown) return;
    if (m_stateEnabled && !PersistCheckpoint(true, systemNow))
        spdlog::error("Unable to persist clean cinema shutdown: {}", m_persistenceError);
    StopEmergency();
    AudioManager::GetInstance().SetNormalPlaybackBlocked(false);
    m_shutdown = true;
    m_initialized = false;
}

} // namespace TSM
