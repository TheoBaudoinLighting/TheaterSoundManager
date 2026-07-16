#pragma once

#include "tsm_cinema_schedule.h"
#include "tsm_playlist_manager.h"
#include <string>
#include <vector>

namespace TSM
{

struct ConfiguredSound
{
    std::string id;
    std::string path;
};

struct ConfiguredPlaylist
{
    std::string name;
    PlaylistOptions options;
    std::vector<ConfiguredSound> tracks;
};

struct ConfiguredAnnouncement : ConfiguredSound
{
    int hour = -1;
    int minute = -1;
};

struct WeddingConfig
{
    std::string entrance;
    std::string ceremony;
    std::string exit;
    ConfiguredSound transitionSfx;
};

struct CinemaResumeConfig
{
    bool enabled = true;
    bool automatic = true;
    int maxAgeMinutes = 720;
    int checkpointIntervalSeconds = 1;
};

struct CinemaInterlockConfig
{
    bool enabled = false;
    std::string expectedHeartbeatSource;
    int heartbeatTimeoutMilliseconds = 5000;
    int startupGraceMilliseconds = 10000;
    int safeStableMilliseconds = 1000;
};

struct CinemaSafetyConfig
{
    bool enabled = false;
    std::string evacuationAnnouncementId;
    CinemaInterlockConfig interlock;
};

struct CinemaConfig
{
    bool enabled = false;
    std::vector<CinemaSchedule> schedules;
    CinemaResumeConfig resume;
    CinemaSafetyConfig safety;
};

struct AppConfig
{
    float loudnessTargetLufs = -16.0f;
    std::vector<ConfiguredPlaylist> playlists;
    std::vector<ConfiguredAnnouncement> announcements;
    WeddingConfig wedding;
    CinemaConfig cinema;
};

struct ConfigValidationIssue
{
    std::string path;
    std::string message;
};

bool LoadAppConfig(const std::string& filePath, AppConfig& config, std::string& errorMessage);
bool ValidateAppConfig(
    const std::string& filePath,
    std::vector<ConfigValidationIssue>& issues,
    std::string& errorMessage);
bool LoadValidatedAppConfig(
    const std::string& filePath,
    AppConfig& config,
    std::vector<ConfigValidationIssue>& issues,
    std::string& errorMessage);

} // namespace TSM
