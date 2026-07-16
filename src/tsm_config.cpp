#include "tsm_config.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string_view>
#include <utility>

namespace TSM
{

namespace
{
std::filesystem::path PathFromUtf8(const std::string& value)
{
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

ConfiguredSound ParseSound(const nlohmann::json& value)
{
    return {value.value("id", ""), value.value("path", "")};
}

void AddIssue(
    std::vector<ConfigValidationIssue>& issues,
    std::string path,
    std::string message)
{
    issues.push_back({std::move(path), std::move(message)});
}

bool IsNonEmptyString(const nlohmann::json& value, const char* key)
{
    return value.contains(key) && value[key].is_string() && !value[key].get<std::string>().empty();
}

bool IsNonNegativeIntegerAtMost(const nlohmann::json& value, int maximum)
{
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>() <= static_cast<std::uint64_t>(maximum);
    if (!value.is_number_integer()) return false;
    const std::int64_t number = value.get<std::int64_t>();
    return number >= 0 && number <= maximum;
}

bool IsIntegerInRange(const nlohmann::json& value, int minimum, int maximum)
{
    if (value.is_number_unsigned())
    {
        const std::uint64_t number = value.get<std::uint64_t>();
        if (maximum < 0) return false;
        const std::uint64_t unsignedMinimum = minimum > 0
            ? static_cast<std::uint64_t>(minimum)
            : 0;
        return number >= unsignedMinimum &&
            number <= static_cast<std::uint64_t>(maximum);
    }
    if (!value.is_number_integer()) return false;
    const std::int64_t number = value.get<std::int64_t>();
    return number >= minimum && number <= maximum;
}

bool ParseFixedInteger(std::string_view text, int& value)
{
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool ParseCinemaDateText(const std::string& text, CinemaDate& date)
{
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    return ParseFixedInteger(std::string_view(text).substr(0, 4), date.year) &&
        ParseFixedInteger(std::string_view(text).substr(5, 2), date.month) &&
        ParseFixedInteger(std::string_view(text).substr(8, 2), date.day) &&
        IsValidCinemaDate(date);
}

bool ParseCinemaMonthDayText(const std::string& text, CinemaDate& date)
{
    if (text.size() != 5 || text[2] != '-') return false;
    date.year = 2000;
    return ParseFixedInteger(std::string_view(text).substr(0, 2), date.month) &&
        ParseFixedInteger(std::string_view(text).substr(3, 2), date.day) &&
        IsValidCinemaDate(date);
}

bool ParseCinemaTimeText(const std::string& text, int& minuteOfDay)
{
    if (text.size() != 5 || text[2] != ':') return false;
    int hour = 0;
    int minute = 0;
    if (!ParseFixedInteger(std::string_view(text).substr(0, 2), hour) ||
        !ParseFixedInteger(std::string_view(text).substr(3, 2), minute) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59)
        return false;
    minuteOfDay = hour * 60 + minute;
    return true;
}

CinemaSchedule ParseCinemaSchedule(const nlohmann::json& value)
{
    CinemaSchedule schedule;
    schedule.id = value.value("id", "");
    schedule.playlist = value.value("playlist", "");
    schedule.enabled = value.value("enabled", true);
    schedule.priority = value.value("priority", 0);

    const auto& period = value.value("period", nlohmann::json::object());
    TryParseCinemaPeriodKind(period.value("type", "always"), schedule.period.kind);
    switch (schedule.period.kind)
    {
        case CinemaPeriodKind::AbsoluteDate:
            ParseCinemaDateText(period.value("date", ""), schedule.period.start);
            schedule.period.end = schedule.period.start;
            break;
        case CinemaPeriodKind::AbsoluteRange:
            ParseCinemaDateText(period.value("start", ""), schedule.period.start);
            ParseCinemaDateText(period.value("end", ""), schedule.period.end);
            break;
        case CinemaPeriodKind::AnnualDate:
            ParseCinemaMonthDayText(period.value("date", ""), schedule.period.start);
            schedule.period.end = schedule.period.start;
            break;
        case CinemaPeriodKind::AnnualRange:
            ParseCinemaMonthDayText(period.value("start", ""), schedule.period.start);
            ParseCinemaMonthDayText(period.value("end", ""), schedule.period.end);
            break;
        case CinemaPeriodKind::EasterRange:
            schedule.period.startOffsetDays = period.value("startOffsetDays", 0);
            schedule.period.endOffsetDays = period.value("endOffsetDays", 0);
            break;
        case CinemaPeriodKind::Always:
            break;
    }

    if (value.contains("window"))
    {
        const auto& window = value["window"];
        ParseCinemaTimeText(window.value("start", "00:00"), schedule.windowStartMinute);
        ParseCinemaTimeText(window.value("end", "00:00"), schedule.windowEndMinute);
    }
    for (const auto& weekday : value.value("weekdays", nlohmann::json::array()))
        schedule.isoWeekdays.push_back(weekday.get<int>());
    return schedule;
}
}

namespace
{

bool ReadConfigDocument(
    const std::string& filePath,
    nlohmann::json& root,
    std::string& errorMessage)
{
    try
    {
        std::ifstream input(PathFromUtf8(filePath));
        if (!input.is_open())
        {
            errorMessage = "Unable to open " + filePath;
            return false;
        }

        input >> root;
        return true;
    }
    catch (const std::exception& error)
    {
        errorMessage = error.what();
        return false;
    }
}

bool ParseAppConfigDocument(
    const nlohmann::json& root,
    AppConfig& config,
    std::string& errorMessage)
{
    AppConfig previousConfig = std::move(config);
    try
    {
        config = AppConfig{};
        config.loudnessTargetLufs = root.value("loudnessTargetLufs", -16.0f);

        for (const auto& playlistValue : root.value("playlists", nlohmann::json::array()))
        {
            ConfiguredPlaylist playlist;
            playlist.name = playlistValue.value("name", "");
            const auto& options = playlistValue.value("options", nlohmann::json::object());
            playlist.options.randomOrder = options.value("randomOrder", true);
            playlist.options.randomSegment = options.value("randomSegment", true);
            playlist.options.loopPlaylist = options.value("loopPlaylist", true);
            playlist.options.segmentDuration = options.value(
                "segmentDuration", PlaylistOptions::DefaultSegmentDuration);
            for (const auto& trackValue : playlistValue.value("tracks", nlohmann::json::array()))
            {
                ConfiguredSound track = ParseSound(trackValue);
                if (!track.id.empty() && !track.path.empty()) playlist.tracks.push_back(std::move(track));
            }
            if (!playlist.name.empty()) config.playlists.push_back(std::move(playlist));
        }

        if (root.contains("wedding"))
        {
            const auto& wedding = root["wedding"];
            config.wedding.entrance = wedding.value("entrance", "");
            config.wedding.ceremony = wedding.value("ceremony", "");
            config.wedding.exit = wedding.value("exit", "");
            if (wedding.contains("transitionSfx"))
                config.wedding.transitionSfx = ParseSound(wedding["transitionSfx"]);
        }

        for (const auto& announcementValue : root.value("announcements", nlohmann::json::array()))
        {
            ConfiguredAnnouncement announcement;
            const ConfiguredSound sound = ParseSound(announcementValue);
            announcement.id = sound.id;
            announcement.path = sound.path;
            announcement.hour = announcementValue.value("hour", -1);
            announcement.minute = announcementValue.value("minute", -1);
            if (!announcement.id.empty() && !announcement.path.empty())
                config.announcements.push_back(std::move(announcement));
        }

        if (root.contains("cinema"))
        {
            const auto& cinema = root["cinema"];
            config.cinema.enabled = cinema.value("enabled", false);
            for (const auto& schedule : cinema.value("schedules", nlohmann::json::array()))
                config.cinema.schedules.push_back(ParseCinemaSchedule(schedule));

            if (cinema.contains("resume"))
            {
                const auto& resume = cinema["resume"];
                config.cinema.resume.enabled = resume.value("enabled", true);
                config.cinema.resume.automatic = resume.value("automatic", true);
                config.cinema.resume.maxAgeMinutes = resume.value("maxAgeMinutes", 720);
                config.cinema.resume.checkpointIntervalSeconds =
                    resume.value("checkpointIntervalSeconds", 1);
            }
            if (cinema.contains("safety"))
            {
                const auto& safety = cinema["safety"];
                config.cinema.safety.enabled = safety.value("enabled", false);
                config.cinema.safety.evacuationAnnouncementId =
                    safety.value("evacuationAnnouncementId", "");
                if (safety.contains("interlock"))
                {
                    const auto& interlock = safety["interlock"];
                    config.cinema.safety.interlock.enabled =
                        interlock.value("enabled", false);
                    config.cinema.safety.interlock.expectedHeartbeatSource =
                        interlock.value("expectedHeartbeatSource", "");
                    config.cinema.safety.interlock.heartbeatTimeoutMilliseconds =
                        interlock.value("heartbeatTimeoutMilliseconds", 5000);
                    config.cinema.safety.interlock.startupGraceMilliseconds =
                        interlock.value("startupGraceMilliseconds", 10000);
                    config.cinema.safety.interlock.safeStableMilliseconds =
                        interlock.value("safeStableMilliseconds", 1000);
                }
            }
        }
        return true;
    }
    catch (const std::exception& error)
    {
        config = std::move(previousConfig);
        errorMessage = error.what();
        return false;
    }
}

bool ValidateAppConfigDocument(
    const nlohmann::json& root,
    std::vector<ConfigValidationIssue>& issues)
{
    issues.clear();

    if (!root.is_object())
    {
        AddIssue(issues, "$", "Configuration root must be an object.");
        return false;
    }

    if (root.contains("loudnessTargetLufs"))
    {
        if (!root["loudnessTargetLufs"].is_number())
        {
            AddIssue(issues, "$.loudnessTargetLufs", "Value must be a number.");
        }
        else
        {
            const double target = root["loudnessTargetLufs"].get<double>();
            if (!std::isfinite(target) || target < -30.0 || target > -8.0)
            {
                AddIssue(
                    issues, "$.loudnessTargetLufs",
                    "Value must be finite and between -30 and -8 LUFS.");
            }
        }
    }

    std::set<std::string> playlistNames;
    std::set<std::string> playlistsWithTracks;
    std::map<std::string, std::string> soundPaths;
    if (root.contains("playlists") && !root["playlists"].is_array())
    {
        AddIssue(issues, "$.playlists", "Value must be an array.");
    }
    else
    {
        const auto playlists = root.value("playlists", nlohmann::json::array());
        for (std::size_t playlistIndex = 0; playlistIndex < playlists.size(); ++playlistIndex)
        {
            const auto& playlist = playlists[playlistIndex];
            const std::string base = "$.playlists[" + std::to_string(playlistIndex) + "]";
            if (!playlist.is_object())
            {
                AddIssue(issues, base, "Playlist must be an object.");
                continue;
            }
            if (!IsNonEmptyString(playlist, "name"))
            {
                AddIssue(issues, base + ".name", "Playlist name must be a non-empty string.");
            }
            else
            {
                const std::string name = playlist["name"].get<std::string>();
                if (!playlistNames.insert(name).second)
                    AddIssue(issues, base + ".name", "Playlist name must be unique.");
            }

            if (playlist.contains("options"))
            {
                if (!playlist["options"].is_object())
                {
                    AddIssue(issues, base + ".options", "Options must be an object.");
                }
                else if (playlist["options"].contains("segmentDuration"))
                {
                    const auto& duration = playlist["options"]["segmentDuration"];
                    if (!duration.is_number() || !std::isfinite(duration.get<double>()) ||
                        duration.get<double>() <= 0.0)
                    {
                        AddIssue(
                            issues, base + ".options.segmentDuration",
                            "Segment duration must be a finite positive number.");
                    }
                }
                for (const char* key : {"randomOrder", "randomSegment", "loopPlaylist"})
                {
                    if (playlist["options"].contains(key) &&
                        !playlist["options"][key].is_boolean())
                    {
                        AddIssue(
                            issues, base + ".options." + key,
                            "Value must be a boolean.");
                    }
                }
            }

            if (!playlist.contains("tracks") || !playlist["tracks"].is_array())
            {
                AddIssue(issues, base + ".tracks", "Tracks must be an array.");
                continue;
            }
            for (std::size_t trackIndex = 0; trackIndex < playlist["tracks"].size(); ++trackIndex)
            {
                const auto& track = playlist["tracks"][trackIndex];
                const std::string trackBase =
                    base + ".tracks[" + std::to_string(trackIndex) + "]";
                if (!track.is_object() || !IsNonEmptyString(track, "id") ||
                    !IsNonEmptyString(track, "path"))
                {
                    AddIssue(
                        issues, trackBase,
                        "Track must contain non-empty string fields 'id' and 'path'.");
                    continue;
                }
                const std::string id = track["id"].get<std::string>();
                const std::string path = track["path"].get<std::string>();
                if (playlist.contains("name") && playlist["name"].is_string() &&
                    !playlist["name"].get_ref<const std::string&>().empty())
                {
                    playlistsWithTracks.insert(playlist["name"].get<std::string>());
                }
                const auto [it, inserted] = soundPaths.emplace(id, path);
                if (!inserted && it->second != path)
                {
                    AddIssue(
                        issues, trackBase + ".id",
                        "A sound ID cannot refer to multiple paths.");
                }
            }
        }
    }

    if (root.contains("wedding") && !root["wedding"].is_object())
    {
        AddIssue(issues, "$.wedding", "Value must be an object.");
    }
    else if (root.contains("wedding"))
    {
        const auto& wedding = root["wedding"];
        for (const char* key : {"entrance", "ceremony", "exit"})
        {
            if (wedding.contains(key) && !wedding[key].is_string())
                AddIssue(issues, std::string("$.wedding.") + key, "Value must be a string.");
        }
        const std::pair<const char*, const char*> weddingSounds[] = {
            {"entrance", "wedding_entrance_sound"},
            {"ceremony", "wedding_ceremony_sound"},
            {"exit", "wedding_exit_sound"}
        };
        for (const auto& [key, id] : weddingSounds)
        {
            if (!wedding.contains(key) || !wedding[key].is_string() ||
                wedding[key].get_ref<const std::string&>().empty())
                continue;
            if (!soundPaths.emplace(id, wedding[key].get<std::string>()).second)
            {
                AddIssue(
                    issues, std::string("$.wedding.") + key,
                    "Wedding asset conflicts with another sound ID.");
            }
        }
        if (wedding.contains("transitionSfx"))
        {
            const auto& sfx = wedding["transitionSfx"];
            if (!sfx.is_object() || !IsNonEmptyString(sfx, "id") || !IsNonEmptyString(sfx, "path"))
            {
                AddIssue(
                    issues, "$.wedding.transitionSfx",
                    "Transition SFX must contain non-empty string fields 'id' and 'path'.");
            }
            else
            {
                const std::string id = sfx["id"].get<std::string>();
                const std::string path = sfx["path"].get<std::string>();
                if (!soundPaths.emplace(id, path).second)
                    AddIssue(
                        issues, "$.wedding.transitionSfx.id",
                        "Transition SFX ID must not conflict with another sound ID.");
            }
        }
    }

    std::set<std::string> announcementIds;
    if (root.contains("announcements") && !root["announcements"].is_array())
    {
        AddIssue(issues, "$.announcements", "Value must be an array.");
    }
    else
    {
        const auto announcements = root.value("announcements", nlohmann::json::array());
        for (std::size_t index = 0; index < announcements.size(); ++index)
        {
            const auto& announcement = announcements[index];
            const std::string base = "$.announcements[" + std::to_string(index) + "]";
            if (!announcement.is_object() || !IsNonEmptyString(announcement, "id") ||
                !IsNonEmptyString(announcement, "path"))
            {
                AddIssue(
                    issues, base,
                    "Announcement must contain non-empty string fields 'id' and 'path'.");
                continue;
            }
            const std::string id = announcement["id"].get<std::string>();
            if (!announcementIds.insert(id).second)
                AddIssue(issues, base + ".id", "Announcement ID must be unique.");
            const std::string announcementPath = announcement["path"].get<std::string>();
            if (!soundPaths.emplace(id, announcementPath).second)
                AddIssue(
                    issues, base + ".id",
                    "Announcement ID must not conflict with another sound ID.");

            const bool hasHour = announcement.contains("hour");
            const bool hasMinute = announcement.contains("minute");
            if (hasHour != hasMinute)
            {
                AddIssue(issues, base, "Schedule must provide both 'hour' and 'minute'.");
            }
            else if (hasHour)
            {
                if (!IsNonNegativeIntegerAtMost(announcement["hour"], 23))
                    AddIssue(issues, base + ".hour", "Hour must be an integer from 0 to 23.");
                if (!IsNonNegativeIntegerAtMost(announcement["minute"], 59))
                    AddIssue(issues, base + ".minute", "Minute must be an integer from 0 to 59.");
            }
        }
    }

    if (root.contains("cinema") && !root["cinema"].is_object())
    {
        AddIssue(issues, "$.cinema", "Value must be an object.");
    }
    else if (root.contains("cinema"))
    {
        const auto& cinema = root["cinema"];
        if (cinema.contains("enabled") && !cinema["enabled"].is_boolean())
            AddIssue(issues, "$.cinema.enabled", "Value must be a boolean.");

        std::set<std::string> scheduleIds;
        if (cinema.contains("schedules") && !cinema["schedules"].is_array())
        {
            AddIssue(issues, "$.cinema.schedules", "Value must be an array.");
        }
        else
        {
            const auto schedules = cinema.value("schedules", nlohmann::json::array());
            if (schedules.size() > 1024)
                AddIssue(issues, "$.cinema.schedules", "At most 1024 schedules are allowed.");
            for (std::size_t index = 0; index < schedules.size(); ++index)
            {
                const auto& value = schedules[index];
                const std::string base = "$.cinema.schedules[" + std::to_string(index) + "]";
                if (!value.is_object())
                {
                    AddIssue(issues, base, "Cinema schedule must be an object.");
                    continue;
                }

                bool shapeValid = true;
                if (!IsNonEmptyString(value, "id"))
                {
                    AddIssue(issues, base + ".id", "Schedule ID must be a non-empty string.");
                    shapeValid = false;
                }
                else if (!scheduleIds.insert(value["id"].get<std::string>()).second)
                {
                    AddIssue(issues, base + ".id", "Schedule ID must be unique.");
                }
                if (!IsNonEmptyString(value, "playlist"))
                {
                    AddIssue(
                        issues, base + ".playlist",
                        "Schedule playlist must be a non-empty string.");
                    shapeValid = false;
                }
                else if (!playlistNames.contains(value["playlist"].get<std::string>()))
                {
                    AddIssue(
                        issues, base + ".playlist",
                        "Schedule playlist must reference a configured playlist.");
                }
                else if (!playlistsWithTracks.contains(
                             value["playlist"].get<std::string>()))
                {
                    AddIssue(
                        issues, base + ".playlist",
                        "Schedule playlist must contain at least one configured track.");
                }
                if (value.contains("enabled") && !value["enabled"].is_boolean())
                {
                    AddIssue(issues, base + ".enabled", "Value must be a boolean.");
                    shapeValid = false;
                }
                if (value.contains("priority") &&
                    !IsIntegerInRange(value["priority"], -1000, 1000))
                {
                    AddIssue(
                        issues, base + ".priority",
                        "Priority must be an integer between -1000 and 1000.");
                    shapeValid = false;
                }

                CinemaPeriodKind periodKind = CinemaPeriodKind::Always;
                if (value.contains("period"))
                {
                    const auto& period = value["period"];
                    if (!period.is_object())
                    {
                        AddIssue(issues, base + ".period", "Period must be an object.");
                        shapeValid = false;
                    }
                    else if (!period.contains("type") || !period["type"].is_string() ||
                             !TryParseCinemaPeriodKind(
                                 period.value("type", ""), periodKind))
                    {
                        AddIssue(
                            issues, base + ".period.type",
                            "Type must be always, date, date-range, annual-date, "
                            "annual-range, or easter-range.");
                        shapeValid = false;
                    }
                    else
                    {
                        const auto validateDateField = [&](const char* field, bool annual)
                        {
                            if (!period.contains(field) || !period[field].is_string())
                            {
                                AddIssue(
                                    issues, base + ".period." + field,
                                    annual ? "Value must use MM-DD." : "Value must use YYYY-MM-DD.");
                                shapeValid = false;
                                return;
                            }
                            CinemaDate parsed;
                            const bool valid = annual
                                ? ParseCinemaMonthDayText(period[field].get<std::string>(), parsed)
                                : ParseCinemaDateText(period[field].get<std::string>(), parsed);
                            if (!valid)
                            {
                                AddIssue(
                                    issues, base + ".period." + field,
                                    annual ? "Value must be a valid MM-DD date."
                                           : "Value must be a valid YYYY-MM-DD date.");
                                shapeValid = false;
                            }
                        };

                        switch (periodKind)
                        {
                            case CinemaPeriodKind::AbsoluteDate:
                                validateDateField("date", false);
                                break;
                            case CinemaPeriodKind::AbsoluteRange:
                                validateDateField("start", false);
                                validateDateField("end", false);
                                break;
                            case CinemaPeriodKind::AnnualDate:
                                validateDateField("date", true);
                                break;
                            case CinemaPeriodKind::AnnualRange:
                                validateDateField("start", true);
                                validateDateField("end", true);
                                break;
                            case CinemaPeriodKind::EasterRange:
                                if (!period.contains("startOffsetDays") ||
                                    !IsIntegerInRange(period["startOffsetDays"], -366, 366))
                                {
                                    AddIssue(
                                        issues, base + ".period.startOffsetDays",
                                        "Value must be an integer between -366 and 366.");
                                    shapeValid = false;
                                }
                                if (!period.contains("endOffsetDays") ||
                                    !IsIntegerInRange(period["endOffsetDays"], -366, 366))
                                {
                                    AddIssue(
                                        issues, base + ".period.endOffsetDays",
                                        "Value must be an integer between -366 and 366.");
                                    shapeValid = false;
                                }
                                break;
                            case CinemaPeriodKind::Always:
                                break;
                        }
                    }
                }

                if (value.contains("window"))
                {
                    const auto& window = value["window"];
                    if (!window.is_object())
                    {
                        AddIssue(issues, base + ".window", "Window must be an object.");
                        shapeValid = false;
                    }
                    else
                    {
                        for (const char* field : {"start", "end"})
                        {
                            int minute = 0;
                            if (!window.contains(field) || !window[field].is_string() ||
                                !ParseCinemaTimeText(window.value(field, ""), minute))
                            {
                                AddIssue(
                                    issues, base + ".window." + field,
                                    "Value must use 24-hour HH:MM local time.");
                                shapeValid = false;
                            }
                        }
                    }
                }

                if (value.contains("weekdays"))
                {
                    if (!value["weekdays"].is_array())
                    {
                        AddIssue(
                            issues, base + ".weekdays",
                            "Weekdays must be an array of ISO weekday integers.");
                        shapeValid = false;
                    }
                    else
                    {
                        std::set<int> weekdays;
                        for (std::size_t weekdayIndex = 0;
                             weekdayIndex < value["weekdays"].size(); ++weekdayIndex)
                        {
                            const auto& weekday = value["weekdays"][weekdayIndex];
                            if (!IsIntegerInRange(weekday, 1, 7))
                            {
                                AddIssue(
                                    issues,
                                    base + ".weekdays[" + std::to_string(weekdayIndex) + "]",
                                    "ISO weekday must be 1 (Monday) through 7 (Sunday).");
                                shapeValid = false;
                            }
                            else if (!weekdays.insert(weekday.get<int>()).second)
                            {
                                AddIssue(
                                    issues,
                                    base + ".weekdays[" + std::to_string(weekdayIndex) + "]",
                                    "ISO weekday must not be duplicated.");
                                shapeValid = false;
                            }
                        }
                    }
                }

                if (shapeValid)
                {
                    std::string scheduleError;
                    if (!ValidateCinemaSchedule(ParseCinemaSchedule(value), scheduleError))
                        AddIssue(issues, base, scheduleError);
                }
            }
        }

        if (cinema.contains("resume"))
        {
            const auto& resume = cinema["resume"];
            if (!resume.is_object())
            {
                AddIssue(issues, "$.cinema.resume", "Value must be an object.");
            }
            else
            {
                for (const char* key : {"enabled", "automatic"})
                {
                    if (resume.contains(key) && !resume[key].is_boolean())
                        AddIssue(
                            issues, std::string("$.cinema.resume.") + key,
                            "Value must be a boolean.");
                }
                if (resume.contains("maxAgeMinutes") &&
                    !IsIntegerInRange(resume["maxAgeMinutes"], 1, 10080))
                    AddIssue(
                        issues, "$.cinema.resume.maxAgeMinutes",
                        "Value must be an integer from 1 to 10080.");
                if (resume.contains("checkpointIntervalSeconds") &&
                    !IsIntegerInRange(resume["checkpointIntervalSeconds"], 1, 60))
                    AddIssue(
                        issues, "$.cinema.resume.checkpointIntervalSeconds",
                        "Value must be an integer from 1 to 60.");
            }
        }

        if (cinema.contains("safety"))
        {
            const auto& safety = cinema["safety"];
            if (!safety.is_object())
            {
                AddIssue(issues, "$.cinema.safety", "Value must be an object.");
            }
            else
            {
                const bool safetyEnabled =
                    safety.contains("enabled") && safety["enabled"].is_boolean()
                        ? safety["enabled"].get<bool>()
                        : false;
                if (safety.contains("enabled") && !safety["enabled"].is_boolean())
                    AddIssue(issues, "$.cinema.safety.enabled", "Value must be a boolean.");
                if (!safety.contains("evacuationAnnouncementId"))
                {
                    if (safetyEnabled)
                        AddIssue(
                            issues, "$.cinema.safety.evacuationAnnouncementId",
                            "A non-empty evacuation announcement is required when safety is enabled.");
                }
                else if (!safety["evacuationAnnouncementId"].is_string())
                {
                    AddIssue(
                        issues, "$.cinema.safety.evacuationAnnouncementId",
                        "Value must be a string.");
                }
                else
                {
                    const std::string id =
                        safety["evacuationAnnouncementId"].get<std::string>();
                    if (safetyEnabled && id.empty())
                        AddIssue(
                            issues, "$.cinema.safety.evacuationAnnouncementId",
                            "A non-empty evacuation announcement is required when safety is enabled.");
                    else if (!id.empty() && !announcementIds.contains(id))
                        AddIssue(
                            issues, "$.cinema.safety.evacuationAnnouncementId",
                            "Value must reference a configured announcement.");
                    else if (safetyEnabled && !id.empty() &&
                             root.contains("announcements") &&
                             root["announcements"].is_array())
                    {
                        const auto scheduledEmergency = std::find_if(
                            root["announcements"].begin(),
                            root["announcements"].end(),
                            [&](const nlohmann::json& announcement) {
                                return announcement.is_object() &&
                                    announcement.contains("id") &&
                                    announcement["id"].is_string() &&
                                    announcement["id"].get_ref<const std::string&>() == id &&
                                    (announcement.contains("hour") ||
                                     announcement.contains("minute"));
                            });
                        if (scheduledEmergency != root["announcements"].end())
                            AddIssue(
                                issues,
                                "$.cinema.safety.evacuationAnnouncementId",
                                "The protected evacuation preset cannot also be a daily announcement.");
                    }
                }
                if (safety.contains("interlock"))
                {
                    const auto& interlock = safety["interlock"];
                    if (!interlock.is_object())
                    {
                        AddIssue(
                            issues, "$.cinema.safety.interlock",
                            "Value must be an object.");
                    }
                    else
                    {
                        const bool interlockEnabled =
                            interlock.contains("enabled") && interlock["enabled"].is_boolean()
                                ? interlock["enabled"].get<bool>()
                                : false;
                        if (interlock.contains("enabled") && !interlock["enabled"].is_boolean())
                            AddIssue(
                                issues, "$.cinema.safety.interlock.enabled",
                                "Value must be a boolean.");
                        if (interlockEnabled && !safetyEnabled)
                            AddIssue(
                                issues, "$.cinema.safety.interlock.enabled",
                                "Safety must be enabled when the interlock is enabled.");
                        if (interlock.contains("expectedHeartbeatSource") &&
                            !interlock["expectedHeartbeatSource"].is_string())
                        {
                            AddIssue(
                                issues,
                                "$.cinema.safety.interlock.expectedHeartbeatSource",
                                "Value must be a string.");
                        }
                        else
                        {
                            const std::string source =
                                interlock.value("expectedHeartbeatSource", "");
                            if (interlockEnabled && source.empty())
                            {
                                AddIssue(
                                    issues,
                                    "$.cinema.safety.interlock.expectedHeartbeatSource",
                                    "A non-empty authorized heartbeat source is required when the interlock is enabled.");
                            }
                            else if (source.size() > 128 ||
                                     std::any_of(
                                         source.begin(), source.end(),
                                         [](unsigned char character) {
                                             return character < 0x20 || character == 0x7f;
                                         }))
                            {
                                AddIssue(
                                    issues,
                                    "$.cinema.safety.interlock.expectedHeartbeatSource",
                                    "Heartbeat source must be at most 128 characters without control characters.");
                            }
                        }
                        const std::pair<const char*, std::pair<int, int>> ranges[] = {
                            {"heartbeatTimeoutMilliseconds", {100, 60000}},
                            {"startupGraceMilliseconds", {0, 300000}},
                            {"safeStableMilliseconds", {0, 60000}}
                        };
                        for (const auto& [key, range] : ranges)
                        {
                            if (interlock.contains(key) &&
                                !IsIntegerInRange(interlock[key], range.first, range.second))
                            {
                                AddIssue(
                                    issues, std::string("$.cinema.safety.interlock.") + key,
                                    "Value is outside the supported millisecond range.");
                            }
                        }
                    }
                }
            }
        }
    }

    return issues.empty();
}

} // namespace

bool LoadAppConfig(
    const std::string& filePath, AppConfig& config, std::string& errorMessage)
{
    nlohmann::json root;
    errorMessage.clear();
    return ReadConfigDocument(filePath, root, errorMessage) &&
           ParseAppConfigDocument(root, config, errorMessage);
}

bool ValidateAppConfig(
    const std::string& filePath,
    std::vector<ConfigValidationIssue>& issues,
    std::string& errorMessage)
{
    nlohmann::json root;
    errorMessage.clear();
    if (!ReadConfigDocument(filePath, root, errorMessage))
    {
        issues.clear();
        return false;
    }
    return ValidateAppConfigDocument(root, issues);
}

bool LoadValidatedAppConfig(
    const std::string& filePath,
    AppConfig& config,
    std::vector<ConfigValidationIssue>& issues,
    std::string& errorMessage)
{
    nlohmann::json root;
    errorMessage.clear();
    if (!ReadConfigDocument(filePath, root, errorMessage))
    {
        issues.clear();
        return false;
    }
    if (!ValidateAppConfigDocument(root, issues)) return false;
    return ParseAppConfigDocument(root, config, errorMessage);
}

} // namespace TSM
