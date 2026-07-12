#include "tsm_config.h"

#include <fstream>
#include <json/json.hpp>
#include <utility>

namespace TSM
{

namespace
{
ConfiguredSound ParseSound(const nlohmann::json& value)
{
    return {value.value("id", ""), value.value("path", "")};
}
}

bool LoadAppConfig(const std::string& filePath, AppConfig& config, std::string& errorMessage)
{
    try
    {
        std::ifstream input(filePath);
        if (!input.is_open())
        {
            errorMessage = "Unable to open " + filePath;
            return false;
        }

        nlohmann::json root;
        input >> root;
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
        return true;
    }
    catch (const std::exception& error)
    {
        errorMessage = error.what();
        return false;
    }
}

} // namespace TSM
