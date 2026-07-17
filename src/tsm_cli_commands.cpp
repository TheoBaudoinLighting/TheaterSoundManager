#include "tsm_cli.h"

#include "tsm_announcement_manager.h"
#include "tsm_audio_manager.h"
#include "tsm_config.h"
#include "tsm_fmod_wrapper.h"
#include "tsm_mixer.h"
#include "tsm_playlist_manager.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace TSM
{
namespace
{

#ifndef TSM_VERSION
#define TSM_VERSION "dev"
#endif

class CommandError final : public std::runtime_error
{
public:
    CommandError(
        CliExitCode exitCode,
        std::string code,
        std::string message,
        nlohmann::json details = nlohmann::json::object())
        : std::runtime_error(message),
          exitCode(exitCode),
          code(std::move(code)),
          details(std::move(details))
    {
    }

    CliExitCode exitCode;
    std::string code;
    nlohmann::json details;
};

class Parameters
{
public:
    explicit Parameters(const nlohmann::json& value) : m_value(value)
    {
        if (!m_value.is_object())
            throw CommandError(
                CliExitCode::Usage, "invalid_params", "Command parameters must be an object.");
    }

    void Allow(std::initializer_list<std::string_view> names) const
    {
        std::set<std::string_view> allowed(names);
        for (const auto& item : m_value.items())
        {
            if (!allowed.contains(item.key()))
            {
                throw CommandError(
                    CliExitCode::Usage,
                    "unknown_parameter",
                    "Unknown parameter '" + item.key() + "'.",
                    {{"parameter", item.key()}});
            }
        }
    }

    bool Has(std::string_view name) const
    {
        return m_value.contains(std::string(name));
    }

    std::string String(
        std::string_view name,
        bool required = true,
        std::string defaultValue = {}) const
    {
        const std::string key(name);
        if (!m_value.contains(key))
        {
            if (required)
                throw Missing(name);
            return defaultValue;
        }
        if (!m_value[key].is_string() || m_value[key].get_ref<const std::string&>().empty())
        {
            throw Invalid(name, "must be a non-empty string");
        }
        return m_value[key].get<std::string>();
    }

    bool Boolean(std::string_view name, bool defaultValue) const
    {
        const std::string key(name);
        if (!m_value.contains(key)) return defaultValue;
        const auto& value = m_value[key];
        if (value.is_boolean()) return value.get<bool>();
        if (value.is_string())
        {
            std::string text = value.get<std::string>();
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            if (text == "true" || text == "1" || text == "yes" || text == "on") return true;
            if (text == "false" || text == "0" || text == "no" || text == "off") return false;
        }
        throw Invalid(name, "must be a boolean");
    }

    double Number(
        std::string_view name,
        bool required = true,
        double defaultValue = 0.0) const
    {
        const std::string key(name);
        if (!m_value.contains(key))
        {
            if (required) throw Missing(name);
            return defaultValue;
        }

        double result = 0.0;
        const auto& value = m_value[key];
        if (value.is_number())
        {
            result = value.get<double>();
        }
        else if (value.is_string())
        {
            const std::string text = value.get<std::string>();
            try
            {
                std::size_t consumed = 0;
                result = std::stod(text, &consumed);
                if (consumed != text.size()) throw std::invalid_argument("trailing data");
            }
            catch (...)
            {
                throw Invalid(name, "must be a number");
            }
        }
        else
        {
            throw Invalid(name, "must be a number");
        }
        if (!std::isfinite(result)) throw Invalid(name, "must be finite");
        return result;
    }

    int Integer(std::string_view name, bool required = true, int defaultValue = 0) const
    {
        const std::string key(name);
        if (!m_value.contains(key))
        {
            if (required) throw Missing(name);
            return defaultValue;
        }
        const auto& value = m_value[key];
        if (value.is_number_unsigned())
        {
            const std::uint64_t number = value.get<std::uint64_t>();
            if (number <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                return static_cast<int>(number);
            throw Invalid(name, "must fit in a signed 32-bit integer");
        }
        if (value.is_number_integer())
        {
            const std::int64_t number = value.get<std::int64_t>();
            if (number >= std::numeric_limits<int>::min() &&
                number <= std::numeric_limits<int>::max())
                return static_cast<int>(number);
            throw Invalid(name, "must fit in a signed 32-bit integer");
        }
        if (value.is_string())
        {
            const std::string text = value.get<std::string>();
            int result = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
            if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) return result;
        }
        throw Invalid(name, "must be an integer");
    }

    std::uint64_t Unsigned64(std::string_view name) const
    {
        const std::string key(name);
        if (!m_value.contains(key)) throw Missing(name);
        const auto& value = m_value[key];
        if (value.is_number_unsigned()) return value.get<std::uint64_t>();
        if (value.is_number_integer())
        {
            const auto number = value.get<std::int64_t>();
            if (number >= 0) return static_cast<std::uint64_t>(number);
        }
        if (value.is_string())
        {
            const std::string text = value.get<std::string>();
            std::uint64_t result = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
            if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) return result;
        }
        throw Invalid(name, "must be a non-negative integer");
    }

private:
    static CommandError Missing(std::string_view name)
    {
        return CommandError(
            CliExitCode::Usage,
            "missing_parameter",
            "Required parameter '" + std::string(name) + "' is missing.",
            {{"parameter", name}});
    }

    static CommandError Invalid(std::string_view name, std::string_view expectation)
    {
        return CommandError(
            CliExitCode::InvalidArgument,
            "invalid_parameter",
            "Parameter '" + std::string(name) + "' " + std::string(expectation) + ".",
            {{"parameter", name}, {"expectation", expectation}});
    }

    const nlohmann::json& m_value;
};

void RequireRuntime(const ApplicationRuntime& runtime)
{
    if (!runtime.IsInitialized() || !FModWrapper::GetInstance().IsInitialized())
    {
        throw CommandError(
            CliExitCode::AudioEngine,
            "runtime_unavailable",
            "The audio runtime is not initialized.");
    }
}

CinemaManager& RequireCinemaManager(ApplicationRuntime& runtime)
{
    RequireRuntime(runtime);
    CinemaManager* manager = runtime.GetCinemaManager();
    if (!manager)
    {
        throw CommandError(
            CliExitCode::Conflict,
            "cinema_not_configured",
            "Cinema operation requires a loaded configuration.");
    }
    return *manager;
}

void RequireNormalPlaybackAllowed(const ApplicationRuntime& runtime)
{
    if (runtime.CanPlayNormalAudio()) return;
    throw CommandError(
        CliExitCode::Conflict,
        "playback_inhibited",
        "Normal playback is blocked by the cinema safety or operational gate.");
}

void RejectProtectedEmergencySound(
    const AudioManager::SoundData& sound, const std::string& id)
{
    if (sound.kind != AudioManager::SoundKind::Emergency) return;
    throw CommandError(
        CliExitCode::Conflict,
        "protected_emergency_preset",
        "Emergency preset '" + id +
            "' is controlled exclusively by the cinema safety commands.");
}

std::filesystem::path PathFromUtf8(const std::string& value)
{
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string ResolvePath(const std::string& value, const std::string& relativeRoot = {})
{
    std::filesystem::path path = PathFromUtf8(value);
    if (path.is_relative())
    {
        path = relativeRoot.empty()
            ? std::filesystem::current_path() / path
            : PathFromUtf8(relativeRoot) / path;
    }
    return PathToUtf8(path.lexically_normal());
}

AudioManager::SoundKind ParseSoundKind(const std::string& value)
{
    if (value == "sfx" || value == "sound-effect") return AudioManager::SoundKind::SoundEffect;
    if (value == "music") return AudioManager::SoundKind::Music;
    if (value == "announcement") return AudioManager::SoundKind::Announcement;
    throw CommandError(
        CliExitCode::InvalidArgument,
        "invalid_sound_kind",
        "Sound kind must be one of: sfx, sound-effect, music, announcement.",
        {{"kind", value}});
}

double JsonFloat(float value)
{
    return std::round(static_cast<double>(value) * 1'000'000.0) / 1'000'000.0;
}

nlohmann::json MixerToJson()
{
    const auto& mixer = MixerState::GetInstance();
    return {
        {"master", JsonFloat(mixer.GetMasterVolume())},
        {"music", JsonFloat(mixer.GetMusicVolume())},
        {"announcement", JsonFloat(mixer.GetAnnouncementVolume())},
        {"sfx", JsonFloat(mixer.GetSfxVolume())},
        {"duck", JsonFloat(mixer.GetDuckFactor())},
        {"effectiveDuck", JsonFloat(mixer.GetEffectiveDuckFactor())}
    };
}

nlohmann::json ChannelToJson(FMOD::Channel* channel)
{
    nlohmann::json result = nlohmann::json::object();
    bool playing = false;
    bool paused = false;
    float volume = 0.0f;
    float pitch = 1.0f;
    unsigned int position = 0;
    if (!channel || channel->isPlaying(&playing) != FMOD_OK)
    {
        result["valid"] = false;
        return result;
    }
    channel->getPaused(&paused);
    channel->getVolume(&volume);
    channel->getPitch(&pitch);
    channel->getPosition(&position, FMOD_TIMEUNIT_MS);
    result = {
        {"valid", true},
        {"playing", playing},
        {"paused", paused},
        {"volume", JsonFloat(volume)},
        {"pitch", JsonFloat(pitch)},
        {"positionMs", position}
    };
    return result;
}

nlohmann::json SoundToJson(
    const std::string& id, const AudioManager::SoundData& sound, bool includeChannels = true)
{
    nlohmann::json channels = nlohmann::json::array();
    std::size_t playingChannels = 0;
    if (includeChannels)
    {
        for (FMOD::Channel* channel : sound.channels)
        {
            nlohmann::json channelJson = ChannelToJson(channel);
            if (channelJson.value("playing", false)) ++playingChannels;
            channels.push_back(std::move(channelJson));
        }
    }
    else
    {
        for (FMOD::Channel* channel : sound.channels)
        {
            bool playing = false;
            if (channel && channel->isPlaying(&playing) == FMOD_OK && playing) ++playingChannels;
        }
    }

    nlohmann::json result = {
        {"id", id},
        {"path", sound.filePath},
        {"kind", AudioManager::SoundKindToString(sound.kind)},
        {"playingChannels", playingChannels},
        {"loudness", {
            {"status", AudioManager::LoudnessStatusToString(sound.loudnessStatus)},
            {"integratedLufs", JsonFloat(sound.integratedLufs)},
            {"truePeakDb", JsonFloat(sound.truePeakDb)},
            {"gainDb", JsonFloat(sound.normalizationGainDb)},
            {"musicAnalysisReady", sound.musicAnalysisReady}
        }}
    };
    if (includeChannels) result["channels"] = std::move(channels);
    return result;
}

nlohmann::json SegmentDecisionToJson(const SegmentDecisionInfo& decision)
{
    if (!decision.active) return nullptr;
    return {
        {"mode", decision.mode},
        {"fullTrack", decision.fullTrack},
        {"startSeconds", JsonFloat(decision.startSeconds)},
        {"endSeconds", JsonFloat(decision.endSeconds)},
        {"durationSeconds", JsonFloat(decision.durationSeconds)},
        {"entryScore", JsonFloat(decision.entryScore)},
        {"exitScore", JsonFloat(decision.exitScore)},
        {"transitionScore", JsonFloat(decision.transitionScore)},
        {"explorationScore", JsonFloat(decision.explorationScore)},
        {"transitionDiversityScore",
         JsonFloat(decision.transitionDiversityScore)},
        {"totalScore", JsonFloat(decision.totalScore)},
        {"reasons", decision.reasons}
    };
}

template <typename PlaylistType>
nlohmann::json PlaylistToJson(const PlaylistType& playlist)
{
    return {
        {"name", playlist.name},
        {"tracks", playlist.tracks},
        {"trackCount", playlist.tracks.size()},
        {"playing", playlist.isPlaying},
        {"currentIndex", playlist.currentIndex},
        {"nextIndex", playlist.nextIndex},
        {"options", {
            {"randomOrder", playlist.options.randomOrder},
            {"randomSegment", playlist.options.randomSegment},
            {"segmentDuration", JsonFloat(playlist.options.segmentDuration)},
            {"automaticSegmentDuration",
             playlist.options.automaticSegmentDuration},
            {"minSegmentDuration", JsonFloat(playlist.options.minSegmentDuration)},
            {"maxSegmentDuration", JsonFloat(playlist.options.maxSegmentDuration)},
            {"loop", playlist.options.loopPlaylist},
            {"crossfadeDuration", JsonFloat(playlist.crossfadeDuration)}
        }}
    };
}

nlohmann::json MusicLibraryToJson(
    const PlaylistManager& manager, bool includeTracks = true)
{
    const std::vector<std::string> availableTracks = manager.GetLibraryMusicIds();
    const std::vector<std::string>& tracks = manager.IsLibraryPlaying()
        ? manager.GetLibraryPlaybackSnapshot()
        : availableTracks;
    const PlaylistOptions options = manager.GetLibraryOptions();
    nlohmann::json result = {
        {"source", "music_library"},
        {"playing", manager.IsLibraryPlaying()},
        {"trackCount", tracks.size()},
        {"availableTrackCount", availableTracks.size()},
        {"currentTrack", manager.IsLibraryPlaying()
            ? nlohmann::json(manager.GetCurrentTrackName()) : nlohmann::json(nullptr)},
        {"nextTrack", manager.IsLibraryPlaying()
            ? nlohmann::json(manager.GetNextTrackName()) : nlohmann::json(nullptr)},
        {"options", {
            {"randomOrder", options.randomOrder},
            {"randomSegment", options.randomSegment},
            {"segmentDuration", JsonFloat(options.segmentDuration)},
            {"automaticSegmentDuration", options.automaticSegmentDuration},
            {"minSegmentDuration", JsonFloat(options.minSegmentDuration)},
            {"maxSegmentDuration", JsonFloat(options.maxSegmentDuration)},
            {"loop", options.loopPlaylist},
            {"crossfadeDuration", JsonFloat(manager.GetLibraryCrossfadeDuration())}
        }}
    };
    if (includeTracks) result["tracks"] = tracks;
    if (manager.IsLibraryPlaying())
    {
        result["trackProgress"] = JsonFloat(manager.GetTrackProgress());
        result["segmentProgress"] = JsonFloat(manager.GetSegmentProgress());
        result["activeSegmentDuration"] =
            JsonFloat(manager.GetSegmentDuration());
        result["segmentRemainingSeconds"] =
            JsonFloat(manager.GetSegmentRemainingTime());
        result["segmentDecision"] = SegmentDecisionToJson(
            manager.GetSegmentDecisionInfo());
        result["crossfadeProgress"] = JsonFloat(manager.GetCrossfadeProgress());
        result["secondsUntilTransition"] = JsonFloat(
            manager.GetSecondsUntilTransition());
        result["transitionReason"] = manager.GetLastTransitionReason();
    }
    return result;
}

nlohmann::json ScheduleToJson(
    const AnnouncementManager::ScheduledAnnouncement& announcement)
{
    return {
        {"scheduleId", announcement.scheduleId},
        {"announcement", announcement.announcementId},
        {"hour", announcement.hour},
        {"minute", announcement.minute},
        {"triggeredToday", announcement.triggered}
    };
}

nlohmann::json LoadReportToJson(const AssetLoadReport& report)
{
    nlohmann::json failures = nlohmann::json::array();
    for (const auto& failure : report.failures)
    {
        failures.push_back({
            {"id", failure.id},
            {"path", failure.path},
            {"reason", failure.reason}
        });
    }
    return {
        {"playlists", report.playlists},
        {"sounds", report.sounds},
        {"announcements", report.announcements},
        {"schedules", report.schedules},
        {"failedAssets", std::move(failures)}
    };
}

nlohmann::json SafetyToJson(const SafetySnapshot& snapshot)
{
    nlohmann::json incident = nullptr;
    if (snapshot.incident)
    {
        incident = {
            {"id", snapshot.incident->id},
            {"causes", snapshot.incident->causes}
        };
    }
    return {
        {"mode", SafetyModeToString(snapshot.mode)},
        {"enabled", snapshot.enabled},
        {"baselineEstablished", snapshot.baselineEstablished},
        {"safeToPlay", snapshot.safeToPlay},
        {"hasFreshHeartbeat", snapshot.hasFreshHeartbeat},
        {"resetAllowed", snapshot.resetAllowed},
        {"heartbeatAgeMs", snapshot.heartbeatAge
            ? nlohmann::json(snapshot.heartbeatAge->count())
            : nlohmann::json(nullptr)},
        {"heartbeat", {
            {"source", snapshot.heartbeatSource.empty()
                ? nlohmann::json(nullptr) : nlohmann::json(snapshot.heartbeatSource)},
            {"session", snapshot.heartbeatSession.empty()
                ? nlohmann::json(nullptr) : nlohmann::json(snapshot.heartbeatSession)},
            {"sequence", snapshot.heartbeatSequence},
            {"signal", InterlockSignalToString(snapshot.interlockSignal)}
        }},
        {"incident", std::move(incident)},
        {"inhibitReason", snapshot.inhibitReason.empty()
            ? nlohmann::json(nullptr) : nlohmann::json(snapshot.inhibitReason)},
        {"persistenceError", snapshot.persistenceError.empty()
            ? nlohmann::json(nullptr) : nlohmann::json(snapshot.persistenceError)}
    };
}

nlohmann::json CinemaStatusToJson(
    const CinemaManager& manager,
    std::chrono::system_clock::time_point systemNow = std::chrono::system_clock::now(),
    std::chrono::steady_clock::time_point steadyNow = std::chrono::steady_clock::now())
{
    const CinemaStatus status = manager.GetStatus(systemNow, steadyNow);
    return {
        {"enabled", status.enabled},
        {"safetyEnabled", status.safetyEnabled},
        {"interlockEnabled", status.interlockEnabled},
        {"safeToPlay", status.safeToPlay},
        {"automationSuspended", status.automationSuspended},
        {"emergencyPlaying", status.emergencyPlaying},
        {"operationalInhibit", status.operationalInhibit},
        {"operationalInhibitReason", status.operationalInhibitReason.empty()
            ? nlohmann::json(nullptr)
            : nlohmann::json(status.operationalInhibitReason)},
        {"selected", status.selectedScheduleId.empty()
            ? nlohmann::json(nullptr)
            : nlohmann::json({
                {"schedule", status.selectedScheduleId},
                {"playlist", status.selectedPlaylist}})},
        {"active", status.activeScheduleId.empty()
            ? nlohmann::json(nullptr)
            : nlohmann::json({
                {"schedule", status.activeScheduleId},
                {"playlist", status.activePlaylist}})},
        {"resume", {
            {"pending", status.resume.pending},
            {"automaticEligible", status.resume.automaticEligible},
            {"previousShutdownClean", status.resume.previousShutdownClean},
            {"capturedAtUnixMs", status.resume.capturedAtUnixMilliseconds},
            {"playlist", status.resume.playlist.empty()
                ? nlohmann::json(nullptr) : nlohmann::json(status.resume.playlist)},
            {"track", status.resume.track.empty()
                ? nlohmann::json(nullptr) : nlohmann::json(status.resume.track)},
            {"positionMs", status.resume.positionMilliseconds},
            {"status", status.resume.status}
        }},
        {"persistenceError", status.persistenceError.empty()
            ? nlohmann::json(nullptr) : nlohmann::json(status.persistenceError)},
        {"safety", SafetyToJson(manager.GetSafetySnapshot(steadyNow))}
    };
}

std::string TwoDigits(int value)
{
    std::string result(2, '0');
    result[0] = static_cast<char>('0' + (value / 10) % 10);
    result[1] = static_cast<char>('0' + value % 10);
    return result;
}

std::string FourDigits(int value)
{
    return TwoDigits(value / 100) + TwoDigits(value % 100);
}

std::string DateText(const CinemaDate& value, bool annual)
{
    const std::string monthDay = TwoDigits(value.month) + "-" + TwoDigits(value.day);
    return annual ? monthDay : FourDigits(value.year) + "-" + monthDay;
}

nlohmann::json CinemaScheduleToJson(const CinemaSchedule& schedule)
{
    nlohmann::json period = {{"type", CinemaPeriodKindName(schedule.period.kind)}};
    switch (schedule.period.kind)
    {
        case CinemaPeriodKind::AbsoluteDate:
            period["date"] = DateText(schedule.period.start, false);
            break;
        case CinemaPeriodKind::AbsoluteRange:
            period["start"] = DateText(schedule.period.start, false);
            period["end"] = DateText(schedule.period.end, false);
            break;
        case CinemaPeriodKind::AnnualDate:
            period["date"] = DateText(schedule.period.start, true);
            break;
        case CinemaPeriodKind::AnnualRange:
            period["start"] = DateText(schedule.period.start, true);
            period["end"] = DateText(schedule.period.end, true);
            break;
        case CinemaPeriodKind::EasterRange:
            period["startOffsetDays"] = schedule.period.startOffsetDays;
            period["endOffsetDays"] = schedule.period.endOffsetDays;
            break;
        case CinemaPeriodKind::Always:
            break;
    }
    const auto timeText = [](int minuteOfDay) {
        return TwoDigits(minuteOfDay / 60) + ":" + TwoDigits(minuteOfDay % 60);
    };
    return {
        {"id", schedule.id},
        {"playlist", schedule.playlist},
        {"enabled", schedule.enabled},
        {"priority", schedule.priority},
        {"period", std::move(period)},
        {"window", {
            {"start", timeText(schedule.windowStartMinute)},
            {"end", timeText(schedule.windowEndMinute)}
        }},
        {"weekdays", schedule.isoWeekdays}
    };
}

bool ParseFixedInt(std::string_view value, int& result)
{
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

CinemaCivilTime ParseCinemaLocalTime(const std::string& value)
{
    if (value.size() != 16 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':')
    {
        throw CommandError(
            CliExitCode::InvalidArgument,
            "invalid_local_datetime",
            "Local cinema time must use YYYY-MM-DDTHH:MM.");
    }
    CinemaCivilTime result;
    int hour = 0;
    int minute = 0;
    if (!ParseFixedInt(std::string_view(value).substr(0, 4), result.year) ||
        !ParseFixedInt(std::string_view(value).substr(5, 2), result.month) ||
        !ParseFixedInt(std::string_view(value).substr(8, 2), result.day) ||
        !ParseFixedInt(std::string_view(value).substr(11, 2), hour) ||
        !ParseFixedInt(std::string_view(value).substr(14, 2), minute) ||
        !IsValidCinemaDate(result) || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59)
    {
        throw CommandError(
            CliExitCode::InvalidArgument,
            "invalid_local_datetime",
            "Local cinema time is not a valid Gregorian date and 24-hour time.");
    }
    using namespace std::chrono;
    const weekday weekdayValue{sys_days{
        year{result.year} / month{static_cast<unsigned int>(result.month)} /
        day{static_cast<unsigned int>(result.day)}}};
    result.isoWeekday = weekdayValue.iso_encoding();
    result.minuteOfDay = hour * 60 + minute;
    return result;
}

InterlockSignal ParseInterlockSignal(const std::string& value)
{
    if (value == "safe") return InterlockSignal::Safe;
    if (value == "alarm") return InterlockSignal::Alarm;
    if (value == "fault") return InterlockSignal::Fault;
    throw CommandError(
        CliExitCode::InvalidArgument,
        "invalid_interlock_state",
        "Interlock state must be safe, alarm, or fault.");
}

void RequireCinemaResult(const CinemaResult& result)
{
    if (result.ok) return;
    const bool inputError = result.code.starts_with("invalid_");
    const bool ioError = result.code.find("persistence") != std::string::npos ||
        result.code.find("state_") != std::string::npos;
    throw CommandError(
        inputError ? CliExitCode::InvalidArgument
                   : ioError ? CliExitCode::InputOutput : CliExitCode::Conflict,
        result.code.empty() ? "cinema_operation_failed" : result.code,
        result.message.empty() ? "Cinema operation failed." : result.message);
}

nlohmann::json StatusToJson(const ApplicationRuntime& runtime)
{
    auto& playlistManager = PlaylistManager::GetInstance();
    auto& announcementManager = AnnouncementManager::GetInstance();

    nlohmann::json activePlaylist = nullptr;
    if (!playlistManager.IsLibraryPlaying())
    {
        if (const auto* playlist = playlistManager.GetActivePlaylist())
        {
            activePlaylist = PlaylistToJson(*playlist);
            activePlaylist["currentTrack"] = playlistManager.GetCurrentTrackName();
            activePlaylist["nextTrack"] = playlistManager.GetNextTrackName();
            activePlaylist["trackProgress"] = JsonFloat(playlistManager.GetTrackProgress());
            activePlaylist["segmentProgress"] = JsonFloat(playlistManager.GetSegmentProgress());
            activePlaylist["segmentDecision"] = SegmentDecisionToJson(
                playlistManager.GetSegmentDecisionInfo());
            activePlaylist["crossfadeProgress"] = JsonFloat(playlistManager.GetCrossfadeProgress());
            activePlaylist["transitionReason"] = playlistManager.GetLastTransitionReason();
            activePlaylist["secondsUntilTransition"] = JsonFloat(
                playlistManager.GetSecondsUntilTransition());
        }
    }

    nlohmann::json cinema = nullptr;
    if (const CinemaManager* manager = runtime.GetCinemaManager())
        cinema = CinemaStatusToJson(*manager);

    return {
        {"runtime", {
            {"initialized", runtime.IsInitialized()},
            {"audioOutput", runtime.IsNoSound() ? "no-sound" : "default"},
            {"bluetooth", runtime.IsBluetoothEnabled()},
            {"bluetoothState", runtime.GetBluetoothState()},
            {"bluetoothError", runtime.GetBluetoothError().empty()
                ? nlohmann::json(nullptr)
                : nlohmann::json(runtime.GetBluetoothError())},
            {"configPath", runtime.GetConfigPath()},
            {"resourceRoot", runtime.GetResourceRoot()},
            {"stateDirectory", runtime.GetStateDirectory()},
            {"playbackMemory", {
                {"persistent", AudioManager::GetInstance()
                    .IsPlaybackMemoryPersistent()},
                {"path", AudioManager::GetInstance().GetPlaybackMemoryPath()},
                {"error", AudioManager::GetInstance()
                    .GetPlaybackMemoryError().empty()
                    ? nlohmann::json(nullptr)
                    : nlohmann::json(AudioManager::GetInstance()
                        .GetPlaybackMemoryError())}
            }},
            {"load", LoadReportToJson(runtime.GetLoadReport())}
        }},
        {"mixer", MixerToJson()},
        {"sounds", AudioManager::GetInstance().GetAllSounds().size()},
        {"playlists", playlistManager.GetPlaylistNames().size()},
        {"activePlaylist", std::move(activePlaylist)},
        {"musicLibrary", MusicLibraryToJson(playlistManager, false)},
        {"announcement", {
            {"active", announcementManager.IsAnnouncing()},
            {"id", announcementManager.IsAnnouncing()
                ? nlohmann::json(announcementManager.GetCurrentAnnouncementName())
                : nlohmann::json(nullptr)},
            {"state", announcementManager.GetAnnouncementStateString()},
            {"progress", JsonFloat(announcementManager.GetAnnouncementProgress())},
            {"schedules", announcementManager.GetScheduledAnnouncements().size()}
        }},
        {"cinema", std::move(cinema)}
    };
}

void ValidateRange(std::string_view name, double value, double minimum, double maximum)
{
    if (value < minimum || value > maximum)
    {
        throw CommandError(
            CliExitCode::InvalidArgument,
            "value_out_of_range",
            "Parameter '" + std::string(name) + "' must be between " +
                std::to_string(minimum) + " and " + std::to_string(maximum) + ".",
            {{"parameter", name}, {"minimum", minimum}, {"maximum", maximum}, {"value", value}});
    }
}

void ValidateSegmentOptions(const PlaylistOptions& options)
{
    const auto durationIsValid = [](float value) {
        return std::isfinite(value) && value >= 0.001f && value <= 86400.0f;
    };
    if (!durationIsValid(options.segmentDuration) ||
        !durationIsValid(options.minSegmentDuration) ||
        !durationIsValid(options.maxSegmentDuration) ||
        options.minSegmentDuration > options.maxSegmentDuration)
    {
        throw CommandError(
            CliExitCode::InvalidArgument,
            "invalid_segment_duration_range",
            "Segment durations must be between 0.001 and 86400 seconds, and "
            "the minimum cannot exceed the maximum.",
            {{"segmentDuration", options.segmentDuration},
             {"minSegmentDuration", options.minSegmentDuration},
             {"maxSegmentDuration", options.maxSegmentDuration}});
    }
}

void ParseScheduleTime(
    const Parameters& parameters, int& hour, int& minute, bool allowPartial)
{
    const bool hasAt = parameters.Has("at");
    const bool hasHour = parameters.Has("hour");
    const bool hasMinute = parameters.Has("minute");
    if (hasAt && (hasHour || hasMinute))
        throw CommandError(
            CliExitCode::Usage,
            "ambiguous_time",
            "Provide either 'at' or numeric hour/minute fields, not both.");
    if (!allowPartial && !hasAt && (!hasHour || !hasMinute))
        throw CommandError(
            CliExitCode::Usage,
            "missing_time",
            "Provide 'at' or both 'hour' and 'minute'.");

    if (hasAt)
    {
        const std::string at = parameters.String("at");
        const std::size_t colon = at.find(':');
        if (colon == std::string::npos)
            throw CommandError(
                CliExitCode::InvalidArgument, "invalid_time", "Time must use HH:MM format.");
        const std::string hourText = at.substr(0, colon);
        const std::string minuteText = at.substr(colon + 1);
        const auto hourParsed = std::from_chars(
            hourText.data(), hourText.data() + hourText.size(), hour);
        const auto minuteParsed = std::from_chars(
            minuteText.data(), minuteText.data() + minuteText.size(), minute);
        if (hourParsed.ec != std::errc{} || hourParsed.ptr != hourText.data() + hourText.size() ||
            minuteParsed.ec != std::errc{} ||
            minuteParsed.ptr != minuteText.data() + minuteText.size())
        {
            throw CommandError(
                CliExitCode::InvalidArgument, "invalid_time", "Time must use HH:MM format.");
        }
    }
    else
    {
        if (hasHour) hour = parameters.Integer("hour");
        if (hasMinute) minute = parameters.Integer("minute");
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
    {
        throw CommandError(
            CliExitCode::InvalidArgument,
            "invalid_time",
            "Hour must be 0-23 and minute must be 0-59.");
    }
}

bool IsPlaying(const AudioManager::SoundData& sound)
{
    for (FMOD::Channel* channel : sound.channels)
    {
        bool playing = false;
        if (channel && channel->isPlaying(&playing) == FMOD_OK && playing) return true;
    }
    return false;
}

nlohmann::json FindSoundReferences(
    const std::string& id, const AudioManager::SoundData& sound)
{
    nlohmann::json references = nlohmann::json::object();
    std::size_t activeChannels = 0;
    std::size_t activeFades = 0;
    auto& audioManager = AudioManager::GetInstance();
    for (FMOD::Channel* channel : sound.channels)
    {
        bool playing = false;
        if (channel && channel->isPlaying(&playing) == FMOD_OK && playing)
            ++activeChannels;
        if (audioManager.IsChannelFading(channel)) ++activeFades;
    }
    if (activeChannels > 0) references["activeChannels"] = activeChannels;
    if (activeFades > 0) references["activeFades"] = activeFades;

    nlohmann::json playlists = nlohmann::json::array();
    for (const auto& playlist : PlaylistManager::GetInstance().GetAllPlaylists())
    {
        if (std::find(playlist.tracks.begin(), playlist.tracks.end(), id) !=
            playlist.tracks.end())
            playlists.push_back(playlist.name);
    }
    if (!playlists.empty()) references["playlists"] = std::move(playlists);

    const auto& playlistManager = PlaylistManager::GetInstance();
    if (playlistManager.IsLibraryPlaying())
    {
        const auto& snapshot = playlistManager.GetLibraryPlaybackSnapshot();
        if (std::find(snapshot.begin(), snapshot.end(), id) != snapshot.end())
            references["activeMusicLibrary"] = true;
    }

    nlohmann::json schedules = nlohmann::json::array();
    for (const auto& schedule :
         AnnouncementManager::GetInstance().GetScheduledAnnouncements())
    {
        if (schedule.announcementId == id) schedules.push_back(schedule.scheduleId);
    }
    if (!schedules.empty()) references["scheduleIds"] = std::move(schedules);

    const auto& announcementManager = AnnouncementManager::GetInstance();
    if (announcementManager.IsAnnouncing() &&
        announcementManager.GetCurrentAnnouncementName() == id)
        references["activeAnnouncement"] = true;

    return references;
}

} // namespace

CliResult CliResult::Success(nlohmann::json data)
{
    CliResult result;
    result.data = std::move(data);
    return result;
}

CliResult CliResult::Failure(
    CliExitCode exitCode,
    std::string errorCode,
    std::string errorMessage,
    nlohmann::json details)
{
    CliResult result;
    result.exitCode = exitCode;
    result.errorCode = std::move(errorCode);
    result.errorMessage = std::move(errorMessage);
    result.errorDetails = std::move(details);
    return result;
}

CliResult CliCommandProcessor::Execute(
    const std::string& command, const nlohmann::json& parametersValue)
{
    try
    {
        AudioManager::GetInstance().RefreshChannelState();
        const Parameters parameters(parametersValue);

        static const std::set<std::string_view> normalPlaybackCommands = {
            "sound.play", "sound.resume", "playlist.play",
            "playlist.play-index", "playlist.next", "library.play", "library.next",
            "announcement.play"};
        if (normalPlaybackCommands.contains(command))
        {
            RequireRuntime(m_runtime);
            RequireNormalPlaybackAllowed(m_runtime);
        }

        if (command == "system.ping")
        {
            parameters.Allow({});
            return CliResult::Success({{"pong", true}});
        }
        if (command == "system.capabilities")
        {
            parameters.Allow({});
            return CliResult::Success(GetSchema());
        }
        if (command == "system.status")
        {
            parameters.Allow({});
            return CliResult::Success(StatusToJson(m_runtime));
        }
        if (command == "system.health")
        {
            parameters.Allow({});
            const bool runtimeReady = m_runtime.IsInitialized() &&
                                      FModWrapper::GetInstance().IsInitialized();
            const bool bluetoothDegraded = m_runtime.GetBluetoothState() == "failed";
            const bool playbackInhibited = runtimeReady &&
                !m_runtime.CanPlayNormalAudio();
            const bool playbackMemoryPersistent = runtimeReady &&
                AudioManager::GetInstance().IsPlaybackMemoryPersistent();
            const bool degraded = !m_runtime.GetLoadReport().failures.empty() ||
                                  bluetoothDegraded ||
                                  (runtimeReady && !playbackMemoryPersistent);
            return CliResult::Success({
                {"status", !runtimeReady ? "unavailable"
                    : playbackInhibited ? "critical"
                    : degraded ? "degraded" : "ok"},
                {"runtimeReady", runtimeReady},
                {"safeToPlay", runtimeReady && m_runtime.CanPlayNormalAudio()},
                {"failedAssets", m_runtime.GetLoadReport().failures.size()},
                {"playbackMemoryPersistent", playbackMemoryPersistent},
                {"playbackMemoryError", AudioManager::GetInstance()
                    .GetPlaybackMemoryError().empty()
                    ? nlohmann::json(nullptr)
                    : nlohmann::json(AudioManager::GetInstance()
                        .GetPlaybackMemoryError())},
                {"bluetoothState", m_runtime.GetBluetoothState()},
                {"bluetoothError", m_runtime.GetBluetoothError().empty()
                    ? nlohmann::json(nullptr)
                    : nlohmann::json(m_runtime.GetBluetoothError())},
                {"cinema", m_runtime.GetCinemaManager()
                    ? CinemaStatusToJson(*m_runtime.GetCinemaManager())
                    : nlohmann::json(nullptr)}
            });
        }
        if (command == "system.shutdown")
        {
            parameters.Allow({});
            CliResult result = CliResult::Success({{"shuttingDown", true}});
            result.shutdownRequested = true;
            return result;
        }

        if (command == "config.validate")
        {
            parameters.Allow({"path", "check_files"});
            const std::string configuredPath = parameters.String(
                "path", false, m_runtime.GetConfigPath());
            if (configuredPath.empty())
                throw CommandError(
                    CliExitCode::Usage,
                    "missing_parameter",
                    "Configuration path is required when no configuration is loaded.");
            const std::string path = ResolvePath(configuredPath);
            std::vector<ConfigValidationIssue> issues;
            std::string error;
            AppConfig config;
            const bool valid = LoadValidatedAppConfig(path, config, issues, error);
            nlohmann::json issueJson = nlohmann::json::array();
            for (const auto& issue : issues)
                issueJson.push_back({{"path", issue.path}, {"message", issue.message}});
            if (!valid)
            {
                return CliResult::Failure(
                    CliExitCode::Configuration,
                    "invalid_configuration",
                    error.empty() ? "Configuration validation failed." : error,
                    {{"path", path}, {"issues", std::move(issueJson)}});
            }

            nlohmann::json missingFiles = nlohmann::json::array();
            if (parameters.Boolean("check_files", false))
            {
                std::filesystem::path root = PathFromUtf8(path).parent_path();
                if (root.filename() == "config") root = root.parent_path();
                const auto check = [&](const std::string& configuredPath) {
                    if (configuredPath.empty()) return;
                    const std::string resolved = ResolvePath(configuredPath, PathToUtf8(root));
                    if (!std::filesystem::is_regular_file(PathFromUtf8(resolved)))
                        missingFiles.push_back(resolved);
                };
                for (const auto& playlist : config.playlists)
                    for (const auto& track : playlist.tracks) check(track.path);
                for (const auto& announcement : config.announcements) check(announcement.path);
            }
            if (!missingFiles.empty())
            {
                return CliResult::Failure(
                    CliExitCode::Configuration,
                    "missing_assets",
                    "Configuration is valid but references missing assets.",
                    {{"path", path}, {"missingFiles", std::move(missingFiles)}});
            }
            return CliResult::Success({{"valid", true}, {"path", path}, {"issues", issueJson}});
        }
        if (command == "config.show")
        {
            parameters.Allow({"path"});
            const std::string configuredPath = parameters.String(
                "path", false, m_runtime.GetConfigPath());
            if (configuredPath.empty())
                throw CommandError(
                    CliExitCode::Usage,
                    "missing_parameter",
                    "Configuration path is required when no configuration is loaded.");
            const std::string path = ResolvePath(configuredPath);
            std::ifstream input(PathFromUtf8(path));
            if (!input.is_open())
                throw CommandError(
                    CliExitCode::InputOutput,
                    "file_open_failed",
                    "Unable to open configuration file.",
                    {{"path", path}});
            nlohmann::json document;
            try
            {
                input >> document;
            }
            catch (const std::exception& exception)
            {
                throw CommandError(
                    CliExitCode::Configuration,
                    "invalid_json",
                    exception.what(),
                    {{"path", path}});
            }
            return CliResult::Success({{"path", path}, {"config", std::move(document)}});
        }
        if (command == "config.reload")
        {
            parameters.Allow({"path"});
            RequireRuntime(m_runtime);
            if (const CinemaManager* cinema = m_runtime.GetCinemaManager();
                cinema && !cinema->IsReloadAllowed())
            {
                throw CommandError(
                    CliExitCode::Conflict,
                    "safety_incident_latched",
                    "Configuration reload is blocked while a safety incident is latched.");
            }
            const std::string path = parameters.Has("path")
                ? ResolvePath(parameters.String("path"))
                : m_runtime.GetConfigPath();
            std::string error;
            if (!m_runtime.ReloadConfiguration(path, error))
                throw CommandError(
                    CliExitCode::Configuration,
                    "reload_failed",
                    error,
                    {{"path", path}});
            return CliResult::Success({
                {"path", m_runtime.GetConfigPath()},
                {"load", LoadReportToJson(m_runtime.GetLoadReport())}
            });
        }

        if (command == "cinema.status")
        {
            parameters.Allow({});
            CinemaManager& manager = RequireCinemaManager(m_runtime);
            return CliResult::Success({{"cinema", CinemaStatusToJson(manager)}});
        }
        if (command == "cinema.schedules")
        {
            parameters.Allow({});
            CinemaManager& manager = RequireCinemaManager(m_runtime);
            const CinemaCivilTime localTime = GetLocalCinemaTime(
                std::chrono::system_clock::now());
            nlohmann::json schedules = nlohmann::json::array();
            for (const CinemaSchedule& schedule : manager.GetSchedules())
            {
                nlohmann::json item = CinemaScheduleToJson(schedule);
                item["activeNow"] = IsCinemaScheduleActive(schedule, localTime);
                schedules.push_back(std::move(item));
            }
            return CliResult::Success({{"schedules", std::move(schedules)}});
        }
        if (command == "cinema.preview")
        {
            parameters.Allow({"at"});
            CinemaManager& manager = RequireCinemaManager(m_runtime);
            const CinemaCivilTime localTime = parameters.Has("at")
                ? ParseCinemaLocalTime(parameters.String("at"))
                : GetLocalCinemaTime(std::chrono::system_clock::now());
            const CinemaScheduleResolution resolution = manager.Preview(localTime);
            nlohmann::json matches = nlohmann::json::array();
            for (const CinemaSchedule* schedule : resolution.matches)
                matches.push_back(CinemaScheduleToJson(*schedule));
            return CliResult::Success({
                {"localTime", FourDigits(localTime.year) + "-" +
                    TwoDigits(localTime.month) + "-" + TwoDigits(localTime.day) +
                    "T" + TwoDigits(localTime.minuteOfDay / 60) + ":" +
                    TwoDigits(localTime.minuteOfDay % 60)},
                {"selected", resolution.selected
                    ? CinemaScheduleToJson(*resolution.selected)
                    : nlohmann::json(nullptr)},
                {"matches", std::move(matches)}
            });
        }
        if (command == "safety.status")
        {
            parameters.Allow({});
            CinemaManager& manager = RequireCinemaManager(m_runtime);
            return CliResult::Success(
                {{"safety", SafetyToJson(manager.GetSafetySnapshot())}});
        }
        if (command == "safety.heartbeat")
        {
            parameters.Allow({"source_id", "session_id", "sequence", "state"});
            CinemaManager& manager = RequireCinemaManager(m_runtime);
            SafetyHeartbeat heartbeat;
            heartbeat.sourceId = parameters.String("source_id");
            heartbeat.sessionId = parameters.String("session_id");
            heartbeat.sequence = parameters.Unsigned64("sequence");
            heartbeat.signal = ParseInterlockSignal(parameters.String("state"));
            const CinemaResult result = manager.ObserveHeartbeat(heartbeat);
            RequireCinemaResult(result);
            return CliResult::Success({
                {"accepted", true},
                {"result", result.code},
                {"safety", SafetyToJson(manager.GetSafetySnapshot())}
            });
        }
        if (command == "safety.trip")
        {
            parameters.Allow({"cause", "source", "play_evacuation"});
            CinemaManager& manager = RequireCinemaManager(m_runtime);
            const CinemaResult result = manager.Trip(
                parameters.String("cause"),
                parameters.String("source", false, "cli"),
                parameters.Boolean("play_evacuation", false));
            RequireCinemaResult(result);
            return CliResult::Success({
                {"latched", true},
                {"result", result.code},
                {"safety", SafetyToJson(manager.GetSafetySnapshot())}
            });
        }
        if (command == "safety.reset")
        {
            parameters.Allow({"incident_id", "operator", "reason"});
            CinemaManager& manager = RequireCinemaManager(m_runtime);
            const CinemaResult result = manager.Reset(
                parameters.String("incident_id"),
                parameters.String("operator"),
                parameters.String("reason"));
            RequireCinemaResult(result);
            return CliResult::Success({
                {"reset", true},
                {"playbackSuspended", true},
                {"safety", SafetyToJson(manager.GetSafetySnapshot())}
            });
        }
        if (command == "session.status")
        {
            parameters.Allow({});
            CinemaManager& manager = RequireCinemaManager(m_runtime);
            return CliResult::Success({{"session", CinemaStatusToJson(manager)}});
        }
        if (command == "session.resume")
        {
            parameters.Allow({});
            CinemaManager& manager = RequireCinemaManager(m_runtime);
            const CinemaResult result = manager.ResumePending();
            RequireCinemaResult(result);
            return CliResult::Success({
                {"resumed", true},
                {"result", result.code},
                {"session", CinemaStatusToJson(manager)}
            });
        }
        if (command == "session.discard")
        {
            parameters.Allow({});
            CinemaManager& manager = RequireCinemaManager(m_runtime);
            const CinemaResult result = manager.DiscardPending();
            RequireCinemaResult(result);
            return CliResult::Success({
                {"discarded", true},
                {"session", CinemaStatusToJson(manager)}
            });
        }

        if (command == "sound.list")
        {
            parameters.Allow({"kind"});
            RequireRuntime(m_runtime);
            const std::string requestedKind = parameters.String("kind", false);
            const std::string filter = requestedKind.empty()
                ? std::string()
                : AudioManager::SoundKindToString(ParseSoundKind(requestedKind));
            nlohmann::json sounds = nlohmann::json::array();
            for (const auto& [id, sound] : AudioManager::GetInstance().GetAllSounds())
            {
                if (!filter.empty() && filter != AudioManager::SoundKindToString(sound.kind)) continue;
                sounds.push_back(SoundToJson(id, sound, false));
            }
            return CliResult::Success({{"sounds", std::move(sounds)}});
        }
        if (command == "sound.show")
        {
            parameters.Allow({"id"});
            RequireRuntime(m_runtime);
            const std::string id = parameters.String("id");
            const auto& sounds = AudioManager::GetInstance().GetAllSounds();
            const auto it = sounds.find(id);
            if (it == sounds.end())
                throw CommandError(
                    CliExitCode::NotFound, "sound_not_found", "Sound '" + id + "' was not found.");
            return CliResult::Success({{"sound", SoundToJson(it->first, it->second)}});
        }
        if (command == "sound.load")
        {
            parameters.Allow({"id", "path", "kind", "stream"});
            RequireRuntime(m_runtime);
            const std::string id = parameters.String("id");
            const std::string path = ResolvePath(
                parameters.String("path"), m_runtime.GetResourceRoot());
            const AudioManager::SoundKind kind = ParseSoundKind(
                parameters.String("kind", false, "sfx"));
            const bool stream = parameters.Boolean(
                "stream", kind != AudioManager::SoundKind::SoundEffect);
            auto& audioManager = AudioManager::GetInstance();
            if (audioManager.GetAllSounds().contains(id))
                throw CommandError(
                    CliExitCode::Conflict,
                    "sound_already_exists",
                    "Sound '" + id + "' is already loaded.");
            if (!audioManager.LoadSound(id, path, stream, kind))
                throw CommandError(
                    CliExitCode::AudioEngine,
                    "sound_load_failed",
                    "Unable to load sound '" + id + "'.",
                    {{"path", path}});
            return CliResult::Success({{"sound", SoundToJson(id, audioManager.GetAllSounds().at(id))}});
        }
        if (command == "sound.unload" || command == "announcement.unload")
        {
            parameters.Allow({"id"});
            RequireRuntime(m_runtime);
            const std::string id = parameters.String("id");
            auto& audioManager = AudioManager::GetInstance();
            const auto sound = audioManager.GetAllSounds().find(id);
            if (sound == audioManager.GetAllSounds().end())
                throw CommandError(
                    CliExitCode::NotFound,
                    command == "announcement.unload"
                        ? "announcement_not_found" : "sound_not_found",
                    (command == "announcement.unload" ? "Announcement '" : "Sound '") +
                        id + "' was not found.");
            if (command == "announcement.unload" &&
                sound->second.kind != AudioManager::SoundKind::Announcement)
                throw CommandError(
                    CliExitCode::NotFound,
                    "announcement_not_found",
                    "Announcement '" + id + "' was not found.");
            RejectProtectedEmergencySound(sound->second, id);
            const nlohmann::json references = FindSoundReferences(id, sound->second);
            if (!references.empty())
                throw CommandError(
                    CliExitCode::Conflict,
                    "sound_in_use",
                    "Sound '" + id + "' is still referenced and cannot be unloaded.",
                    { {"id", id}, {"references", references} });
            if (!audioManager.UnloadSound(id))
                throw CommandError(
                    CliExitCode::AudioEngine,
                    "sound_unload_failed",
                    "Unable to unload sound '" + id + "'.");
            return CliResult::Success({{"id", id}, {"unloaded", true}});
        }
        if (command == "sound.play")
        {
            parameters.Allow({"id", "loop", "volume", "pitch", "fade_in"});
            RequireRuntime(m_runtime);
            const std::string id = parameters.String("id");
            auto& audioManager = AudioManager::GetInstance();
            auto soundIt = audioManager.GetAllSounds().find(id);
            if (soundIt == audioManager.GetAllSounds().end())
                throw CommandError(
                    CliExitCode::NotFound, "sound_not_found", "Sound '" + id + "' was not found.");
            RejectProtectedEmergencySound(soundIt->second, id);
            const float volume = static_cast<float>(
                parameters.Number("volume", false, 1.0));
            const float pitch = static_cast<float>(parameters.Number("pitch", false, 1.0));
            ValidateRange("volume", volume, 0.0, 3.0);
            ValidateRange("pitch", pitch, 0.25, 4.0);
            const bool loop = parameters.Boolean("loop", false);
            const bool fade = parameters.Boolean("fade_in", false);
            FMOD::Channel* channel = nullptr;
            const bool music = soundIt->second.kind == AudioManager::SoundKind::Music;
            if (music)
                channel = fade
                    ? audioManager.PlayMusicWithFadeIn(id, loop, volume, pitch)
                    : audioManager.PlayMusic(id, loop, volume, pitch);
            else
                channel = fade
                    ? audioManager.PlaySoundWithFadeIn(id, loop, volume, pitch)
                    : audioManager.PlaySound(id, loop, volume, pitch);
            if (!channel)
                throw CommandError(
                    CliExitCode::AudioEngine,
                    "playback_failed",
                    "Unable to start sound '" + id + "'.");
            return CliResult::Success({{"id", id}, {"channel", ChannelToJson(channel)}});
        }
        if (command == "sound.stop")
        {
            parameters.Allow({"id", "fade"});
            RequireRuntime(m_runtime);
            const std::string id = parameters.String("id");
            auto& audioManager = AudioManager::GetInstance();
            const auto it = audioManager.GetAllSounds().find(id);
            if (it == audioManager.GetAllSounds().end())
                throw CommandError(
                    CliExitCode::NotFound, "sound_not_found", "Sound '" + id + "' was not found.");
            RejectProtectedEmergencySound(it->second, id);
            const auto& playlistManager = PlaylistManager::GetInstance();
            if (playlistManager.GetCurrentTrackName() == id ||
                playlistManager.GetNextTrackName() == id)
                throw CommandError(
                    CliExitCode::Conflict,
                    "sound_managed_by_playlist",
                    "Use playlist.stop to stop a channel owned by a playlist.");
            const auto& announcementManager = AnnouncementManager::GetInstance();
            if (announcementManager.IsAnnouncing() &&
                announcementManager.GetCurrentAnnouncementName() == id)
                throw CommandError(
                    CliExitCode::Conflict,
                    "sound_managed_by_announcement",
                    "Use announcement.stop to stop the active announcement.");
            if (parameters.Boolean("fade", false)) audioManager.StopSoundWithFadeOut(id);
            else audioManager.StopSound(id);
            return CliResult::Success({{"id", id}, {"stopped", true}});
        }
        if (command == "sound.stop-all")
        {
            parameters.Allow({"fade"});
            RequireRuntime(m_runtime);
            PlaylistManager::GetInstance().Stop("");
            AnnouncementManager::GetInstance().StopAnnouncement();
            if (parameters.Boolean("fade", false))
                AudioManager::GetInstance().StopAllSoundsWithFadeOut();
            else
                AudioManager::GetInstance().StopAllNonEmergencyImmediately();
            return CliResult::Success({{"stopped", true}});
        }
        if (command == "sound.set")
        {
            parameters.Allow({"id", "volume", "pitch"});
            RequireRuntime(m_runtime);
            const std::string id = parameters.String("id");
            auto& audioManager = AudioManager::GetInstance();
            const auto it = audioManager.GetAllSounds().find(id);
            if (it == audioManager.GetAllSounds().end())
                throw CommandError(
                    CliExitCode::NotFound, "sound_not_found", "Sound '" + id + "' was not found.");
            RejectProtectedEmergencySound(it->second, id);
            if (!parameters.Has("volume") && !parameters.Has("pitch"))
                throw CommandError(
                    CliExitCode::Usage,
                    "missing_parameter",
                    "At least one of 'volume' or 'pitch' is required.");
            if (!IsPlaying(it->second))
                throw CommandError(
                    CliExitCode::Conflict,
                    "sound_not_playing",
                    "Sound '" + id + "' has no active channel.");
            float volume = 0.0f;
            float pitch = 1.0f;
            const bool setVolume = parameters.Has("volume");
            const bool setPitch = parameters.Has("pitch");
            if (setVolume)
            {
                volume = static_cast<float>(parameters.Number("volume"));
                ValidateRange("volume", volume, 0.0, 3.0);
            }
            if (setPitch)
            {
                pitch = static_cast<float>(parameters.Number("pitch"));
                ValidateRange("pitch", pitch, 0.25, 4.0);
            }
            if (setVolume) audioManager.SetVolume(id, volume);
            if (setPitch) audioManager.SetPitch(id, pitch);
            return CliResult::Success({{"sound", SoundToJson(id, it->second)}});
        }
        if (command == "sound.pause" || command == "sound.resume")
        {
            parameters.Allow({"id"});
            RequireRuntime(m_runtime);
            const std::string id = parameters.String("id");
            auto& sounds = AudioManager::GetInstance().GetAllSounds();
            const auto it = sounds.find(id);
            if (it == sounds.end())
                throw CommandError(
                    CliExitCode::NotFound, "sound_not_found", "Sound '" + id + "' was not found.");
            RejectProtectedEmergencySound(it->second, id);
            if (!IsPlaying(it->second))
                throw CommandError(
                    CliExitCode::Conflict,
                    "sound_not_playing",
                    "Sound '" + id + "' has no active channel.");
            const bool paused = command == "sound.pause";
            for (FMOD::Channel* channel : it->second.channels)
            {
                bool playing = false;
                if (channel && channel->isPlaying(&playing) == FMOD_OK && playing)
                    channel->setPaused(paused);
            }
            return CliResult::Success({{"id", id}, {"paused", paused}});
        }
        if (command == "sound.seek")
        {
            parameters.Allow({"id", "position_ms"});
            RequireRuntime(m_runtime);
            const std::string id = parameters.String("id");
            const int position = parameters.Integer("position_ms");
            if (position < 0)
                throw CommandError(
                    CliExitCode::InvalidArgument,
                    "invalid_position",
                    "position_ms cannot be negative.");
            auto& audioManager = AudioManager::GetInstance();
            const auto it = audioManager.GetAllSounds().find(id);
            if (it == audioManager.GetAllSounds().end())
                throw CommandError(
                    CliExitCode::NotFound, "sound_not_found", "Sound '" + id + "' was not found.");
            RejectProtectedEmergencySound(it->second, id);
            FMOD::Channel* channel = audioManager.GetLastChannelOfSound(id);
            if (!channel || channel->setPosition(
                                static_cast<unsigned int>(position), FMOD_TIMEUNIT_MS) != FMOD_OK)
                throw CommandError(
                    CliExitCode::Conflict,
                    "seek_failed",
                    "Sound '" + id + "' has no seekable active channel.");
            return CliResult::Success({{"id", id}, {"positionMs", position}});
        }

        if (command == "playlist.list")
        {
            parameters.Allow({});
            RequireRuntime(m_runtime);
            nlohmann::json playlists = nlohmann::json::array();
            for (const auto& playlist : PlaylistManager::GetInstance().GetAllPlaylists())
                playlists.push_back(PlaylistToJson(playlist));
            return CliResult::Success({{"playlists", std::move(playlists)}});
        }
        if (command == "playlist.show")
        {
            parameters.Allow({"name"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name");
            const auto* playlist = PlaylistManager::GetInstance().GetPlaylistByName(name);
            if (!playlist)
                throw CommandError(
                    CliExitCode::NotFound,
                    "playlist_not_found",
                    "Playlist '" + name + "' was not found.");
            return CliResult::Success({{"playlist", PlaylistToJson(*playlist)}});
        }
        if (command == "playlist.create")
        {
            parameters.Allow({"name"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name");
            auto& manager = PlaylistManager::GetInstance();
            if (manager.GetPlaylistByName(name))
                throw CommandError(
                    CliExitCode::Conflict,
                    "playlist_already_exists",
                    "Playlist '" + name + "' already exists.");
            manager.CreatePlaylist(name);
            return CliResult::Success({{"playlist", PlaylistToJson(*manager.GetPlaylistByName(name))}});
        }
        if (command == "playlist.delete")
        {
            parameters.Allow({"name"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name");
            auto& manager = PlaylistManager::GetInstance();
            if (!manager.GetPlaylistByName(name))
                throw CommandError(
                    CliExitCode::NotFound,
                    "playlist_not_found",
                    "Playlist '" + name + "' was not found.");
            manager.DeletePlaylist(name);
            return CliResult::Success({{"name", name}, {"deleted", true}});
        }
        if (command == "playlist.rename" || command == "playlist.duplicate")
        {
            parameters.Allow({"name", "new_name"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name");
            const std::string newName = parameters.String("new_name");
            auto& manager = PlaylistManager::GetInstance();
            if (!manager.GetPlaylistByName(name))
                throw CommandError(
                    CliExitCode::NotFound,
                    "playlist_not_found",
                    "Playlist '" + name + "' was not found.");
            if (manager.GetPlaylistByName(newName))
                throw CommandError(
                    CliExitCode::Conflict,
                    "playlist_already_exists",
                    "Playlist '" + newName + "' already exists.");
            if (command == "playlist.rename") manager.RenamePlaylist(name, newName);
            else manager.DuplicatePlaylist(name, newName);
            return CliResult::Success({{"playlist", PlaylistToJson(*manager.GetPlaylistByName(newName))}});
        }
        if (command == "playlist.add")
        {
            parameters.Allow({"name", "id"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name");
            const std::string id = parameters.String("id");
            auto& manager = PlaylistManager::GetInstance();
            if (!manager.GetPlaylistByName(name))
                throw CommandError(
                    CliExitCode::NotFound,
                    "playlist_not_found",
                    "Playlist '" + name + "' was not found.");
            const auto sound = AudioManager::GetInstance().GetAllSounds().find(id);
            if (sound == AudioManager::GetInstance().GetAllSounds().end())
                throw CommandError(
                    CliExitCode::NotFound, "sound_not_found", "Sound '" + id + "' was not found.");
            if (sound->second.kind != AudioManager::SoundKind::Music)
                throw CommandError(
                    CliExitCode::Conflict,
                    "sound_kind_conflict",
                    "Only sounds classified as music can be added to a playlist.",
                    {{"id", id}, {"kind", AudioManager::SoundKindToString(sound->second.kind)}});
            manager.AddToPlaylist(name, id);
            return CliResult::Success({{"playlist", PlaylistToJson(*manager.GetPlaylistByName(name))}});
        }
        if (command == "playlist.remove")
        {
            parameters.Allow({"name", "id", "index"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name");
            auto& manager = PlaylistManager::GetInstance();
            auto* playlist = manager.GetPlaylistByName(name);
            if (!playlist)
                throw CommandError(
                    CliExitCode::NotFound,
                    "playlist_not_found",
                    "Playlist '" + name + "' was not found.");
            if (parameters.Has("id") == parameters.Has("index"))
                throw CommandError(
                    CliExitCode::Usage,
                    "ambiguous_track",
                    "Provide exactly one of 'id' or 'index'.");
            if (parameters.Has("index"))
            {
                const int index = parameters.Integer("index");
                if (index < 0 || static_cast<std::size_t>(index) >= playlist->tracks.size())
                    throw CommandError(
                        CliExitCode::InvalidArgument,
                        "index_out_of_range",
                        "Track index is out of range.");
                manager.RemoveFromPlaylistAtIndex(name, static_cast<std::size_t>(index));
            }
            else
            {
                const std::string id = parameters.String("id");
                if (std::find(playlist->tracks.begin(), playlist->tracks.end(), id) == playlist->tracks.end())
                    throw CommandError(
                        CliExitCode::NotFound,
                        "track_not_found",
                        "Track '" + id + "' is not in playlist '" + name + "'.");
                manager.RemoveFromPlaylist(name, id);
            }
            return CliResult::Success({{"playlist", PlaylistToJson(*manager.GetPlaylistByName(name))}});
        }
        if (command == "playlist.clear")
        {
            parameters.Allow({"name"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name");
            auto& manager = PlaylistManager::GetInstance();
            if (!manager.GetPlaylistByName(name))
                throw CommandError(
                    CliExitCode::NotFound,
                    "playlist_not_found",
                    "Playlist '" + name + "' was not found.");
            manager.ClearPlaylist(name);
            return CliResult::Success({{"playlist", PlaylistToJson(*manager.GetPlaylistByName(name))}});
        }
        if (command == "playlist.move")
        {
            parameters.Allow({"name", "from", "to"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name");
            const int from = parameters.Integer("from");
            const int to = parameters.Integer("to");
            auto& manager = PlaylistManager::GetInstance();
            const auto* playlist = manager.GetPlaylistByName(name);
            if (!playlist)
                throw CommandError(
                    CliExitCode::NotFound,
                    "playlist_not_found",
                    "Playlist '" + name + "' was not found.");
            if (from < 0 || to < 0 || static_cast<std::size_t>(from) >= playlist->tracks.size() ||
                static_cast<std::size_t>(to) >= playlist->tracks.size())
                throw CommandError(
                    CliExitCode::InvalidArgument,
                    "index_out_of_range",
                    "Track move indices are out of range.");
            manager.MoveTrackToPosition(name, from, to);
            return CliResult::Success({{"playlist", PlaylistToJson(*manager.GetPlaylistByName(name))}});
        }
        if (command == "playlist.options")
        {
            parameters.Allow({
                "name", "random_order", "random_segment", "segment_duration",
                "automatic_segment_duration", "min_segment_duration",
                "max_segment_duration", "loop", "crossfade"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name");
            auto* playlist = PlaylistManager::GetInstance().GetPlaylistByName(name);
            if (!playlist)
                throw CommandError(
                    CliExitCode::NotFound,
                    "playlist_not_found",
                    "Playlist '" + name + "' was not found.");
            PlaylistOptions updatedOptions = playlist->options;
            float updatedCrossfade = playlist->crossfadeDuration;
            if (parameters.Has("random_order"))
                updatedOptions.randomOrder = parameters.Boolean("random_order", false);
            if (parameters.Has("random_segment"))
                updatedOptions.randomSegment = parameters.Boolean("random_segment", false);
            if (parameters.Has("automatic_segment_duration"))
                updatedOptions.automaticSegmentDuration =
                    parameters.Boolean("automatic_segment_duration", false);
            if (parameters.Has("loop"))
                updatedOptions.loopPlaylist = parameters.Boolean("loop", false);
            if (parameters.Has("segment_duration"))
            {
                const double duration = parameters.Number("segment_duration");
                ValidateRange("segment_duration", duration, 0.001, 86400.0);
                updatedOptions.segmentDuration = static_cast<float>(duration);
            }
            if (parameters.Has("min_segment_duration"))
            {
                const double duration = parameters.Number("min_segment_duration");
                ValidateRange("min_segment_duration", duration, 0.001, 86400.0);
                updatedOptions.minSegmentDuration = static_cast<float>(duration);
            }
            if (parameters.Has("max_segment_duration"))
            {
                const double duration = parameters.Number("max_segment_duration");
                ValidateRange("max_segment_duration", duration, 0.001, 86400.0);
                updatedOptions.maxSegmentDuration = static_cast<float>(duration);
            }
            ValidateSegmentOptions(updatedOptions);
            if (parameters.Has("crossfade"))
            {
                const double duration = parameters.Number("crossfade");
                ValidateRange("crossfade", duration, 0.0, 3600.0);
                updatedCrossfade = static_cast<float>(duration);
            }
            const PlaylistOptions previousOptions = playlist->options;
            const bool wasPlaying = playlist->isPlaying;
            const bool runtimeOptionsChanged =
                previousOptions.randomOrder != updatedOptions.randomOrder ||
                previousOptions.randomSegment != updatedOptions.randomSegment ||
                previousOptions.segmentDuration != updatedOptions.segmentDuration ||
                previousOptions.automaticSegmentDuration !=
                    updatedOptions.automaticSegmentDuration ||
                previousOptions.minSegmentDuration !=
                    updatedOptions.minSegmentDuration ||
                previousOptions.maxSegmentDuration !=
                    updatedOptions.maxSegmentDuration ||
                previousOptions.loopPlaylist != updatedOptions.loopPlaylist;
            auto& manager = PlaylistManager::GetInstance();
            if (!manager.ConfigurePlaylistPlayback(
                    name, updatedOptions, updatedCrossfade, true))
            {
                throw CommandError(
                    CliExitCode::Conflict,
                    "playlist_options_apply_failed",
                    "Playlist options could not be applied safely.");
            }
            playlist = manager.GetPlaylistByName(name);
            return CliResult::Success({
                {"playlist", PlaylistToJson(*playlist)},
                {"restarted", wasPlaying && runtimeOptionsChanged}
            });
        }
        if (command == "playlist.import")
        {
            parameters.Allow({"path", "name"});
            RequireRuntime(m_runtime);
            const std::string path = ResolvePath(parameters.String("path"));
            const std::string name = parameters.String("name", false);
            if (!PlaylistManager::GetInstance().ImportPlaylist(path, name))
                throw CommandError(
                    CliExitCode::InputOutput,
                    "playlist_import_failed",
                    "Unable to import playlist.",
                    {{"path", path}});
            return CliResult::Success({{"imported", true}, {"path", path}});
        }
        if (command == "playlist.export")
        {
            parameters.Allow({"name", "path"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name");
            const std::string path = ResolvePath(parameters.String("path"));
            if (!PlaylistManager::GetInstance().GetPlaylistByName(name))
                throw CommandError(
                    CliExitCode::NotFound,
                    "playlist_not_found",
                    "Playlist '" + name + "' was not found.");
            if (!PlaylistManager::GetInstance().ExportPlaylist(name, path))
                throw CommandError(
                    CliExitCode::InputOutput,
                    "playlist_export_failed",
                    "Unable to export playlist.",
                    {{"path", path}});
            return CliResult::Success({{"exported", true}, {"name", name}, {"path", path}});
        }
        if (command == "playlist.save" || command == "playlist.load")
        {
            parameters.Allow({"path"});
            RequireRuntime(m_runtime);
            const std::string path = ResolvePath(parameters.String("path"));
            const bool success = command == "playlist.save"
                ? PlaylistManager::GetInstance().SavePlaylistsToFile(path)
                : PlaylistManager::GetInstance().LoadPlaylistsFromFile(path);
            if (!success)
                throw CommandError(
                    CliExitCode::InputOutput,
                    command == "playlist.save" ? "playlist_save_failed" : "playlist_load_failed",
                    "Playlist file operation failed.",
                    {{"path", path}});
            return CliResult::Success({
                {command == "playlist.save" ? "saved" : "loaded", true},
                {"path", path}
            });
        }
        if (command == "playlist.play")
        {
            parameters.Allow({
                "name", "random_order", "random_segment", "segment_duration",
                "automatic_segment_duration", "min_segment_duration",
                "max_segment_duration", "loop", "crossfade"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name");
            auto& manager = PlaylistManager::GetInstance();
            auto* playlist = manager.GetPlaylistByName(name);
            if (!playlist)
                throw CommandError(
                    CliExitCode::NotFound,
                    "playlist_not_found",
                    "Playlist '" + name + "' was not found.");
            const bool hasPlayableTrack = std::any_of(
                playlist->tracks.begin(), playlist->tracks.end(), [](const std::string& id) {
                    const auto& sounds = AudioManager::GetInstance().GetAllSounds();
                    const auto sound = sounds.find(id);
                    return sound != sounds.end() &&
                           sound->second.kind == AudioManager::SoundKind::Music;
                });
            if (!hasPlayableTrack)
                throw CommandError(
                    CliExitCode::Conflict,
                    "playlist_not_playable",
                    "Playlist '" + name + "' has no loaded music track.");
            PlaylistOptions options = playlist->options;
            if (parameters.Has("random_order"))
                options.randomOrder = parameters.Boolean("random_order", false);
            if (parameters.Has("random_segment"))
                options.randomSegment = parameters.Boolean("random_segment", false);
            if (parameters.Has("automatic_segment_duration"))
                options.automaticSegmentDuration =
                    parameters.Boolean("automatic_segment_duration", false);
            if (parameters.Has("loop")) options.loopPlaylist = parameters.Boolean("loop", false);
            if (parameters.Has("segment_duration"))
            {
                const double duration = parameters.Number("segment_duration");
                ValidateRange("segment_duration", duration, 0.001, 86400.0);
                options.segmentDuration = static_cast<float>(duration);
            }
            if (parameters.Has("min_segment_duration"))
            {
                const double duration = parameters.Number("min_segment_duration");
                ValidateRange("min_segment_duration", duration, 0.001, 86400.0);
                options.minSegmentDuration = static_cast<float>(duration);
            }
            if (parameters.Has("max_segment_duration"))
            {
                const double duration = parameters.Number("max_segment_duration");
                ValidateRange("max_segment_duration", duration, 0.001, 86400.0);
                options.maxSegmentDuration = static_cast<float>(duration);
            }
            ValidateSegmentOptions(options);
            if (parameters.Has("crossfade"))
            {
                const double duration = parameters.Number("crossfade");
                ValidateRange("crossfade", duration, 0.0, 3600.0);
                playlist->crossfadeDuration = static_cast<float>(duration);
            }
            manager.Play(name, options);
            if (!manager.IsPlaylistPlaying(name))
                throw CommandError(
                    CliExitCode::Conflict,
                    "playlist_not_playable",
                    "Playlist '" + name + "' has no playable track.");
            return CliResult::Success({{"playlist", PlaylistToJson(*manager.GetPlaylistByName(name))}});
        }
        if (command == "playlist.play-index")
        {
            parameters.Allow({"name", "index"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name");
            const int index = parameters.Integer("index");
            auto& manager = PlaylistManager::GetInstance();
            const auto* playlist = manager.GetPlaylistByName(name);
            if (!playlist)
                throw CommandError(
                    CliExitCode::NotFound,
                    "playlist_not_found",
                    "Playlist '" + name + "' was not found.");
            if (index < 0 || static_cast<std::size_t>(index) >= playlist->tracks.size())
                throw CommandError(
                    CliExitCode::InvalidArgument,
                    "index_out_of_range",
                    "Track index is out of range.");
            manager.PlayFromIndex(name, index);
            if (!manager.IsPlaylistPlaying(name))
                throw CommandError(
                    CliExitCode::Conflict,
                    "track_not_playable",
                    "The selected track could not be played.");
            return CliResult::Success({{"playlist", PlaylistToJson(*manager.GetPlaylistByName(name))}});
        }
        if (command == "playlist.stop")
        {
            parameters.Allow({"name"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name", false);
            if (!name.empty() && !PlaylistManager::GetInstance().GetPlaylistByName(name))
                throw CommandError(
                    CliExitCode::NotFound,
                    "playlist_not_found",
                    "Playlist '" + name + "' was not found.");
            PlaylistManager::GetInstance().Stop(name);
            return CliResult::Success({{"name", name.empty() ? nlohmann::json(nullptr) : nlohmann::json(name)}, {"stopped", true}});
        }
        if (command == "playlist.next")
        {
            parameters.Allow({"name"});
            RequireRuntime(m_runtime);
            const std::string name = parameters.String("name");
            auto& manager = PlaylistManager::GetInstance();
            if (!manager.GetPlaylistByName(name))
                throw CommandError(
                    CliExitCode::NotFound,
                    "playlist_not_found",
                    "Playlist '" + name + "' was not found.");
            if (!manager.IsPlaylistPlaying(name))
                throw CommandError(
                    CliExitCode::Conflict,
                    "playlist_not_playing",
                    "Playlist '" + name + "' is not playing.");
            manager.SkipToNextTrack(name);
            return CliResult::Success({{"playlist", PlaylistToJson(*manager.GetPlaylistByName(name))}});
        }
        if (command == "playlist.status")
        {
            parameters.Allow({"name"});
            RequireRuntime(m_runtime);
            const auto& manager = PlaylistManager::GetInstance();
            const auto* playlist = parameters.Has("name")
                ? manager.GetPlaylistByName(parameters.String("name"))
                : (manager.IsLibraryPlaying() ? nullptr : manager.GetActivePlaylist());
            if (!playlist)
            {
                if (parameters.Has("name"))
                    throw CommandError(
                        CliExitCode::NotFound,
                        "playlist_not_found",
                        "Requested playlist was not found.");
                return CliResult::Success({{"active", nullptr}});
            }
            nlohmann::json status = PlaylistToJson(*playlist);
            if (manager.GetActivePlaylist() == playlist)
            {
                status["currentTrack"] = manager.GetCurrentTrackName();
                status["nextTrack"] = manager.GetNextTrackName();
                status["trackProgress"] = JsonFloat(manager.GetTrackProgress());
                status["segmentProgress"] = JsonFloat(manager.GetSegmentProgress());
                status["activeSegmentDuration"] =
                    JsonFloat(manager.GetSegmentDuration());
                status["segmentRemainingSeconds"] =
                    JsonFloat(manager.GetSegmentRemainingTime());
                status["segmentDecision"] = SegmentDecisionToJson(
                    manager.GetSegmentDecisionInfo());
                status["crossfadeProgress"] = JsonFloat(manager.GetCrossfadeProgress());
                status["secondsUntilTransition"] = JsonFloat(
                    manager.GetSecondsUntilTransition());
                status["transitionReason"] = manager.GetLastTransitionReason();
            }
            else
            {
                status["currentTrack"] = nullptr;
                status["nextTrack"] = nullptr;
                status["trackProgress"] = 0.0;
                status["segmentProgress"] = 0.0;
                status["activeSegmentDuration"] = 0.0;
                status["segmentRemainingSeconds"] = 0.0;
                status["segmentDecision"] = nullptr;
                status["crossfadeProgress"] = 0.0;
                status["secondsUntilTransition"] = 0.0;
                status["transitionReason"] = nullptr;
            }
            return CliResult::Success({{"active", std::move(status)}});
        }

        if (command == "library.play")
        {
            parameters.Allow({
                "random_order", "random_segment", "segment_duration",
                "automatic_segment_duration", "min_segment_duration",
                "max_segment_duration", "loop", "crossfade"});
            RequireRuntime(m_runtime);
            auto& manager = PlaylistManager::GetInstance();
            PlaylistOptions options = manager.GetLibraryOptions();
            float crossfade = manager.GetLibraryCrossfadeDuration();
            if (parameters.Has("random_order"))
                options.randomOrder = parameters.Boolean("random_order", false);
            if (parameters.Has("random_segment"))
                options.randomSegment = parameters.Boolean("random_segment", false);
            if (parameters.Has("automatic_segment_duration"))
                options.automaticSegmentDuration =
                    parameters.Boolean("automatic_segment_duration", false);
            if (parameters.Has("loop"))
                options.loopPlaylist = parameters.Boolean("loop", false);
            if (parameters.Has("segment_duration"))
            {
                const double duration = parameters.Number("segment_duration");
                ValidateRange("segment_duration", duration, 0.001, 86400.0);
                options.segmentDuration = static_cast<float>(duration);
            }
            if (parameters.Has("min_segment_duration"))
            {
                const double duration = parameters.Number("min_segment_duration");
                ValidateRange("min_segment_duration", duration, 0.001, 86400.0);
                options.minSegmentDuration = static_cast<float>(duration);
            }
            if (parameters.Has("max_segment_duration"))
            {
                const double duration = parameters.Number("max_segment_duration");
                ValidateRange("max_segment_duration", duration, 0.001, 86400.0);
                options.maxSegmentDuration = static_cast<float>(duration);
            }
            ValidateSegmentOptions(options);
            if (parameters.Has("crossfade"))
            {
                const double duration = parameters.Number("crossfade");
                ValidateRange("crossfade", duration, 0.0, 3600.0);
                crossfade = static_cast<float>(duration);
            }
            if (!manager.PlayLibrary(options, crossfade))
                throw CommandError(
                    CliExitCode::Conflict,
                    "music_library_not_playable",
                    "The music library has no loaded playable music track.");
            return CliResult::Success({{"library", MusicLibraryToJson(manager)}});
        }
        if (command == "library.stop")
        {
            parameters.Allow({});
            RequireRuntime(m_runtime);
            auto& manager = PlaylistManager::GetInstance();
            manager.StopLibrary();
            return CliResult::Success({{"library", MusicLibraryToJson(manager)}});
        }
        if (command == "library.next")
        {
            parameters.Allow({});
            RequireRuntime(m_runtime);
            auto& manager = PlaylistManager::GetInstance();
            if (!manager.IsLibraryPlaying())
                throw CommandError(
                    CliExitCode::Conflict,
                    "music_library_not_playing",
                    "Music library playback is not active.");
            manager.SkipLibrary();
            return CliResult::Success({{"library", MusicLibraryToJson(manager)}});
        }
        if (command == "library.status")
        {
            parameters.Allow({});
            RequireRuntime(m_runtime);
            return CliResult::Success({
                {"library", MusicLibraryToJson(PlaylistManager::GetInstance())}});
        }
        if (command == "library.history")
        {
            parameters.Allow({"id", "include_buckets"});
            RequireRuntime(m_runtime);
            auto& audioManager = AudioManager::GetInstance();
            const std::optional<std::string> requestedId = parameters.Has("id")
                ? std::optional<std::string>(parameters.String("id"))
                : std::nullopt;
            const bool includeBuckets =
                parameters.Boolean("include_buckets", false);
            if (includeBuckets && !requestedId)
            {
                throw CommandError(
                    CliExitCode::Usage,
                    "track_required_for_buckets",
                    "include_buckets requires a music track id.");
            }

            const std::int64_t now =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            nlohmann::json tracks = nlohmann::json::array();
            bool found = false;
            for (const auto& diagnostic :
                 audioManager.GetPlaybackMemoryDiagnostics(now))
            {
                if (requestedId && diagnostic.soundName != *requestedId) continue;
                found = true;
                nlohmann::json track = {
                    {"id", diagnostic.soundName},
                    {"bucketCount", diagnostic.bucketCount},
                    {"coverage", diagnostic.coverage},
                    {"meanFatigue", diagnostic.meanFatigue},
                    {"maxFatigue", diagnostic.maxFatigue}
                };
                if (const auto* fatigue =
                        audioManager.GetListeningFatigue(diagnostic.soundName))
                {
                    ListeningHeatmap::State snapshot =
                        ListeningHeatmap::ExportState(*fatigue);
                    ListeningHeatmap::DecayTo(snapshot, now);
                    track["bucketDurationSeconds"] =
                        snapshot.bucketDurationSeconds;
                    track["dailyDecayFactor"] = snapshot.dailyDecayFactor;
                    track["referenceEpochSeconds"] =
                        snapshot.referenceEpochSeconds;
                    if (includeBuckets)
                        track["bucketFatigue"] = snapshot.bucketFatigue;
                }

                nlohmann::json transitions = nlohmann::json::array();
                for (const auto& [toId, toSound] : audioManager.GetAllSounds())
                {
                    if (toSound.kind != AudioManager::SoundKind::Music) continue;
                    const auto transition = audioManager.GetTransitionMemory(
                        diagnostic.soundName, toId, now);
                    if (!transition) continue;
                    transitions.push_back({
                        {"to", toId},
                        {"totalCount", transition->totalCount},
                        {"fatigue", transition->fatigue},
                        {"diversityScore", 1.0 - transition->fatigue},
                        {"referenceEpochSeconds",
                         transition->referenceEpochSeconds}
                    });
                }
                track["outgoingTransitions"] = std::move(transitions);
                tracks.push_back(std::move(track));
            }
            if (requestedId && !found)
            {
                throw CommandError(
                    CliExitCode::NotFound,
                    "music_not_found",
                    "Requested music track was not found.");
            }
            return CliResult::Success({
                {"history", {
                    {"persistent", audioManager.IsPlaybackMemoryPersistent()},
                    {"path", audioManager.GetPlaybackMemoryPath()},
                    {"error", audioManager.GetPlaybackMemoryError().empty()
                        ? nlohmann::json(nullptr)
                        : nlohmann::json(audioManager.GetPlaybackMemoryError())},
                    {"tracks", std::move(tracks)}
                }}
            });
        }
        if (command == "library.clear-history")
        {
            parameters.Allow({"id"});
            RequireRuntime(m_runtime);
            const std::optional<std::string> id = parameters.Has("id")
                ? std::optional<std::string>(parameters.String("id"))
                : std::nullopt;
            if (id)
            {
                const auto sound =
                    AudioManager::GetInstance().GetAllSounds().find(*id);
                if (sound == AudioManager::GetInstance().GetAllSounds().end() ||
                    sound->second.kind != AudioManager::SoundKind::Music)
                {
                    throw CommandError(
                        CliExitCode::NotFound,
                        "music_not_found",
                        "Requested music track was not found.");
                }
            }
            std::string clearError;
            if (!AudioManager::GetInstance().ClearPlaybackMemory(id, clearError))
            {
                throw CommandError(
                    CliExitCode::InputOutput,
                    "playback_memory_clear_failed",
                    clearError);
            }
            return CliResult::Success({
                {"cleared", true},
                {"id", id ? nlohmann::json(*id) : nlohmann::json(nullptr)}
            });
        }

        if (command == "announcement.list")
        {
            parameters.Allow({});
            RequireRuntime(m_runtime);
            nlohmann::json announcements = nlohmann::json::array();
            for (const auto& [id, sound] : AudioManager::GetInstance().GetAllSounds())
            {
                if (sound.kind == AudioManager::SoundKind::Announcement)
                    announcements.push_back(SoundToJson(id, sound, false));
            }
            return CliResult::Success({{"announcements", std::move(announcements)}});
        }
        if (command == "announcement.load")
        {
            parameters.Allow({"id", "path"});
            RequireRuntime(m_runtime);
            const std::string id = parameters.String("id");
            const std::string path = ResolvePath(
                parameters.String("path"), m_runtime.GetResourceRoot());
            if (AudioManager::GetInstance().GetAllSounds().contains(id))
                throw CommandError(
                    CliExitCode::Conflict,
                    "sound_already_exists",
                    "Sound '" + id + "' is already loaded.");
            if (!AnnouncementManager::GetInstance().LoadAnnouncement(id, path))
                throw CommandError(
                    CliExitCode::AudioEngine,
                    "announcement_load_failed",
                    "Unable to load announcement '" + id + "'.",
                    {{"path", path}});
            return CliResult::Success({
                {"announcement", SoundToJson(
                    id, AudioManager::GetInstance().GetAllSounds().at(id))}
            });
        }
        if (command == "announcement.play")
        {
            parameters.Allow({"id", "duck", "sfx_before", "sfx_after"});
            RequireRuntime(m_runtime);
            const std::string id = parameters.String("id");
            const auto soundIt = AudioManager::GetInstance().GetAllSounds().find(id);
            if (soundIt == AudioManager::GetInstance().GetAllSounds().end() ||
                soundIt->second.kind != AudioManager::SoundKind::Announcement)
                throw CommandError(
                    CliExitCode::NotFound,
                    "announcement_not_found",
                    "Announcement '" + id + "' was not found.");
            const double duck = parameters.Number("duck", false, 0.05);
            ValidateRange("duck", duck, 0.0, 1.0);
            auto& manager = AnnouncementManager::GetInstance();
            manager.PlayAnnouncement(
                id,
                static_cast<float>(duck),
                parameters.Boolean("sfx_before", true),
                parameters.Boolean("sfx_after", true));
            if (!manager.IsAnnouncing())
                throw CommandError(
                    CliExitCode::AudioEngine,
                    "announcement_playback_failed",
                    "Unable to start announcement '" + id + "'.");
            return CliResult::Success({
                {"id", id},
                {"state", manager.GetAnnouncementStateString()}
            });
        }
        if (command == "announcement.stop")
        {
            parameters.Allow({});
            RequireRuntime(m_runtime);
            AnnouncementManager::GetInstance().StopAnnouncement();
            return CliResult::Success({{"stopped", true}});
        }
        if (command == "announcement.status")
        {
            parameters.Allow({});
            RequireRuntime(m_runtime);
            const auto& manager = AnnouncementManager::GetInstance();
            return CliResult::Success({
                {"active", manager.IsAnnouncing()},
                {"id", manager.IsAnnouncing()
                    ? nlohmann::json(manager.GetCurrentAnnouncementName())
                    : nlohmann::json(nullptr)},
                {"state", manager.GetAnnouncementStateString()},
                {"progress", JsonFloat(manager.GetAnnouncementProgress())}
            });
        }

        if (command == "schedule.list")
        {
            parameters.Allow({});
            RequireRuntime(m_runtime);
            nlohmann::json schedules = nlohmann::json::array();
            for (const auto& schedule : AnnouncementManager::GetInstance().GetScheduledAnnouncements())
                schedules.push_back(ScheduleToJson(schedule));
            return CliResult::Success({{"schedules", std::move(schedules)}});
        }
        if (command == "schedule.add")
        {
            parameters.Allow({"announcement", "at", "hour", "minute"});
            RequireRuntime(m_runtime);
            const std::string announcement = parameters.String("announcement");
            const auto soundIt = AudioManager::GetInstance().GetAllSounds().find(announcement);
            if (soundIt == AudioManager::GetInstance().GetAllSounds().end() ||
                soundIt->second.kind != AudioManager::SoundKind::Announcement)
                throw CommandError(
                    CliExitCode::NotFound,
                    "announcement_not_found",
                    "Announcement '" + announcement + "' was not found.");
            int hour = 0;
            int minute = 0;
            ParseScheduleTime(parameters, hour, minute, false);
            auto& manager = AnnouncementManager::GetInstance();
            manager.ScheduleAnnouncement(hour, minute, announcement);
            return CliResult::Success({
                {"schedule", ScheduleToJson(manager.GetScheduledAnnouncements().back())}
            });
        }
        if (command == "schedule.update")
        {
            parameters.Allow({"schedule_id", "announcement", "at", "hour", "minute"});
            RequireRuntime(m_runtime);
            const std::uint64_t scheduleId = parameters.Unsigned64("schedule_id");
            auto& manager = AnnouncementManager::GetInstance();
            const auto& schedules = manager.GetScheduledAnnouncements();
            const auto it = std::find_if(
                schedules.begin(), schedules.end(), [scheduleId](const auto& schedule) {
                    return schedule.scheduleId == scheduleId;
                });
            if (it == schedules.end())
                throw CommandError(
                    CliExitCode::NotFound,
                    "schedule_not_found",
                    "Schedule was not found.",
                    {{"scheduleId", scheduleId}});
            const std::string announcement = parameters.String(
                "announcement", false, it->announcementId);
            int hour = it->hour;
            int minute = it->minute;
            if (parameters.Has("at") || parameters.Has("hour") || parameters.Has("minute"))
                ParseScheduleTime(parameters, hour, minute, true);
            const auto soundIt = AudioManager::GetInstance().GetAllSounds().find(announcement);
            if (soundIt == AudioManager::GetInstance().GetAllSounds().end() ||
                soundIt->second.kind != AudioManager::SoundKind::Announcement)
                throw CommandError(
                    CliExitCode::NotFound,
                    "announcement_not_found",
                    "Announcement '" + announcement + "' was not found.");
            if (!manager.UpdateScheduledAnnouncementById(scheduleId, hour, minute, announcement))
                throw CommandError(
                    CliExitCode::InvalidArgument,
                    "schedule_update_failed",
                    "Schedule could not be updated.");
            const auto updated = std::find_if(
                manager.GetScheduledAnnouncements().begin(),
                manager.GetScheduledAnnouncements().end(),
                [scheduleId](const auto& schedule) { return schedule.scheduleId == scheduleId; });
            return CliResult::Success({{"schedule", ScheduleToJson(*updated)}});
        }
        if (command == "schedule.remove")
        {
            parameters.Allow({"schedule_id"});
            RequireRuntime(m_runtime);
            const std::uint64_t scheduleId = parameters.Unsigned64("schedule_id");
            if (!AnnouncementManager::GetInstance().RemoveScheduledAnnouncementById(scheduleId))
                throw CommandError(
                    CliExitCode::NotFound,
                    "schedule_not_found",
                    "Schedule was not found.",
                    {{"scheduleId", scheduleId}});
            return CliResult::Success({{"scheduleId", scheduleId}, {"removed", true}});
        }
        if (command == "schedule.reset")
        {
            parameters.Allow({});
            RequireRuntime(m_runtime);
            AnnouncementManager::GetInstance().ResetTriggeredAnnouncements();
            return CliResult::Success({{"reset", true}});
        }

        if (command == "mixer.get")
        {
            parameters.Allow({});
            RequireRuntime(m_runtime);
            return CliResult::Success({{"mixer", MixerToJson()}});
        }
        if (command == "mixer.set")
        {
            parameters.Allow({"master", "music", "announcement", "sfx", "duck"});
            RequireRuntime(m_runtime);
            if (!parameters.Has("master") && !parameters.Has("music") &&
                !parameters.Has("announcement") && !parameters.Has("sfx") &&
                !parameters.Has("duck"))
                throw CommandError(
                    CliExitCode::Usage,
                    "missing_parameter",
                    "At least one mixer value is required.");
            auto& mixer = MixerState::GetInstance();
            float master = mixer.GetMasterVolume();
            float music = mixer.GetMusicVolume();
            float announcement = mixer.GetAnnouncementVolume();
            float sfx = mixer.GetSfxVolume();
            float duck = mixer.GetDuckFactor();
            if (parameters.Has("master"))
            {
                const double value = parameters.Number("master");
                ValidateRange("master", value, 0.0, 1.0);
                master = static_cast<float>(value);
            }
            if (parameters.Has("music"))
            {
                const double value = parameters.Number("music");
                ValidateRange("music", value, 0.0, 1.0);
                music = static_cast<float>(value);
            }
            if (parameters.Has("announcement"))
            {
                const double value = parameters.Number("announcement");
                ValidateRange("announcement", value, 0.0, 3.0);
                announcement = static_cast<float>(value);
            }
            if (parameters.Has("sfx"))
            {
                const double value = parameters.Number("sfx");
                ValidateRange("sfx", value, 0.0, 3.0);
                sfx = static_cast<float>(value);
            }
            if (parameters.Has("duck"))
            {
                const double value = parameters.Number("duck");
                ValidateRange("duck", value, 0.0, 1.0);
                duck = static_cast<float>(value);
            }
            mixer.SetMasterVolume(master);
            mixer.SetMusicVolume(music);
            mixer.SetAnnouncementVolume(announcement);
            mixer.SetSfxVolume(sfx);
            mixer.SetDuckFactor(duck);
            mixer.ApplyAllVolumes();
            return CliResult::Success({{"mixer", MixerToJson()}});
        }

        if (command == "loudness.status")
        {
            parameters.Allow({"id"});
            RequireRuntime(m_runtime);
            const std::string id = parameters.String("id", false);
            if (!id.empty())
            {
                const auto sound = AudioManager::GetInstance().GetAllSounds().find(id);
                if (sound == AudioManager::GetInstance().GetAllSounds().end())
                    throw CommandError(
                        CliExitCode::NotFound,
                        "sound_not_found",
                        "Sound '" + id + "' was not found.");
                if (sound->second.kind != AudioManager::SoundKind::Music)
                    throw CommandError(
                        CliExitCode::Conflict,
                        "sound_not_analyzable",
                        "Loudness analysis is only available for music.");
            }
            nlohmann::json diagnostics = nlohmann::json::array();
            for (const auto& diagnostic : AudioManager::GetInstance().GetLoudnessDiagnostics())
            {
                if (!id.empty() && diagnostic.soundName != id) continue;
                diagnostics.push_back({
                    {"id", diagnostic.soundName},
                    {"path", diagnostic.filePath},
                    {"status", AudioManager::LoudnessStatusToString(diagnostic.status)},
                    {"integratedLufs", JsonFloat(diagnostic.integratedLufs)},
                    {"truePeakDb", JsonFloat(diagnostic.truePeakDb)},
                    {"gainDb", JsonFloat(diagnostic.gainDb)},
                    {"queuePosition", diagnostic.queuePosition},
                    {"musicAnalysisReady", diagnostic.musicAnalysisReady}
                });
            }
            return CliResult::Success({
                {"targetLufs", JsonFloat(AudioManager::GetInstance().GetLoudnessTarget())},
                {"diagnostics", std::move(diagnostics)}
            });
        }
        if (command == "loudness.analyze")
        {
            parameters.Allow({"id"});
            RequireRuntime(m_runtime);
            const std::string id = parameters.String("id", false);
            auto& audioManager = AudioManager::GetInstance();
            std::size_t queued = 0;
            if (!id.empty())
            {
                const auto it = audioManager.GetAllSounds().find(id);
                if (it == audioManager.GetAllSounds().end())
                    throw CommandError(
                        CliExitCode::NotFound, "sound_not_found", "Sound '" + id + "' was not found.");
                if (it->second.kind != AudioManager::SoundKind::Music)
                    throw CommandError(
                        CliExitCode::Conflict,
                        "sound_not_analyzable",
                        "Loudness analysis is only available for music.");
                audioManager.QueueLoudnessAnalysis(id);
                queued = 1;
            }
            else
            {
                for (const auto& [soundId, sound] : audioManager.GetAllSounds())
                {
                    if (sound.kind == AudioManager::SoundKind::Music)
                    {
                        audioManager.QueueLoudnessAnalysis(soundId);
                        ++queued;
                    }
                }
            }
            return CliResult::Success({{"queued", queued}});
        }
        if (command == "loudness.target")
        {
            parameters.Allow({"value"});
            RequireRuntime(m_runtime);
            auto& audioManager = AudioManager::GetInstance();
            if (parameters.Has("value"))
            {
                const double target = parameters.Number("value");
                ValidateRange("value", target, -30.0, -8.0);
                audioManager.SetLoudnessTarget(static_cast<float>(target));
            }
            return CliResult::Success({
                {"targetLufs", JsonFloat(audioManager.GetLoudnessTarget())}
            });
        }
        if (command == "loudness.clear-cache")
        {
            parameters.Allow({});
            RequireRuntime(m_runtime);
            auto& audioManager = AudioManager::GetInstance();
            const std::string path = audioManager.GetLoudnessCachePath();
            bool removed = false;
            std::string clearError;
            if (!audioManager.ClearLoudnessCache(removed, clearError))
                throw CommandError(
                    CliExitCode::InputOutput,
                    "cache_remove_failed",
                    clearError,
                    {{"path", path}});
            return CliResult::Success({{"path", path}, {"removed", removed}});
        }

        return CliResult::Failure(
            CliExitCode::Usage,
            "unknown_command",
            "Unknown command '" + command + "'.",
            {{"command", command}});
    }
    catch (const CommandError& error)
    {
        return CliResult::Failure(
            error.exitCode, error.code, error.what(), error.details);
    }
    catch (const std::exception& error)
    {
        return CliResult::Failure(
            CliExitCode::Internal,
            "internal_error",
            error.what());
    }
}

nlohmann::json CliCommandProcessor::GetSchema()
{
    using Json = nlohmann::json;

    const auto text = [] {
        return Json{{"type", "string"}, {"minLength", 1}};
    };
    const auto path = [&] {
        Json value = text();
        value["x-tsm-format"] = "path";
        return value;
    };
    const auto flag = [] {
        return Json{{"type", "boolean"}};
    };
    const auto flagWithDefault = [](bool defaultValue) {
        return Json{{"type", "boolean"}, {"default", defaultValue}};
    };
    const auto number = [](double minimum, double maximum) {
        return Json{
            {"type", "number"}, {"minimum", minimum}, {"maximum", maximum}};
    };
    const auto numberWithDefault = [&](double minimum, double maximum, double defaultValue) {
        Json value = number(minimum, maximum);
        value["default"] = defaultValue;
        return value;
    };
    const auto integer = [](std::int64_t minimum, std::int64_t maximum) {
        return Json{
            {"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
    };
    const auto unsignedInteger = [] {
        return Json{
            {"type", "integer"},
            {"minimum", 0},
            {"maximum", std::numeric_limits<std::uint64_t>::max()}
        };
    };
    const auto params = [](
        Json properties, std::initializer_list<const char*> requiredNames) {
        Json required = Json::array();
        for (const char* name : requiredNames) required.push_back(name);
        return Json{
            {"type", "object"},
            {"additionalProperties", false},
            {"properties", properties},
            {"required", required}
        };
    };
    const auto positions = [](std::initializer_list<const char*> names) {
        Json result = Json::array();
        for (const char* name : names) result.push_back(name);
        return result;
    };
    const auto omitted = [](Json value, const char* behavior) {
        value["x-tsm-omitted"] = behavior;
        return value;
    };
    const auto command = [](
        const char* name,
        bool persistent,
        const char* description,
        Json parameterSchema,
        Json positionals) {
        return Json{
            {"name", name},
            {"persistentRecommended", persistent},
            {"description", description},
            {"positionals", positionals},
            {"paramsSchema", parameterSchema}
        };
    };

    const Json emptyParams = params(Json::object(), {});

    Json configPath = path();
    configPath["x-tsm-defaultSource"] = "runtime.configPath";

    Json soundKind = {
        {"type", "string"},
        {"enum", Json::array({
            "sfx", "sound-effect", "music", "announcement"})}
    };
    Json soundLoadKind = soundKind;
    soundLoadKind["default"] = "sfx";
    Json stream = flagWithDefault(false);
    stream["x-tsm-defaultBy"] = {
        {"parameter", "kind"},
        {"values", {
            {"sfx", false}, {"sound-effect", false}, {"music", true},
            {"announcement", true}
        }}
    };

    Json soundSetParams = params({
        {"id", text()},
        {"volume", number(0.0, 3.0)},
        {"pitch", number(0.25, 4.0)}
    }, {"id"});
    soundSetParams["anyOf"] = Json::array({
        Json{{"required", Json::array({"volume"})}},
        Json{{"required", Json::array({"pitch"})}}
    });

    Json playlistRemoveParams = params({
        {"name", text()},
        {"id", text()},
        {"index", integer(0, std::numeric_limits<std::int32_t>::max())}
    }, {"name"});
    playlistRemoveParams["oneOf"] = Json::array({
        Json{{"required", Json::array({"id"})}},
        Json{{"required", Json::array({"index"})}}
    });

    const Json playlistOptionProperties = {
        {"name", text()},
        {"random_order", omitted(flag(), "preserve")},
        {"random_segment", omitted(flag(), "preserve")},
        {"segment_duration", omitted(number(0.001, 86400.0), "preserve")},
        {"automatic_segment_duration", omitted(flag(), "preserve")},
        {"min_segment_duration", omitted(number(0.001, 86400.0), "preserve")},
        {"max_segment_duration", omitted(number(0.001, 86400.0), "preserve")},
        {"loop", omitted(flag(), "preserve")},
        {"crossfade", omitted(number(0.0, 3600.0), "preserve")}
    };
    const Json playlistPlayProperties = {
        {"name", text()},
        {"random_order", omitted(flag(), "inheritPlaylist")},
        {"random_segment", omitted(flag(), "inheritPlaylist")},
        {"segment_duration", omitted(number(0.001, 86400.0), "inheritPlaylist")},
        {"automatic_segment_duration", omitted(flag(), "inheritPlaylist")},
        {"min_segment_duration", omitted(number(0.001, 86400.0), "inheritPlaylist")},
        {"max_segment_duration", omitted(number(0.001, 86400.0), "inheritPlaylist")},
        {"loop", omitted(flag(), "inheritPlaylist")},
        {"crossfade", omitted(number(0.0, 3600.0), "inheritPlaylist")}
    };
    const Json libraryPlayProperties = {
        {"random_order", omitted(flag(), "preserveLibrary")},
        {"random_segment", omitted(flag(), "preserveLibrary")},
        {"segment_duration", omitted(number(0.001, 86400.0), "preserveLibrary")},
        {"automatic_segment_duration", omitted(flag(), "preserveLibrary")},
        {"min_segment_duration", omitted(number(0.001, 86400.0), "preserveLibrary")},
        {"max_segment_duration", omitted(number(0.001, 86400.0), "preserveLibrary")},
        {"loop", omitted(flag(), "preserveLibrary")},
        {"crossfade", omitted(number(0.0, 3600.0), "preserveLibrary")}
    };

    Json localTime = text();
    localTime["pattern"] = R"(^(?:[01][0-9]|2[0-3]):[0-5][0-9]$)";
    localTime["x-tsm-format"] = "local-time-HH:MM";
    Json scheduleAddParams = params({
        {"announcement", text()},
        {"at", localTime},
        {"hour", integer(0, 23)},
        {"minute", integer(0, 59)}
    }, {"announcement"});
    scheduleAddParams["oneOf"] = Json::array({
        Json{
            {"required", Json::array({"at"})},
            {"not", Json{{"anyOf", Json::array({
                Json{{"required", Json::array({"hour"})}},
                Json{{"required", Json::array({"minute"})}}
            })}}}
        },
        Json{
            {"required", Json::array({"hour", "minute"})},
            {"not", Json{{"required", Json::array({"at"})}}}
        }
    });

    Json scheduleUpdateParams = params({
        {"schedule_id", unsignedInteger()},
        {"announcement", omitted(text(), "preserve")},
        {"at", localTime},
        {"hour", omitted(integer(0, 23), "preserve")},
        {"minute", omitted(integer(0, 59), "preserve")}
    }, {"schedule_id"});
    scheduleUpdateParams["not"] = {
        {"anyOf", Json::array({
            Json{{"required", Json::array({"at", "hour"})}},
            Json{{"required", Json::array({"at", "minute"})}}
        })}
    };

    Json mixerSetParams = params({
        {"master", omitted(number(0.0, 1.0), "preserve")},
        {"music", omitted(number(0.0, 1.0), "preserve")},
        {"announcement", omitted(number(0.0, 3.0), "preserve")},
        {"sfx", omitted(number(0.0, 3.0), "preserve")},
        {"duck", omitted(number(0.0, 1.0), "preserve")}
    }, {});
    mixerSetParams["anyOf"] = Json::array({
        Json{{"required", Json::array({"master"})}},
        Json{{"required", Json::array({"music"})}},
        Json{{"required", Json::array({"announcement"})}},
        Json{{"required", Json::array({"sfx"})}},
        Json{{"required", Json::array({"duck"})}}
    });

    Json cinemaLocalDateTime = text();
    cinemaLocalDateTime["pattern"] =
        R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T(?:[01][0-9]|2[0-3]):[0-5][0-9]$)";
    cinemaLocalDateTime["x-tsm-format"] = "local-datetime-YYYY-MM-DDTHH:MM";
    Json safetyIdentifier = text();
    safetyIdentifier["maxLength"] = 128;
    Json safetyReason = text();
    safetyReason["maxLength"] = 512;
    const Json interlockState = {
        {"type", "string"}, {"enum", Json::array({"safe", "alarm", "fault"})}
    };

    return {
        {"schemaVersion", CliSchemaVersion},
        {"apiVersion", CliApiVersion},
        {"protocol", "ndjson"},
        {"parameterSchemaDialect", "https://json-schema.org/draft/2020-12/schema"},
        {"limits", {{"requestBytes", 1048576}, {"tickMilliseconds", {1, 1000}}}},
        {"request", {
            {"id", "string | number | boolean | null"},
            {"command", "resource.action"},
            {"params", "object"}
        }},
        {"response", {
            {"schemaVersion", "integer"},
            {"apiVersion", "string"},
            {"id", "echoed request id"},
            {"command", "string"},
            {"ok", "boolean"},
            {"data", "object | null"},
            {"error", "{code,message,details} | null"}
        }},
        {"events", {{"ready", "Emitted once after persistent host initialization."}}},
        {"exitCodes", {
            {"success", 0}, {"usage", 2}, {"invalidArgument", 3},
            {"notFound", 4}, {"conflict", 5}, {"configuration", 6},
            {"audioEngine", 7}, {"inputOutput", 8}, {"timeout", 9},
            {"internal", 10}, {"partialSuccess", 11}, {"interrupted", 130}
        }},
        {"commands", nlohmann::json::array({
            command("system.ping", false, "Check that the host responds.", emptyParams, positions({})),
            command("system.status", false, "Return a complete runtime snapshot.", emptyParams, positions({})),
            command("system.health", false, "Return runtime health.", emptyParams, positions({})),
            command("system.capabilities", false, "Return this protocol schema.", emptyParams, positions({})),
            command("system.shutdown", true, "Stop the persistent host.", emptyParams, positions({})),
            command("config.validate", false, "Validate configuration and optionally assets.",
                params({{"path", configPath}, {"check_files", flagWithDefault(false)}}, {}), positions({"path"})),
            command("config.show", false, "Read configuration as JSON.",
                params({{"path", configPath}}, {}), positions({"path"})),
            command("config.reload", true, "Reload session configuration.",
                params({{"path", configPath}}, {}), positions({"path"})),
            command("cinema.status", false, "Return cinema automation and safety state.",
                emptyParams, positions({})),
            command("cinema.schedules", false, "List configured cinema calendars.",
                emptyParams, positions({})),
            command("cinema.preview", false, "Resolve calendars at a local civil time.",
                params({{"at", cinemaLocalDateTime}}, {}), positions({"at"})),
            command("safety.status", false, "Return the fail-safe interlock state.",
                emptyParams, positions({})),
            command("safety.heartbeat", true, "Submit a configured-source interlock heartbeat.",
                params({
                    {"source_id", safetyIdentifier},
                    {"session_id", safetyIdentifier},
                    {"sequence", unsignedInteger()},
                    {"state", interlockState}},
                    {"source_id", "session_id", "sequence", "state"}),
                positions({"source_id", "session_id", "sequence", "state"})),
            command("safety.trip", true, "Latch a safety incident and optionally request evacuation audio.",
                params({
                    {"cause", safetyIdentifier},
                    {"source", omitted(safetyIdentifier, "cli")},
                    {"play_evacuation", flagWithDefault(false)}}, {"cause"}),
                positions({"cause", "source"})),
            command("safety.reset", true, "Reset the matching incident with an operator audit record.",
                params({
                    {"incident_id", safetyIdentifier},
                    {"operator", safetyIdentifier},
                    {"reason", safetyReason}},
                    {"incident_id", "operator", "reason"}),
                positions({"incident_id", "operator", "reason"})),
            command("session.status", false, "Return restart-recovery state.",
                emptyParams, positions({})),
            command("session.resume", true, "Explicitly resume recovery or re-arm calendar playback.",
                emptyParams, positions({})),
            command("session.discard", true, "Discard interrupted playback without restarting music.",
                emptyParams, positions({})),
            command("sound.list", false, "List loaded sounds.",
                params({{"kind", soundKind}}, {}), positions({})),
            command("sound.show", false, "Inspect a sound and its channels.",
                params({{"id", text()}}, {"id"}), positions({"id"})),
            command("sound.load", true, "Load a sound.",
                params({{"id", text()}, {"path", path()}, {"kind", soundLoadKind}, {"stream", stream}},
                    {"id", "path"}), positions({"id", "path"})),
            command("sound.unload", true, "Unload a sound.",
                params({{"id", text()}}, {"id"}), positions({"id"})),
            command("sound.play", true, "Start playback.",
                params({
                    {"id", text()}, {"loop", flagWithDefault(false)},
                    {"volume", numberWithDefault(0.0, 3.0, 1.0)},
                    {"pitch", numberWithDefault(0.25, 4.0, 1.0)},
                    {"fade_in", flagWithDefault(false)}}, {"id"}), positions({"id"})),
            command("sound.stop", true, "Stop one sound.",
                params({{"id", text()}, {"fade", flagWithDefault(false)}}, {"id"}), positions({"id"})),
            command("sound.stop-all", true, "Stop all sounds.",
                params({{"fade", flagWithDefault(false)}}, {}), positions({})),
            command("sound.set", true, "Set channel volume or pitch.",
                soundSetParams, positions({"id"})),
            command("sound.pause", true, "Pause active channels.",
                params({{"id", text()}}, {"id"}), positions({"id"})),
            command("sound.resume", true, "Resume active channels.",
                params({{"id", text()}}, {"id"}), positions({"id"})),
            command("sound.seek", true, "Seek the latest channel.",
                params({
                    {"id", text()},
                    {"position_ms", integer(0, std::numeric_limits<std::int32_t>::max())}},
                    {"id", "position_ms"}), positions({"id", "position_ms"})),
            command("playlist.list", false, "List playlists.", emptyParams, positions({})),
            command("playlist.show", false, "Inspect a playlist.",
                params({{"name", text()}}, {"name"}), positions({"name"})),
            command("playlist.create", true, "Create a playlist.",
                params({{"name", text()}}, {"name"}), positions({"name"})),
            command("playlist.delete", true, "Delete a playlist.",
                params({{"name", text()}}, {"name"}), positions({"name"})),
            command("playlist.rename", true, "Rename a playlist.",
                params({{"name", text()}, {"new_name", text()}}, {"name", "new_name"}),
                    positions({"name", "new_name"})),
            command("playlist.duplicate", true, "Duplicate a playlist.",
                params({{"name", text()}, {"new_name", text()}}, {"name", "new_name"}),
                    positions({"name", "new_name"})),
            command("playlist.add", true, "Add a loaded sound.",
                params({{"name", text()}, {"id", text()}}, {"name", "id"}),
                    positions({"name", "id"})),
            command("playlist.remove", true, "Remove a track.",
                playlistRemoveParams, positions({"name", "id"})),
            command("playlist.clear", true, "Clear tracks.",
                params({{"name", text()}}, {"name"}), positions({"name"})),
            command("playlist.move", true, "Reorder tracks.",
                params({
                    {"name", text()},
                    {"from", integer(0, std::numeric_limits<std::int32_t>::max())},
                    {"to", integer(0, std::numeric_limits<std::int32_t>::max())}},
                    {"name", "from", "to"}), positions({"name", "from", "to"})),
            command("playlist.options", true, "Read or update playback options.",
                params(playlistOptionProperties, {"name"}), positions({"name"})),
            command("playlist.import", true, "Import one playlist.",
                params({{"path", path()}, {"name", text()}}, {"path"}), positions({"path", "name"})),
            command("playlist.export", false, "Export one playlist.",
                params({{"name", text()}, {"path", path()}}, {"name", "path"}),
                    positions({"name", "path"})),
            command("playlist.save", false, "Save all playlists.",
                params({{"path", path()}}, {"path"}), positions({"path"})),
            command("playlist.load", true, "Load all playlists.",
                params({{"path", path()}}, {"path"}), positions({"path"})),
            command("playlist.play", true, "Play a playlist.",
                params(playlistPlayProperties, {"name"}), positions({"name"})),
            command("playlist.play-index", true, "Play from a track index.",
                params({
                    {"name", text()},
                    {"index", integer(0, std::numeric_limits<std::int32_t>::max())}},
                    {"name", "index"}), positions({"name", "index"})),
            command("playlist.stop", true, "Stop one or every playlist.",
                params({{"name", omitted(text(), "allPlaylists")}}, {}), positions({"name"})),
            command("playlist.next", true, "Advance to the next track.",
                params({{"name", text()}}, {"name"}), positions({"name"})),
            command("playlist.status", false, "Return playback status.",
                params({{"name", omitted(text(), "activePlaylist")}}, {}), positions({"name"})),
            command("library.play", true, "Play every loaded music track.",
                params(libraryPlayProperties, {}), positions({})),
            command("library.stop", true, "Stop music-library playback.",
                emptyParams, positions({})),
            command("library.next", true, "Advance music-library playback.",
                emptyParams, positions({})),
            command("library.status", false, "Return music-library playback status.",
                emptyParams, positions({})),
            command("library.history", false, "Inspect durable exploration memory.",
                params({
                    {"id", omitted(text(), "allMusic")},
                    {"include_buckets", flagWithDefault(false)}}, {}),
                positions({"id"})),
            command("library.clear-history", true, "Clear exploration memory.",
                params({{"id", omitted(text(), "allMusic")}}, {}),
                positions({"id"})),
            command("announcement.list", false, "List announcements.", emptyParams, positions({})),
            command("announcement.load", true, "Load an announcement.",
                params({{"id", text()}, {"path", path()}}, {"id", "path"}),
                    positions({"id", "path"})),
            command("announcement.unload", true, "Unload an announcement.",
                params({{"id", text()}}, {"id"}), positions({"id"})),
            command("announcement.play", true, "Start an announcement sequence.",
                params({
                    {"id", text()}, {"duck", numberWithDefault(0.0, 1.0, 0.05)},
                    {"sfx_before", flagWithDefault(true)},
                    {"sfx_after", flagWithDefault(true)}}, {"id"}), positions({"id"})),
            command("announcement.stop", true, "Stop the current announcement.",
                emptyParams, positions({})),
            command("announcement.status", false, "Return announcement status.",
                emptyParams, positions({})),
            command("schedule.list", false, "List daily schedules.", emptyParams, positions({})),
            command("schedule.add", true, "Add a daily schedule.",
                scheduleAddParams, positions({"announcement"})),
            command("schedule.update", true, "Update a schedule by stable ID.",
                scheduleUpdateParams, positions({"schedule_id"})),
            command("schedule.remove", true, "Remove a schedule by stable ID.",
                params({{"schedule_id", unsignedInteger()}},
                    {"schedule_id"}), positions({"schedule_id"})),
            command("schedule.reset", true, "Reset today's triggered state.",
                emptyParams, positions({})),
            command("mixer.get", false, "Return mixer values.", emptyParams, positions({})),
            command("mixer.set", true, "Update mixer values.", mixerSetParams, positions({})),
            command("loudness.status", false, "Return LUFS diagnostics.",
                params({{"id", omitted(text(), "allAnalyzableSounds")}}, {}), positions({"id"})),
            command("loudness.analyze", true, "Queue LUFS analysis.",
                params({{"id", omitted(text(), "allAnalyzableSounds")}}, {}), positions({"id"})),
            command("loudness.target", true, "Read or set the LUFS target.",
                params({{"value", omitted(number(-30.0, -8.0), "readCurrentValue")}}, {}),
                    positions({})),
            command("loudness.clear-cache", true, "Remove the LUFS cache.",
                emptyParams, positions({}))
        })}
    };
}

nlohmann::json BuildCliResponse(
    const CliResult& result,
    const std::string& command,
    const nlohmann::json& requestId)
{
    nlohmann::json response = {
        {"schemaVersion", CliSchemaVersion},
        {"apiVersion", CliApiVersion},
        {"id", requestId},
        {"command", command},
        {"ok", result.IsSuccess()}
    };
    if (result.IsSuccess())
    {
        response["data"] = result.data;
        response["error"] = nullptr;
    }
    else
    {
        response["data"] = nullptr;
        response["error"] = {
            {"code", result.errorCode},
            {"message", result.errorMessage},
            {"details", result.errorDetails}
        };
    }
    return response;
}

} // namespace TSM
