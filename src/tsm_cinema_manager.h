#pragma once

#include "tsm_cinema_schedule.h"
#include "tsm_config.h"
#include "tsm_playlist_manager.h"
#include "tsm_safety.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace TSM
{

struct CinemaResult
{
    bool ok = false;
    std::string code;
    std::string message;

    static CinemaResult Success(std::string code = "ok", std::string message = {});
    static CinemaResult Failure(std::string code, std::string message);
};

struct CinemaResumeSnapshot
{
    bool pending = false;
    bool automaticEligible = false;
    bool previousShutdownClean = true;
    std::int64_t capturedAtUnixMilliseconds = 0;
    std::string playlist;
    std::string track;
    std::uint32_t positionMilliseconds = 0;
    std::string status = "none";
};

struct CinemaStatus
{
    bool enabled = false;
    bool safetyEnabled = false;
    bool interlockEnabled = false;
    bool safeToPlay = true;
    bool automationSuspended = false;
    bool emergencyPlaying = false;
    bool operationalInhibit = false;
    std::string operationalInhibitReason;
    std::string activeScheduleId;
    std::string activePlaylist;
    std::string selectedScheduleId;
    std::string selectedPlaylist;
    std::string persistenceError;
    CinemaResumeSnapshot resume;
};

class CinemaManager
{
public:
    using SystemClock = std::chrono::system_clock;
    using SteadyClock = std::chrono::steady_clock;

    CinemaManager() = default;
    ~CinemaManager();

    CinemaManager(const CinemaManager&) = delete;
    CinemaManager& operator=(const CinemaManager&) = delete;

    bool Initialize(
        CinemaConfig config,
        std::filesystem::path stateDirectory,
        std::string configFingerprint,
        std::string& errorMessage,
        SystemClock::time_point systemNow = SystemClock::now(),
        SteadyClock::time_point steadyNow = SteadyClock::now());
    void Tick(
        SystemClock::time_point systemNow = SystemClock::now(),
        SteadyClock::time_point steadyNow = SteadyClock::now());
    void Shutdown(
        SystemClock::time_point systemNow = SystemClock::now());

    CinemaResult ObserveHeartbeat(
        const SafetyHeartbeat& heartbeat,
        SteadyClock::time_point steadyNow = SteadyClock::now(),
        SystemClock::time_point systemNow = SystemClock::now());
    CinemaResult Trip(
        const std::string& cause,
        const std::string& source,
        bool playEvacuation,
        SystemClock::time_point systemNow = SystemClock::now(),
        SteadyClock::time_point steadyNow = SteadyClock::now());
    CinemaResult Reset(
        const std::string& incidentId,
        const std::string& operatorId,
        const std::string& reason,
        SteadyClock::time_point steadyNow = SteadyClock::now(),
        SystemClock::time_point systemNow = SystemClock::now());
    CinemaResult ResumePending(
        SystemClock::time_point systemNow = SystemClock::now(),
        SteadyClock::time_point steadyNow = SteadyClock::now());
    CinemaResult DiscardPending(
        SystemClock::time_point systemNow = SystemClock::now());

    CinemaStatus GetStatus(
        SystemClock::time_point systemNow = SystemClock::now(),
        SteadyClock::time_point steadyNow = SteadyClock::now()) const;
    SafetySnapshot GetSafetySnapshot(
        SteadyClock::time_point steadyNow = SteadyClock::now()) const;
    CinemaScheduleResolution Preview(const CinemaCivilTime& localTime) const;
    const std::vector<CinemaSchedule>& GetSchedules() const { return m_config.schedules; }

    bool IsInitialized() const { return m_initialized; }
    bool CanPlayNormalAudio() const;
    bool IsReloadAllowed() const;

private:
    void ApplyContainment(
        SystemClock::time_point systemNow,
        SteadyClock::time_point steadyNow);
    void UpdateSchedule(SystemClock::time_point systemNow);
    bool PersistCheckpoint(bool cleanShutdown, SystemClock::time_point systemNow);
    bool LoadPreviousCheckpoint(
        SystemClock::time_point systemNow,
        std::string& errorMessage);
    void CaptureInterruptedPlayback(SystemClock::time_point systemNow);
    void SetOperationalInhibit(
        std::string reason,
        std::string persistenceError = {});
    void ClearOperationalInhibit();
    void EnsureEmergencyState(SteadyClock::time_point steadyNow);
    void StopEmergency();
    bool IncidentRequestsEvacuation(SteadyClock::time_point steadyNow) const;
    bool PendingMatchesCurrentSchedule(SystemClock::time_point systemNow) const;
    CinemaResult ResumePendingInternal(
        SystemClock::time_point systemNow,
        SteadyClock::time_point steadyNow,
        bool automatic);

    CinemaConfig m_config;
    SafetyController m_safety;
    std::filesystem::path m_playbackStatePath;
    std::string m_configFingerprint;
    bool m_initialized = false;
    bool m_stateEnabled = false;
    bool m_shutdown = false;
    bool m_operationalInhibit = false;
    std::string m_operationalInhibitReason;
    std::string m_persistenceError;
    bool m_automationSuspended = false;
    bool m_everReady = false;
    bool m_emergencyPlaying = false;
    bool m_automationOwnsPlayback = false;
    bool m_automaticResumeAttempted = false;
    std::string m_activeScheduleId;
    std::string m_activePlaylist;
    std::string m_selectedScheduleId;
    std::string m_selectedPlaylist;
    CinemaResumeSnapshot m_resume;
    std::optional<PlaybackState> m_pendingPlayback;
    std::int64_t m_pendingCapturedAtUnixMilliseconds = 0;
    std::uint64_t m_generation = 0;
    SteadyClock::time_point m_startedAt{};
    SteadyClock::time_point m_nextCheckpoint{};
    SafetyMode m_previousSafetyMode = SafetyMode::Inhibited;
};

} // namespace TSM
