#pragma once

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

struct AppConfig
{
    float loudnessTargetLufs = -16.0f;
    std::vector<ConfiguredPlaylist> playlists;
    std::vector<ConfiguredAnnouncement> announcements;
    WeddingConfig wedding;
};

bool LoadAppConfig(const std::string& filePath, AppConfig& config, std::string& errorMessage);

} // namespace TSM
