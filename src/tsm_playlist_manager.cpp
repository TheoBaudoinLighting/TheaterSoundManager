// tsm_playlist_manager.cpp

#include "tsm_playlist_manager.h"
#include "tsm_audio_manager.h"
#include "tsm_fmod_wrapper.h"
#include <fstream>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <random>
#include <ctime>
#include <cmath>
#include <limits>
#include <filesystem>
#include <optional>
#include <set>

namespace TSM
{
    using json = nlohmann::json;

namespace
{
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

std::string ResolveImportedTrackPath(
    const std::string& trackPath, const std::string& playlistFilePath)
{
    std::filesystem::path path = PathFromUtf8(trackPath);
    if (path.is_relative())
        path = PathFromUtf8(playlistFilePath).parent_path() / path;
    return PathToUtf8(path.lexically_normal());
}

struct ImportedTrack
{
    std::string id;
    std::optional<std::string> path;
};

struct ImportedPlaylist
{
    std::string name;
    PlaylistOptions options = [] {
        PlaylistOptions defaults;
        defaults.randomOrder = true;
        defaults.randomSegment = true;
        defaults.loopPlaylist = true;
        defaults.segmentDuration = PlaylistOptions::DefaultSegmentDuration;
        return defaults;
    }();
    float crossfadeDuration = 10.0f;
    std::vector<ImportedTrack> tracks;
};

bool ReadBooleanOption(
    const json& options, const char* key, bool& destination, std::string& errorMessage)
{
    if (!options.contains(key)) return true;
    if (!options[key].is_boolean())
    {
        errorMessage = std::string("Playlist option '") + key + "' must be a boolean.";
        return false;
    }
    destination = options[key].get<bool>();
    return true;
}

bool ReadDurationOption(
    const json& options, const char* key, double minimum, bool minimumInclusive,
    double maximum, float& destination, std::string& errorMessage)
{
    if (!options.contains(key)) return true;
    const json& value = options[key];
    if (!value.is_number())
    {
        errorMessage = std::string("Playlist option '") + key + "' must be a number.";
        return false;
    }

    const double duration = value.get<double>();
    const bool belowMinimum = minimumInclusive ? duration < minimum : duration <= minimum;
    if (!std::isfinite(duration) || belowMinimum || duration > maximum)
    {
        errorMessage = std::string("Playlist option '") + key +
            "' is outside the supported range.";
        return false;
    }
    destination = static_cast<float>(duration);
    return true;
}

bool ParseImportedPlaylist(
    const json& document, const std::string& nameOverride,
    ImportedPlaylist& imported, std::string& errorMessage)
{
    if (!document.is_object())
    {
        errorMessage = "A playlist document must be a JSON object.";
        return false;
    }

    if (!nameOverride.empty())
    {
        imported.name = nameOverride;
    }
    else
    {
        if (!document.contains("name") || !document["name"].is_string())
        {
            errorMessage = "A playlist must contain a string 'name'.";
            return false;
        }
        imported.name = document["name"].get<std::string>();
    }
    if (imported.name.empty())
    {
        errorMessage = "A playlist name cannot be empty.";
        return false;
    }

    if (document.contains("options"))
    {
        const json& options = document["options"];
        if (!options.is_object())
        {
            errorMessage = "Playlist 'options' must be a JSON object.";
            return false;
        }
        if (!ReadBooleanOption(
                options, "randomOrder", imported.options.randomOrder, errorMessage) ||
            !ReadBooleanOption(
                options, "randomSegment", imported.options.randomSegment, errorMessage) ||
            !ReadBooleanOption(
                options, "loopPlaylist", imported.options.loopPlaylist, errorMessage) ||
            !ReadDurationOption(
                options, "segmentDuration", 0.0, false, 86400.0,
                imported.options.segmentDuration, errorMessage) ||
            !ReadDurationOption(
                options, "crossfadeDuration", 0.0, true, 3600.0,
                imported.crossfadeDuration, errorMessage))
        {
            return false;
        }
    }

    if (!document.contains("tracks") || !document["tracks"].is_array())
    {
        errorMessage = "A playlist must contain a 'tracks' array.";
        return false;
    }

    imported.tracks.clear();
    imported.tracks.reserve(document["tracks"].size());
    for (std::size_t index = 0; index < document["tracks"].size(); ++index)
    {
        const json& trackDocument = document["tracks"][index];
        if (!trackDocument.is_object() || !trackDocument.contains("id") ||
            !trackDocument["id"].is_string())
        {
            errorMessage = "Playlist track " + std::to_string(index) +
                " must contain a string 'id'.";
            return false;
        }

        ImportedTrack track;
        track.id = trackDocument["id"].get<std::string>();
        if (track.id.empty())
        {
            errorMessage = "Playlist track " + std::to_string(index) +
                " has an empty id.";
            return false;
        }
        if (trackDocument.contains("path"))
        {
            if (!trackDocument["path"].is_string())
            {
                errorMessage = "Playlist track " + std::to_string(index) +
                    " has a non-string path.";
                return false;
            }
            std::string path = trackDocument["path"].get<std::string>();
            if (path.empty())
            {
                errorMessage = "Playlist track " + std::to_string(index) +
                    " has an empty path.";
                return false;
            }
            track.path = std::move(path);
        }
        imported.tracks.push_back(std::move(track));
    }
    return true;
}

class NewlyLoadedSoundRollback
{
public:
    void Add(std::string id) { m_soundIds.push_back(std::move(id)); }
    void Dismiss() noexcept { m_active = false; }

    ~NewlyLoadedSoundRollback()
    {
        if (!m_active) return;
        auto& audioManager = AudioManager::GetInstance();
        for (auto it = m_soundIds.rbegin(); it != m_soundIds.rend(); ++it)
        {
            try
            {
                if (!audioManager.UnloadSound(*it))
                    spdlog::warn("Unable to roll back newly loaded sound '{}'.", *it);
            }
            catch (const std::exception& error)
            {
                spdlog::warn(
                    "Exception while rolling back newly loaded sound '{}': {}",
                    *it, error.what());
            }
            catch (...)
            {
                spdlog::warn(
                    "Unknown exception while rolling back newly loaded sound '{}'.", *it);
            }
        }
    }

private:
    std::vector<std::string> m_soundIds;
    bool m_active = true;
};

bool LoadImportedTracks(
    const ImportedPlaylist& imported, const std::string& playlistFilePath,
    NewlyLoadedSoundRollback& rollback, std::string& errorMessage)
{
    auto& audioManager = AudioManager::GetInstance();
    for (const ImportedTrack& track : imported.tracks)
    {
        auto loaded = audioManager.GetAllSounds().find(track.id);
        if (loaded == audioManager.GetAllSounds().end())
        {
            if (!track.path)
            {
                errorMessage = "Track '" + track.id +
                    "' is not loaded and has no path in the playlist document.";
                return false;
            }
            const std::string resolvedPath = ResolveImportedTrackPath(
                *track.path, playlistFilePath);
            if (!audioManager.LoadSound(
                    track.id, resolvedPath, true, AudioManager::SoundKind::Music))
            {
                errorMessage = "Unable to load playlist track '" + track.id + "'.";
                return false;
            }
            rollback.Add(track.id);
            loaded = audioManager.GetAllSounds().find(track.id);
        }

        if (loaded == audioManager.GetAllSounds().end() ||
            loaded->second.kind != AudioManager::SoundKind::Music)
        {
            errorMessage = "Track '" + track.id + "' is not classified as music.";
            return false;
        }
    }
    return true;
}
}

PlaylistManager::PlaylistManager()
    : m_rng(std::random_device{}())
{
    m_libraryPlayback.name = "No playlist";
    m_libraryPlayback.options.randomOrder = true;
    m_libraryPlayback.options.randomSegment = true;
    m_libraryPlayback.options.loopPlaylist = true;
    m_libraryPlayback.options.segmentDuration = PlaylistOptions::DefaultSegmentDuration;
}

void PlaylistManager::CreatePlaylist(const std::string& playlistName)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it == m_playlists.end())
    {
        Playlist newPlaylist;
        newPlaylist.name = playlistName;
        
        // Paramètres par défaut
        newPlaylist.options.randomOrder = true;
        newPlaylist.options.randomSegment = true;
        newPlaylist.options.loopPlaylist = true;
        newPlaylist.options.segmentDuration = PlaylistOptions::DefaultSegmentDuration;
        
        m_playlists.push_back(newPlaylist);
        spdlog::info("Playlist '{}' created.", playlistName);
        NotifyPlaylistChanged();
    }
    else
    {
        spdlog::error("Playlist '{}' already exists.", playlistName);
    }
}

void PlaylistManager::DeletePlaylist(const std::string& playlistName)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it != m_playlists.end())
    {
        if (it->isPlaying)
        {
            Stop(playlistName);
        }
        
        m_playlists.erase(it);
        
        if (m_activePlaylistName == playlistName)
        {
            m_activePlaylistName = "";
        }
        
        spdlog::info("Playlist '{}' deleted.", playlistName);
        NotifyPlaylistChanged();
    }
    else
    {
        spdlog::error("Playlist '{}' not found.", playlistName);
    }
}

void PlaylistManager::RenamePlaylist(const std::string& oldName, const std::string& newName)
{
    if (oldName == newName) return;
    
    auto existingNew = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&newName](const Playlist& p) { return p.name == newName; });

    if (existingNew != m_playlists.end())
    {
        spdlog::error("Cannot rename playlist. Target name '{}' already exists.", newName);
        return;
    }
    
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&oldName](const Playlist& p) { return p.name == oldName; });

    if (it != m_playlists.end())
    {
        it->name = newName;
        
        if (m_activePlaylistName == oldName)
        {
            m_activePlaylistName = newName;
        }
        
        spdlog::info("Playlist renamed from '{}' to '{}'.", oldName, newName);
        NotifyPlaylistChanged();
    }
    else
    {
        spdlog::error("Playlist '{}' not found.", oldName);
    }
}

void PlaylistManager::DuplicatePlaylist(const std::string& sourceName, const std::string& destName)
{
    auto sourceIt = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&sourceName](const Playlist& p) { return p.name == sourceName; });

    if (sourceIt == m_playlists.end())
    {
        spdlog::error("Source playlist '{}' not found.", sourceName);
        return;
    }
    
    auto destIt = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&destName](const Playlist& p) { return p.name == destName; });

    if (destIt != m_playlists.end())
    {
        spdlog::error("Destination playlist '{}' already exists.", destName);
        return;
    }
    
    Playlist newPlaylist = *sourceIt;
    newPlaylist.name = destName;
    newPlaylist.isPlaying = false;
    newPlaylist.currentChannel = nullptr;
    newPlaylist.nextChannel = nullptr;
    newPlaylist.expectedCurrentSound = nullptr;
    newPlaylist.expectedNextSound = nullptr;
    newPlaylist.nextIndex = -1;
    newPlaylist.isCrossfading = false;
    
    m_playlists.push_back(newPlaylist);
    
    spdlog::info("Playlist '{}' duplicated to '{}'.", sourceName, destName);
    NotifyPlaylistChanged();
}

void PlaylistManager::AddToPlaylist(const std::string& playlistName, const std::string& soundName)
{
    const auto& sounds = AudioManager::GetInstance().GetAllSounds();
    const auto sound = sounds.find(soundName);
    if (sound != sounds.end() && sound->second.kind != AudioManager::SoundKind::Music)
    {
        spdlog::error(
            "Sound '{}' is not classified as music and cannot be added to a playlist.",
            soundName);
        return;
    }
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it != m_playlists.end())
    {
        it->tracks.push_back(soundName);
        AudioManager::GetInstance().QueueLoudnessAnalysis(soundName);
        spdlog::info("Added '{}' to playlist '{}'.", soundName, playlistName);
        NotifyPlaylistChanged();
    }
    else
    {
        spdlog::error("Playlist '{}' not found.", playlistName);
    }
}

void PlaylistManager::RemoveFromPlaylist(const std::string& playlistName, const std::string& soundName)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it != m_playlists.end())
    {
        if (it->isPlaying)
        {
            Stop(playlistName);
        }

        auto& tracks = it->tracks;
        auto trackIt = std::find(tracks.begin(), tracks.end(), soundName);
        
        if (trackIt != tracks.end())
        {
            tracks.erase(trackIt);
            spdlog::info("Removed '{}' from playlist '{}'.", soundName, playlistName);
            NotifyPlaylistChanged();
        }
        else
        {
            spdlog::error("Track '{}' not found in playlist '{}'.", soundName, playlistName);
        }
    }
    else
    {
        spdlog::error("Playlist '{}' not found.", playlistName);
    }
}

void PlaylistManager::RemoveFromPlaylistAtIndex(const std::string& playlistName, size_t index)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it != m_playlists.end())
    {
        if (index < it->tracks.size())
        {
            if (it->isPlaying)
            {
                Stop(playlistName);
            }
            it->tracks.erase(it->tracks.begin() + index);
            spdlog::info("Removed track at index {} from playlist '{}'.", index, playlistName);
            NotifyPlaylistChanged();
        }
        else
        {
            spdlog::error("Index {} out of range for playlist '{}'.", index, playlistName);
        }
    }
    else
    {
        spdlog::error("Playlist '{}' not found.", playlistName);
    }
}

void PlaylistManager::ClearPlaylist(const std::string& playlistName)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it != m_playlists.end())
    {
        if (it->isPlaying)
        {
            Stop(playlistName);
        }
        
        it->tracks.clear();
        spdlog::info("Cleared all tracks from playlist '{}'.", playlistName);
        NotifyPlaylistChanged();
    }
    else
    {
        spdlog::error("Playlist '{}' not found.", playlistName);
    }
}

bool PlaylistManager::ExportPlaylist(const std::string& playlistName, const std::string& filePath)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it == m_playlists.end())
    {
        spdlog::error("Playlist '{}' not found for export.", playlistName);
        return false;
    }
    
    try
    {
        json j;
        j["name"] = it->name;
        j["options"]["randomOrder"] = it->options.randomOrder;
        j["options"]["randomSegment"] = it->options.randomSegment;
        j["options"]["segmentDuration"] = it->options.segmentDuration;
        j["options"]["loopPlaylist"] = it->options.loopPlaylist;
        j["options"]["crossfadeDuration"] = it->crossfadeDuration;
        j["tracks"] = json::array();
        
        const auto& audioManager = AudioManager::GetInstance();
        const auto& allSounds = audioManager.GetAllSounds();
        
        for (const auto& trackId : it->tracks)
        {
            json track;
            track["id"] = trackId;
            
            auto soundIt = allSounds.find(trackId);
            if (soundIt != allSounds.end())
            {
                track["path"] = soundIt->second.filePath;
            }
            
            j["tracks"].push_back(track);
        }
        
        std::ofstream file(PathFromUtf8(filePath));
        if (!file.is_open())
        {
            spdlog::error("Failed to open file '{}' for writing.", filePath);
            return false;
        }
        
        file << j.dump(4);
        file.close();
        
        spdlog::info("Playlist '{}' exported to '{}'.", playlistName, filePath);
        return true;
    }
    catch(const std::exception& e)
    {
        spdlog::error("Error exporting playlist: {}", e.what());
        return false;
    }
}

bool PlaylistManager::ImportPlaylist(const std::string& filePath, const std::string& playlistName)
{
    NewlyLoadedSoundRollback rollback;
    try
    {
        std::ifstream file(PathFromUtf8(filePath));
        if (!file.is_open())
        {
            spdlog::error("Failed to open file '{}' for reading.", filePath);
            return false;
        }
        
        json j;
        file >> j;
        file.close();

        ImportedPlaylist imported;
        std::string errorMessage;
        if (!ParseImportedPlaylist(j, playlistName, imported, errorMessage))
        {
            spdlog::error("Invalid playlist document '{}': {}", filePath, errorMessage);
            return false;
        }
        if (!LoadImportedTracks(imported, filePath, rollback, errorMessage))
        {
            spdlog::error("Unable to import playlist from '{}': {}", filePath, errorMessage);
            return false;
        }

        Playlist replacement;
        replacement.name = imported.name;
        replacement.options = imported.options;
        replacement.crossfadeDuration = imported.crossfadeDuration;
        replacement.tracks.reserve(imported.tracks.size());
        for (const ImportedTrack& track : imported.tracks)
            replacement.tracks.push_back(track.id);

        auto existingIt = std::find_if(m_playlists.begin(), m_playlists.end(),
            [&imported](const Playlist& p) { return p.name == imported.name; });

        if (existingIt != m_playlists.end())
        {
            spdlog::warn("Playlist '{}' already exists. It will be overwritten.", imported.name);
            if (existingIt->isPlaying)
                Stop(imported.name);
            *existingIt = std::move(replacement);
        }
        else
        {
            m_playlists.push_back(std::move(replacement));
        }

        rollback.Dismiss();
        auto& audioManager = AudioManager::GetInstance();
        for (const ImportedTrack& track : imported.tracks)
            audioManager.QueueLoudnessAnalysis(track.id);

        spdlog::info("Playlist '{}' imported from '{}' with {} tracks.",
                    imported.name, filePath, imported.tracks.size());
        NotifyPlaylistChanged();
        return true;
    }
    catch(const std::exception& e)
    {
        spdlog::error("Error importing playlist: {}", e.what());
    }
    
    return false;
}

PlaylistManager::Playlist* PlaylistManager::GetPlaylistByName(const std::string& name)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&name](const Playlist& p) { return p.name == name; });
    
    return (it != m_playlists.end()) ? &(*it) : nullptr;
}

std::vector<std::string> PlaylistManager::GetPlaylistNames() const
{
    std::vector<std::string> names;
    names.reserve(m_playlists.size());
    
    for (const auto& playlist : m_playlists)
    {
        names.push_back(playlist.name);
    }
    
    return names;
}

std::vector<std::string> PlaylistManager::GetLibraryMusicIds() const
{
    std::vector<std::string> musicIds;
    for (const auto& [soundId, sound] : AudioManager::GetInstance().GetAllSounds())
    {
        if (sound.kind == AudioManager::SoundKind::Music && sound.sound)
            musicIds.push_back(soundId);
    }
    return musicIds;
}

const std::vector<std::string>& PlaylistManager::GetLibraryPlaybackSnapshot() const
{
    return m_libraryPlayback.tracks;
}

void PlaylistManager::ConfigureLibraryPlayback(
    const PlaylistOptions& options, float crossfadeDuration)
{
    if (m_libraryPlayback.isPlaying) return;
    m_libraryPlayback.options = options;
    m_libraryPlayback.crossfadeDuration = std::max(crossfadeDuration, 0.0f);
}

bool PlaylistManager::IsLibraryPlaying() const
{
    return m_libraryPlayback.isPlaying;
}

PlaylistOptions PlaylistManager::GetLibraryOptions() const
{
    return m_libraryPlayback.options;
}

float PlaylistManager::GetLibraryCrossfadeDuration() const
{
    return m_libraryPlayback.crossfadeDuration;
}

bool PlaylistManager::IsPlaylistPlaying(const std::string& playlistName) const
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });
    
    return (it != m_playlists.end() && it->isPlaying);
}

size_t PlaylistManager::GetPlaylistTrackCount(const std::string& playlistName) const
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });
    
    return (it != m_playlists.end()) ? it->tracks.size() : 0;
}

void PlaylistManager::MoveTrackToPosition(const std::string& playlistName, int sourceIndex, int targetIndex)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });
    
    if (it == m_playlists.end())
    {
        spdlog::error("Playlist '{}' not found.", playlistName);
        return;
    }
    
    auto& tracks = it->tracks;
    
    if (sourceIndex < 0 || sourceIndex >= static_cast<int>(tracks.size()) ||
        targetIndex < 0 || targetIndex >= static_cast<int>(tracks.size()))
    {
        spdlog::error("Invalid source or target index for track move operation.");
        return;
    }
    
    if (sourceIndex == targetIndex)
    {
        return;
    }

    if (it->isPlaying)
    {
        Stop(playlistName);
    }
    
    std::string trackToMove = tracks[sourceIndex];
    
    tracks.erase(tracks.begin() + sourceIndex);
    
    tracks.insert(tracks.begin() + targetIndex, trackToMove);
    
    spdlog::info("Moved track from index {} to index {} in playlist '{}'.", 
                sourceIndex, targetIndex, playlistName);
    NotifyPlaylistChanged();
}

bool PlaylistManager::SavePlaylistsToFile(const std::string& filePath)
{
    try
    {
        json j = json::array();
        
        for (const auto& playlist : m_playlists)
        {
            json playlistJson;
            playlistJson["name"] = playlist.name;
            playlistJson["options"]["randomOrder"] = playlist.options.randomOrder;
            playlistJson["options"]["randomSegment"] = playlist.options.randomSegment;
            playlistJson["options"]["segmentDuration"] = playlist.options.segmentDuration;
            playlistJson["options"]["loopPlaylist"] = playlist.options.loopPlaylist;
            playlistJson["options"]["crossfadeDuration"] = playlist.crossfadeDuration;
            playlistJson["tracks"] = json::array();
            
            const auto& audioManager = AudioManager::GetInstance();
            const auto& allSounds = audioManager.GetAllSounds();
            
            for (const auto& trackId : playlist.tracks)
            {
                json track;
                track["id"] = trackId;
                
                auto soundIt = allSounds.find(trackId);
                if (soundIt != allSounds.end())
                {
                    track["path"] = soundIt->second.filePath;
                }
                
                playlistJson["tracks"].push_back(track);
            }
            
            j.push_back(playlistJson);
        }
        
        std::ofstream file(PathFromUtf8(filePath));
        if (!file.is_open())
        {
            spdlog::error("Failed to open file '{}' for writing.", filePath);
            return false;
        }
        
        file << j.dump(4);
        file.close();
        
        spdlog::info("All playlists saved to '{}'.", filePath);
        return true;
    }
    catch(const std::exception& e)
    {
        spdlog::error("Error saving playlists: {}", e.what());
        return false;
    }
}

bool PlaylistManager::LoadPlaylistsFromFile(const std::string& filePath)
{
    NewlyLoadedSoundRollback rollback;
    try
    {
        std::ifstream file(PathFromUtf8(filePath));
        if (!file.is_open())
        {
            spdlog::error("Failed to open file '{}' for reading.", filePath);
            return false;
        }
        
        json j;
        file >> j;
        file.close();

        if (!j.is_array())
        {
            spdlog::error("Invalid playlists document '{}': expected a JSON array.", filePath);
            return false;
        }

        std::vector<ImportedPlaylist> importedPlaylists;
        importedPlaylists.reserve(j.size());
        std::set<std::string> playlistNames;
        for (std::size_t index = 0; index < j.size(); ++index)
        {
            ImportedPlaylist imported;
            std::string errorMessage;
            if (!ParseImportedPlaylist(j[index], "", imported, errorMessage))
            {
                spdlog::error(
                    "Invalid playlist {} in '{}': {}", index, filePath, errorMessage);
                return false;
            }
            if (!playlistNames.insert(imported.name).second)
            {
                spdlog::error(
                    "Invalid playlists document '{}': duplicate playlist name '{}'.",
                    filePath, imported.name);
                return false;
            }
            importedPlaylists.push_back(std::move(imported));
        }

        for (const ImportedPlaylist& imported : importedPlaylists)
        {
            std::string errorMessage;
            if (!LoadImportedTracks(imported, filePath, rollback, errorMessage))
            {
                spdlog::error(
                    "Unable to load playlist '{}' from '{}': {}",
                    imported.name, filePath, errorMessage);
                return false;
            }
        }

        std::vector<Playlist> newPlaylists;
        newPlaylists.reserve(importedPlaylists.size());
        for (const ImportedPlaylist& imported : importedPlaylists)
        {
            Playlist playlist;
            playlist.name = imported.name;
            playlist.options = imported.options;
            playlist.crossfadeDuration = imported.crossfadeDuration;
            playlist.tracks.reserve(imported.tracks.size());
            for (const ImportedTrack& track : imported.tracks)
                playlist.tracks.push_back(track.id);
            newPlaylists.push_back(std::move(playlist));
        }

        Stop("");
        m_playlists = std::move(newPlaylists);
        m_activePlaylistName = "";

        rollback.Dismiss();
        auto& audioManager = AudioManager::GetInstance();
        for (const ImportedPlaylist& imported : importedPlaylists)
            for (const ImportedTrack& track : imported.tracks)
                audioManager.QueueLoudnessAnalysis(track.id);

        spdlog::info("Loaded {} playlists from '{}'.", m_playlists.size(), filePath);
        NotifyPlaylistChanged();
        return true;
    }
    catch(const std::exception& e)
    {
        spdlog::error("Error loading playlists: {}", e.what());
        return false;
    }
}

void PlaylistManager::RegisterPlaylistChangeCallback(PlaylistChangeCallback callback)
{
    m_changeCallbacks.push_back(callback);
}

void PlaylistManager::NotifyPlaylistChanged()
{
    for (const auto& callback : m_changeCallbacks)
    {
        callback();
    }
}

bool PlaylistManager::IsOwnedChannel(
    FMOD::Channel* channel, FMOD::Sound* expectedSound)
{
    if (!channel || !expectedSound) return false;

    FMOD::Sound* actualSound = nullptr;
    return channel->getCurrentSound(&actualSound) == FMOD_OK &&
           actualSound == expectedSound;
}

bool PlaylistManager::IsOwnedChannelPlaying(
    FMOD::Channel* channel, FMOD::Sound* expectedSound)
{
    if (!IsOwnedChannel(channel, expectedSound)) return false;

    bool isPlaying = false;
    return channel->isPlaying(&isPlaying) == FMOD_OK && isPlaying;
}

void PlaylistManager::StopOwnedChannelWithFade(
    FMOD::Channel* channel, FMOD::Sound* expectedSound)
{
    if (IsOwnedChannelPlaying(channel, expectedSound))
        AudioManager::GetInstance().StopChannelWithFadeOut(channel);
}

void PlaylistManager::StopOwnedChannelImmediately(
    FMOD::Channel* channel, FMOD::Sound* expectedSound)
{
    if (IsOwnedChannelPlaying(channel, expectedSound)) channel->stop();
}

void PlaylistManager::ClearLogicalPlaybackState(Playlist& plist)
{
    plist.isPlaying = false;
    plist.currentIndex = -1;
    plist.nextIndex = -1;
    plist.randomIndices.clear();
    plist.randomIndexPos = 0;
    plist.currentChannel = nullptr;
    plist.nextChannel = nullptr;
    plist.expectedCurrentSound = nullptr;
    plist.expectedNextSound = nullptr;
    plist.isCrossfading = false;
    plist.crossfadeTimer = 0.0f;
    plist.segmentTimer = 0.0f;
    plist.segmentMaxDuration = 0.0f;
    plist.segmentModeActive = false;
    plist.chosenStartTime = 0.0f;
    plist.secondsUntilTransition = (std::numeric_limits<float>::max)();
    plist.lastTransitionReason = TransitionLogic::Reason::None;
}

PlaybackState PlaylistManager::CapturePlaybackState() const
{
    PlaybackState state;
    const Playlist* playlist = GetActivePlaylist();
    if (!playlist || !playlist->isPlaying) return state;
    // The global library is a manual, non-scheduled source. Cinema recovery
    // only resumes the named playlist still selected by the active calendar,
    // so this source must not be written as an automation checkpoint.
    if (playlist == &m_libraryPlayback) return state;

    FMOD::Channel* channel = nullptr;
    FMOD::Sound* expectedSound = nullptr;
    int trackIndex = -1;
    if (playlist->isCrossfading &&
        IsOwnedChannelPlaying(playlist->nextChannel, playlist->expectedNextSound))
    {
        channel = playlist->nextChannel;
        expectedSound = playlist->expectedNextSound;
        trackIndex = playlist->nextIndex;
    }
    else if (IsOwnedChannelPlaying(
                 playlist->currentChannel, playlist->expectedCurrentSound))
    {
        channel = playlist->currentChannel;
        expectedSound = playlist->expectedCurrentSound;
        trackIndex = playlist->currentIndex;
    }

    if (!channel || !expectedSound || trackIndex < 0 ||
        trackIndex >= static_cast<int>(playlist->tracks.size()))
        return state;

    unsigned int positionMs = 0;
    if (channel->getPosition(&positionMs, FMOD_TIMEUNIT_MS) != FMOD_OK) return state;

    state.isPlaying = true;
    state.playlistName = playlist->name;
    state.trackId = playlist->tracks[trackIndex];
    state.trackIndex = trackIndex;
    state.positionMs = positionMs;
    state.options = playlist->options;
    state.crossfadeDuration = playlist->crossfadeDuration;
    state.segmentActive = playlist->segmentModeActive;

    const auto secondsToMilliseconds = [](float seconds) {
        if (!std::isfinite(seconds) || seconds <= 0.0f) return std::uint32_t{0};
        const double milliseconds = static_cast<double>(seconds) * 1000.0;
        return static_cast<std::uint32_t>(std::llround(std::min(
            milliseconds,
            static_cast<double>((std::numeric_limits<std::uint32_t>::max)()))));
    };
    if (state.segmentActive)
    {
        state.segmentStartMs = secondsToMilliseconds(playlist->chosenStartTime);
        state.segmentElapsedMs = secondsToMilliseconds(playlist->segmentTimer);
    }

    if (state.options.randomOrder)
    {
        state.randomPermutation = playlist->randomIndices;
        const auto current = std::find(
            state.randomPermutation.begin(), state.randomPermutation.end(), trackIndex);
        if (current == state.randomPermutation.end()) return PlaybackState{};
        state.randomPermutationIndex = static_cast<int>(
            std::distance(state.randomPermutation.begin(), current));
    }

    return state;
}

bool PlaylistManager::ResumePlaybackState(
    const PlaybackState& state, std::string& errorMessage)
{
    errorMessage.clear();
    if (!state.isPlaying)
    {
        AbortImmediately();
        return true;
    }

    Playlist* playlist = GetPlaylistByName(state.playlistName);
    if (!playlist)
    {
        errorMessage = "The saved playlist no longer exists.";
        return false;
    }
    if (state.trackIndex < 0 ||
        state.trackIndex >= static_cast<int>(playlist->tracks.size()))
    {
        errorMessage = "The saved track index is outside the playlist.";
        return false;
    }
    if (state.trackId.empty() || playlist->tracks[state.trackIndex] != state.trackId)
    {
        errorMessage = "The saved track id does not match the playlist index.";
        return false;
    }
    if (!std::isfinite(state.options.segmentDuration) ||
        state.options.segmentDuration <= 0.0f ||
        state.options.segmentDuration > 86400.0f)
    {
        errorMessage = "The saved segment duration is outside the supported range.";
        return false;
    }
    if (!std::isfinite(state.crossfadeDuration) ||
        state.crossfadeDuration < 0.0f || state.crossfadeDuration > 3600.0f)
    {
        errorMessage = "The saved crossfade duration is outside the supported range.";
        return false;
    }
    if (state.segmentActive != state.options.randomSegment)
    {
        errorMessage = "The saved segment state conflicts with the playlist options.";
        return false;
    }

    FMOD::Sound* expectedSound = AudioManager::GetInstance().GetSound(state.trackId);
    unsigned int lengthMs = 0;
    if (!expectedSound ||
        expectedSound->getLength(&lengthMs, FMOD_TIMEUNIT_MS) != FMOD_OK ||
        lengthMs == 0)
    {
        errorMessage = "The saved track is unavailable or has no playable duration.";
        return false;
    }

    if (state.segmentActive)
    {
        const double durationMs = static_cast<double>(state.options.segmentDuration) * 1000.0;
        if (state.segmentStartMs >= lengthMs ||
            static_cast<double>(state.segmentElapsedMs) > durationMs)
        {
            errorMessage = "The saved random segment position is invalid.";
            return false;
        }
    }
    else if (state.segmentStartMs != 0 || state.segmentElapsedMs != 0)
    {
        errorMessage = "A non-segmented playback state contains segment progress.";
        return false;
    }

    if (state.options.randomOrder)
    {
        if (state.randomPermutationIndex < 0 ||
            state.randomPermutationIndex >=
                static_cast<int>(state.randomPermutation.size()) ||
            state.randomPermutation[state.randomPermutationIndex] != state.trackIndex)
        {
            errorMessage = "The saved random playlist cursor is invalid.";
            return false;
        }

        std::set<int> eligibleIndices;
        for (int index = 0; index < static_cast<int>(playlist->tracks.size()); ++index)
        {
            if (IsTrackEligibleForPlayback(*playlist, index)) eligibleIndices.insert(index);
        }
        const std::set<int> savedIndices(
            state.randomPermutation.begin(), state.randomPermutation.end());
        if (savedIndices.size() != state.randomPermutation.size() ||
            savedIndices != eligibleIndices)
        {
            errorMessage = "The saved random permutation no longer matches the playlist.";
            return false;
        }
    }
    else if (!state.randomPermutation.empty() || state.randomPermutationIndex != -1)
    {
        errorMessage = "A sequential playback state contains a random permutation.";
        return false;
    }

    unsigned int clampedPositionMs = std::min(state.positionMs, lengthMs - 1u);
    if (state.segmentActive)
    {
        const std::uint64_t segmentEndMs =
            static_cast<std::uint64_t>(state.segmentStartMs) +
            static_cast<std::uint64_t>(state.options.segmentDuration * 1000.0f);
        const unsigned int maximumSegmentPosition = static_cast<unsigned int>(
            std::min<std::uint64_t>(lengthMs - 1u, segmentEndMs));
        clampedPositionMs = std::clamp(
            clampedPositionMs, state.segmentStartMs, maximumSegmentPosition);
    }
    FMOD::Channel* stagedChannel = AudioManager::GetInstance().PlayMusic(
        state.trackId, false, 1.0f);
    if (!stagedChannel)
    {
        errorMessage = "Unable to create a channel for the saved track.";
        return false;
    }

    FMOD::Sound* actualSound = nullptr;
    const FMOD_RESULT soundResult = stagedChannel->getCurrentSound(&actualSound);
    if (soundResult != FMOD_OK || actualSound != expectedSound)
    {
        StopOwnedChannelImmediately(stagedChannel, expectedSound);
        errorMessage = "The resumed channel does not own the expected track.";
        return false;
    }

    FMOD_RESULT result = stagedChannel->setPaused(true);
    if (result == FMOD_OK)
        result = stagedChannel->setPosition(clampedPositionMs, FMOD_TIMEUNIT_MS);
    if (result == FMOD_OK) result = stagedChannel->setPaused(false);
    if (result != FMOD_OK)
    {
        StopOwnedChannelImmediately(stagedChannel, expectedSound);
        errorMessage = std::string("Unable to seek the resumed track: ") +
            FMOD_ErrorString(result);
        return false;
    }

    // Commit only after the replacement channel has been fully validated. A
    // just-created channel can reuse a stale FMOD handle, so preserve it while
    // clearing the previous logical state.
    for (Playlist& existing : m_playlists)
    {
        if (existing.currentChannel != stagedChannel)
            StopOwnedChannelImmediately(
                existing.currentChannel, existing.expectedCurrentSound);
        if (existing.nextChannel != stagedChannel)
            StopOwnedChannelImmediately(existing.nextChannel, existing.expectedNextSound);
        ClearLogicalPlaybackState(existing);
    }
    StopOwnedChannelImmediately(
        m_libraryPlayback.currentChannel, m_libraryPlayback.expectedCurrentSound);
    StopOwnedChannelImmediately(
        m_libraryPlayback.nextChannel, m_libraryPlayback.expectedNextSound);
    ClearLogicalPlaybackState(m_libraryPlayback);

    playlist->options = state.options;
    playlist->crossfadeDuration = state.crossfadeDuration;
    playlist->activeCrossfadeDuration = state.crossfadeDuration;
    playlist->currentIndex = state.trackIndex;
    playlist->currentChannel = stagedChannel;
    playlist->expectedCurrentSound = expectedSound;
    playlist->randomIndices = state.randomPermutation;
    playlist->randomIndexPos = state.options.randomOrder
        ? state.randomPermutationIndex
        : 0;
    playlist->segmentModeActive = state.segmentActive;
    playlist->segmentMaxDuration = state.segmentActive
        ? state.options.segmentDuration
        : 0.0f;
    playlist->chosenStartTime = state.segmentStartMs / 1000.0f;
    playlist->segmentTimer = state.segmentElapsedMs / 1000.0f;
    playlist->isPlaying = true;
    m_activePlaylistName = state.playlistName;
    return true;
}

void PlaylistManager::AbortImmediately()
{
    for (Playlist& playlist : m_playlists)
    {
        StopOwnedChannelImmediately(
            playlist.currentChannel, playlist.expectedCurrentSound);
        StopOwnedChannelImmediately(playlist.nextChannel, playlist.expectedNextSound);
        ClearLogicalPlaybackState(playlist);
    }
    StopOwnedChannelImmediately(
        m_libraryPlayback.currentChannel, m_libraryPlayback.expectedCurrentSound);
    StopOwnedChannelImmediately(
        m_libraryPlayback.nextChannel, m_libraryPlayback.expectedNextSound);
    ClearLogicalPlaybackState(m_libraryPlayback);
    m_activePlaylistName.clear();
    spdlog::warn("Programme playback aborted immediately");
}

void PlaylistManager::Play(const std::string& playlistName, const PlaylistOptions& options)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it == m_playlists.end())
    {
        spdlog::error("Playlist '{}' not found.", playlistName);
        return;
    }

    (void)StartPlayback(*it, options);
}

bool PlaylistManager::PlayLibrary(
    const PlaylistOptions& options, float crossfadeDuration)
{
    std::vector<std::string> musicIds = GetLibraryMusicIds();
    if (musicIds.empty())
    {
        spdlog::warn("The music library has no loaded playable track.");
        return false;
    }

    m_libraryPlayback.tracks = std::move(musicIds);
    m_libraryPlayback.crossfadeDuration = std::max(crossfadeDuration, 0.0f);
    return StartPlayback(m_libraryPlayback, options);
}

bool PlaylistManager::StartPlayback(
    Playlist& plist, const PlaylistOptions& options)
{
    if (plist.tracks.empty())
    {
        spdlog::error("Playback source '{}' is empty.", plist.name);
        return false;
    }

    if (GetActivePlaylist()) Stop(m_activePlaylistName);
    
    plist.options = options;
    plist.segmentModeActive = options.randomSegment;
    plist.segmentMaxDuration = std::max(options.segmentDuration, 0.0f);
    plist.segmentTimer = 0.0f;
    plist.chosenStartTime = 0.0f;

    if (options.randomOrder)
    {
        PrepareRandomOrder(plist);
        if (plist.randomIndices.empty())
        {
            spdlog::warn("Playback source '{}' has no loaded playable track.", plist.name);
            FinishPlaylist(plist);
            return false;
        }
        plist.randomIndexPos = 0;
        plist.currentIndex = plist.randomIndices[0];
    }
    else
    {
        plist.currentIndex = FindNextEligibleIndex(plist, -1, false);
        if (plist.currentIndex < 0)
        {
            spdlog::warn("Playback source '{}' has no loaded playable track.", plist.name);
            FinishPlaylist(plist);
            return false;
        }
    }

    m_activePlaylistName = &plist == &m_libraryPlayback ? std::string{} : plist.name;
    plist.isPlaying = true;
    plist.currentChannel = nullptr;
    plist.nextChannel = nullptr;
    plist.expectedCurrentSound = nullptr;
    plist.expectedNextSound = nullptr;
    plist.nextIndex = -1;
    plist.isCrossfading = false;
    plist.crossfadeTimer = 0.0f;

    StartTrackAtIndex(plist, plist.currentIndex);

    if (plist.isPlaying)
        spdlog::info("Playback source '{}' started.", plist.name);
    return plist.isPlaying;
}

void PlaylistManager::Stop(const std::string& playlistName)
{
    auto stopPlaylist = [](Playlist& plist)
    {
        StopOwnedChannelWithFade(plist.currentChannel, plist.expectedCurrentSound);
        StopOwnedChannelWithFade(plist.nextChannel, plist.expectedNextSound);
        ClearLogicalPlaybackState(plist);
    };

    if (playlistName.empty())
    {
        for (auto& plist : m_playlists)
        {
            if (plist.isPlaying)
            {
                stopPlaylist(plist);
            }
        }
        if (m_libraryPlayback.isPlaying) stopPlaylist(m_libraryPlayback);

        m_activePlaylistName.clear();
        spdlog::info("All programme playback stopped with fade-out");
        return;
    }

    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& playlist) { return playlist.name == playlistName; });
    if (it == m_playlists.end())
    {
        spdlog::warn("Playlist '{}' not found while stopping.", playlistName);
        return;
    }

    if (it->isPlaying)
    {
        stopPlaylist(*it);
    }
    if (m_activePlaylistName == playlistName)
    {
        m_activePlaylistName.clear();
    }
    spdlog::info("Playlist '{}' stopped with fade-out", playlistName);
}

void PlaylistManager::StopLibrary()
{
    if (!m_libraryPlayback.isPlaying) return;
    StopOwnedChannelWithFade(
        m_libraryPlayback.currentChannel, m_libraryPlayback.expectedCurrentSound);
    StopOwnedChannelWithFade(
        m_libraryPlayback.nextChannel, m_libraryPlayback.expectedNextSound);
    ClearLogicalPlaybackState(m_libraryPlayback);
    m_activePlaylistName.clear();
    spdlog::info("Music library playback stopped with fade-out");
}

void PlaylistManager::Update(float deltaTime)
{
    for (auto& plist : m_playlists)
        UpdatePlayback(plist, deltaTime);
    UpdatePlayback(m_libraryPlayback, deltaTime);
}

void PlaylistManager::UpdatePlayback(Playlist& plist, float deltaTime)
{
        if (!plist.isPlaying) return;

        constexpr float baseMusicVol = 1.0f;

        if (plist.isCrossfading)
        {
            plist.crossfadeTimer += deltaTime;
            float t = plist.activeCrossfadeDuration <= 0.0f
                ? 1.0f
                : plist.crossfadeTimer / plist.activeCrossfadeDuration;
            if (t > 1.0f) t = 1.0f;

            const TransitionLogic::EqualPowerGains gains =
                TransitionLogic::CalculateEqualPowerGains(t);

            bool currentChannelValid = false;
            bool nextChannelValid    = false;

            if (IsOwnedChannelPlaying(
                    plist.currentChannel, plist.expectedCurrentSound))
            {
                currentChannelValid = true;
                const float normalizationGain = AudioManager::GetInstance()
                    .GetNormalizationGainForChannel(plist.currentChannel);
                float volOld = gains.outgoing * baseMusicVol * normalizationGain;
                plist.currentChannel->setVolume(volOld);
            }

            if (IsOwnedChannelPlaying(plist.nextChannel, plist.expectedNextSound))
            {
                nextChannelValid = true;
                const float normalizationGain = AudioManager::GetInstance()
                    .GetNormalizationGainForChannel(plist.nextChannel);
                float volNext = gains.incoming * baseMusicVol * normalizationGain;
                plist.nextChannel->setVolume(volNext);
            }

            if (t >= 1.0f || (!currentChannelValid && !nextChannelValid))
            {
                FinishCrossfade(plist);
            }
        }
        else if (plist.currentChannel)
        {
            if (!IsOwnedChannel(plist.currentChannel, plist.expectedCurrentSound))
            {
                spdlog::error("Playlist channel ownership changed, restarting track");
                plist.currentChannel = nullptr;
                plist.expectedCurrentSound = nullptr;
                StartTrackAtIndex(plist, plist.currentIndex);
                return;
            }

            bool isPlaying = false;
            FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
            
            if (result != FMOD_OK)
            {
                spdlog::error("Channel state check failed, restarting track");
                StartTrackAtIndex(plist, plist.currentIndex);
                return;
            }

            float trackRemaining = std::numeric_limits<float>::max();

            FMOD::Sound* sound = plist.expectedCurrentSound;
            if (sound)
            {
                unsigned int lengthMs = 0;
                unsigned int positionMs = 0;
                if (sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS) == FMOD_OK &&
                    plist.currentChannel->getPosition(&positionMs, FMOD_TIMEUNIT_MS) == FMOD_OK &&
                    lengthMs > 0)
                {
                    const unsigned int remainingMs = positionMs < lengthMs
                        ? lengthMs - positionMs
                        : 0;
                    trackRemaining = remainingMs / 1000.0f;
                }
            }

            float segmentRemaining = std::numeric_limits<float>::max();
            if (plist.segmentModeActive)
            {
                plist.segmentTimer += deltaTime;
                segmentRemaining = std::max(
                    plist.segmentMaxDuration - plist.segmentTimer, 0.0f);
            }

            const TransitionLogic::Decision decision = TransitionLogic::Evaluate(
                trackRemaining, segmentRemaining, plist.segmentModeActive,
                plist.crossfadeDuration, HasNextTrack(plist), isPlaying);
            plist.secondsUntilTransition = decision.secondsUntilBoundary;
            if (decision.shouldTransition)
            {
                StartNextTrack(plist, decision.crossfadeDuration, decision.reason);
                return;
            }
        }
        else
        {
            spdlog::warn("Invalid channel state detected, attempting to recover");
            StartTrackAtIndex(plist, plist.currentIndex);
        }
}

std::string PlaylistManager::GetCurrentTrackName() const
{
    auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || !activePlaylist->isPlaying) return "";
    if (activePlaylist->currentIndex < 0 || 
        activePlaylist->currentIndex >= (int)activePlaylist->tracks.size()) 
        return "";
    return activePlaylist->tracks[activePlaylist->currentIndex];
}

float PlaylistManager::GetTrackProgress() const
{
    auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || !IsOwnedChannelPlaying(
            activePlaylist->currentChannel, activePlaylist->expectedCurrentSound))
        return 0.0f;

    unsigned int positionMs = 0;
    unsigned int lengthMs   = 0;
    FMOD::Sound* sound      = nullptr;

    FMOD_RESULT result = activePlaylist->currentChannel->getPosition(&positionMs, FMOD_TIMEUNIT_MS);
    if (result != FMOD_OK) return 0.0f;

    sound = activePlaylist->expectedCurrentSound;

    result = sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
    if (result != FMOD_OK || lengthMs == 0) return 0.0f;

    return static_cast<float>(positionMs) / static_cast<float>(lengthMs);
}

float PlaylistManager::GetSegmentProgress() const
{
    auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || !activePlaylist->segmentModeActive) return 0.0f;
    if (activePlaylist->segmentMaxDuration <= 0.0f) return 0.0f;
    return std::clamp(activePlaylist->segmentTimer / activePlaylist->segmentMaxDuration, 0.0f, 1.0f);
}

float PlaylistManager::GetSegmentDuration() const
{
    const auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || !activePlaylist->segmentModeActive) return 0.0f;
    return std::max(activePlaylist->segmentMaxDuration, 0.0f);
}

float PlaylistManager::GetSegmentRemainingTime() const
{
    const auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || !activePlaylist->segmentModeActive) return 0.0f;
    return std::max(activePlaylist->segmentMaxDuration - activePlaylist->segmentTimer, 0.0f);
}

void PlaylistManager::SetCrossfadeDuration(float duration)
{
    if (Playlist* activePlaylist = GetActivePlaylist()) {
        activePlaylist->crossfadeDuration = std::max(duration, 0.0f);
    }
}

FMOD::Channel* PlaylistManager::GetCurrentChannel() const
{
    auto* activePlaylist = GetActivePlaylist();
    return activePlaylist && IsOwnedChannelPlaying(
        activePlaylist->currentChannel, activePlaylist->expectedCurrentSound)
        ? activePlaylist->currentChannel
        : nullptr;
}

bool PlaylistManager::IsInCrossfade() const
{
    auto* activePlaylist = GetActivePlaylist();
    return activePlaylist ? activePlaylist->isCrossfading : false;
}

float PlaylistManager::GetCrossfadeProgress() const
{
    auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || !activePlaylist->isCrossfading) return 0.0f;
    if (activePlaylist->activeCrossfadeDuration <= 0.0f) return 1.0f;
    return std::clamp(
        activePlaylist->crossfadeTimer / activePlaylist->activeCrossfadeDuration,
        0.0f, 1.0f);
}

FMOD::Channel* PlaylistManager::GetNextChannel() const
{
    auto* activePlaylist = GetActivePlaylist();
    return activePlaylist && IsOwnedChannelPlaying(
        activePlaylist->nextChannel, activePlaylist->expectedNextSound)
        ? activePlaylist->nextChannel
        : nullptr;
}

std::string PlaylistManager::GetNextTrackName() const
{
    const Playlist* playlist = GetActivePlaylist();
    if (!playlist || !playlist->isPlaying) return "";
    if (playlist->isCrossfading &&
        playlist->nextIndex >= 0 &&
        playlist->nextIndex < static_cast<int>(playlist->tracks.size()))
    {
        return playlist->tracks[playlist->nextIndex];
    }

    int nextIndex = -1;
    if (playlist->options.randomOrder)
    {
        int nextPosition = playlist->randomIndexPos + 1;
        if (nextPosition >= static_cast<int>(playlist->randomIndices.size()))
        {
            if (!playlist->options.loopPlaylist || playlist->randomIndices.empty()) return "";
            nextPosition = 0;
        }
        nextIndex = playlist->randomIndices[nextPosition];
    }
    else
    {
        nextIndex = FindNextEligibleIndex(
            *playlist, playlist->currentIndex, playlist->options.loopPlaylist);
    }

    return nextIndex >= 0 && nextIndex < static_cast<int>(playlist->tracks.size())
        ? playlist->tracks[nextIndex]
        : "";
}

float PlaylistManager::GetSecondsUntilTransition() const
{
    const Playlist* playlist = GetActivePlaylist();
    if (!playlist || !playlist->isPlaying ||
        !std::isfinite(playlist->secondsUntilTransition)) return -1.0f;
    return std::max(playlist->secondsUntilTransition, 0.0f);
}

const char* PlaylistManager::GetLastTransitionReason() const
{
    const Playlist* playlist = GetActivePlaylist();
    return playlist
        ? TransitionLogic::ToString(playlist->lastTransitionReason)
        : TransitionLogic::ToString(TransitionLogic::Reason::None);
}

const PlaylistManager::Playlist* PlaylistManager::GetActivePlaylist() const
{
    if (m_libraryPlayback.isPlaying) return &m_libraryPlayback;
    if (m_activePlaylistName.empty()) return nullptr;
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [this](const Playlist& p) { return p.name == m_activePlaylistName; });
    return (it != m_playlists.end()) ? &(*it) : nullptr;
}

PlaylistManager::Playlist* PlaylistManager::GetActivePlaylist()
{
    if (m_libraryPlayback.isPlaying) return &m_libraryPlayback;
    if (m_activePlaylistName.empty()) return nullptr;
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [this](Playlist& p) { return p.name == m_activePlaylistName; });
    return (it != m_playlists.end()) ? &(*it) : nullptr;
}

void PlaylistManager::StartNextTrack(Playlist& plist, float transitionDuration,
                                     TransitionLogic::Reason reason)
{
    int nextIndex = -1;
    if (plist.options.randomOrder)
    {
        if (plist.randomIndices.empty())
        {
            FinishPlaylist(plist);
            return;
        }

        plist.randomIndexPos++;
        if (plist.randomIndexPos >= (int)plist.randomIndices.size())
        {
            if (plist.options.loopPlaylist) {
                plist.randomIndexPos = 0;
            } else {
                FinishPlaylist(plist);
                return;
            }
        }
        nextIndex = plist.randomIndices[plist.randomIndexPos];
    }
    else
    {
        nextIndex = FindNextEligibleIndex(plist, plist.currentIndex, plist.options.loopPlaylist);
        if (nextIndex < 0)
        {
            FinishPlaylist(plist);
            return;
        }
    }

    plist.isCrossfading   = true;
    plist.lastTransitionReason = reason;
    plist.crossfadeTimer  = 0.0f;
    plist.activeCrossfadeDuration = transitionDuration >= 0.0f
        ? std::min(transitionDuration, std::max(plist.crossfadeDuration, 0.0f))
        : std::max(plist.crossfadeDuration, 0.0f);

    plist.oldChannelVolume = 1.0f;
    if (IsOwnedChannelPlaying(plist.currentChannel, plist.expectedCurrentSound))
    {
        float vol = 1.0f;
        plist.currentChannel->getVolume(&vol);
        plist.oldChannelVolume = vol;
    }

    constexpr float userVolume = 1.0f;
    plist.nextTargetVolume = userVolume;

    std::string nextTrack = plist.tracks[nextIndex];
    FMOD::Sound* expectedSound = AudioManager::GetInstance().GetSound(nextTrack);
    FMOD::Channel* ch = AudioManager::GetInstance().PlayMusic(nextTrack, false, 0.0f);
    if (!ch || !IsOwnedChannelPlaying(ch, expectedSound))
    {
        StopOwnedChannelImmediately(ch, expectedSound);
        spdlog::error("Failed to start next track '{}' in playlist '{}'.", nextTrack, plist.name);
        FinishPlaylist(plist);
        return;
    }
    plist.nextChannel = ch;
    plist.expectedNextSound = expectedSound;

    plist.nextIndex = nextIndex;

    plist.segmentTimer = 0.0f;
    if (plist.segmentModeActive && ch)
    {
        FMOD::Sound* sound = expectedSound;
        if (sound)
        {
            unsigned int lengthMs = 0;
            sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
            float lengthSec = lengthMs / 1000.0f;

            float maxStart = (lengthSec > plist.segmentMaxDuration)
                             ? (lengthSec - plist.segmentMaxDuration)
                             : 0.0f;
            std::uniform_real_distribution<float> dist(0.0f, maxStart);
            plist.chosenStartTime = dist(m_rng);

            ch->setPosition((unsigned int)(plist.chosenStartTime * 1000.0f), FMOD_TIMEUNIT_MS);
        }
    }
}

void PlaylistManager::StartTrackAtIndex(Playlist& plist, int index)
{
    if (index < 0 || index >= (int)plist.tracks.size()) return;

    std::string track = plist.tracks[index];

    constexpr float userVolume = 1.0f;

    // A track must finish naturally so StartNextTrack can apply the playlist's
    // loop policy. Looping the FMOD sound bypasses playlist progression.
    FMOD::Sound* expectedSound = AudioManager::GetInstance().GetSound(track);
    FMOD::Channel* ch = AudioManager::GetInstance().PlayMusicWithFadeIn(track, false, userVolume);
    if (!ch || !IsOwnedChannelPlaying(ch, expectedSound)) {
        StopOwnedChannelImmediately(ch, expectedSound);
        spdlog::error("Failed to start track at index {}", index);
        FinishPlaylist(plist);
        return;
    }

    if (plist.currentChannel != ch)
        StopOwnedChannelWithFade(plist.currentChannel, plist.expectedCurrentSound);

    plist.currentChannel = ch;
    plist.expectedCurrentSound = expectedSound;

    bool isPlaying = false;
    FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
    if (result != FMOD_OK || !isPlaying) {
        spdlog::error("Channel validation failed after start");
        StopOwnedChannelImmediately(plist.currentChannel, plist.expectedCurrentSound);
        FinishPlaylist(plist);
        return;
    }

    plist.segmentTimer = 0.0f;
    
    if (plist.options.randomSegment && ch)
    {
        plist.segmentModeActive = true;
        
        FMOD::Sound* sound = expectedSound;
        if (sound)
        {
            unsigned int lengthMs = 0;
            sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
            float lengthSec = lengthMs / 1000.0f;

            float maxStart = (lengthSec > plist.options.segmentDuration)
                             ? (lengthSec - plist.options.segmentDuration)
                             : 0.0f;
            
            std::uniform_real_distribution<float> dist(0.0f, maxStart);
            plist.chosenStartTime = dist(m_rng);
            
            ch->setPosition((unsigned int)(plist.chosenStartTime * 1000.0f), FMOD_TIMEUNIT_MS);
            
            plist.segmentMaxDuration = plist.options.segmentDuration;
        }
    }
    else
    {
        plist.segmentModeActive = false;
    }
}

void PlaylistManager::FinishCrossfade(Playlist& plist)
{
    StopOwnedChannelImmediately(plist.currentChannel, plist.expectedCurrentSound);
    plist.currentChannel = nullptr;
    plist.expectedCurrentSound = nullptr;

    plist.currentChannel = plist.nextChannel;
    plist.expectedCurrentSound = plist.expectedNextSound;
    plist.nextChannel    = nullptr;
    plist.expectedNextSound = nullptr;
    if (plist.nextIndex >= 0) plist.currentIndex = plist.nextIndex;
    plist.nextIndex = -1;
    plist.isCrossfading  = false;
    plist.crossfadeTimer = 0.0f;

    if (plist.currentChannel)
    {
        if (!IsOwnedChannelPlaying(
                plist.currentChannel, plist.expectedCurrentSound))
        {
            spdlog::warn("New current channel ended during crossfade, advancing playlist");
            plist.currentChannel = nullptr;
            plist.expectedCurrentSound = nullptr;
            StartNextTrack(plist, 0.0f, TransitionLogic::Reason::PlaybackFailure);
        }
    }
}

void PlaylistManager::PrepareRandomOrder(Playlist& plist)
{
    plist.randomIndices.clear();
    for (int i = 0; i < static_cast<int>(plist.tracks.size()); ++i)
    {
        if (IsTrackEligibleForPlayback(plist, i))
        {
            plist.randomIndices.push_back(i);
        }
    }

    std::shuffle(plist.randomIndices.begin(), plist.randomIndices.end(), m_rng);
}

bool PlaylistManager::IsTrackEligibleForPlayback(const Playlist& plist, int index, float* lengthSeconds) const
{
    if (index < 0 || index >= static_cast<int>(plist.tracks.size())) return false;

    FMOD::Sound* sound = AudioManager::GetInstance().GetSound(plist.tracks[index]);
    if (!sound) return false;

    unsigned int lengthMs = 0;
    if (sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS) != FMOD_OK) return false;

    const float duration = lengthMs / 1000.0f;
    if (lengthSeconds) *lengthSeconds = duration;

    return duration > 0.0f;
}

int PlaylistManager::FindNextEligibleIndex(const Playlist& plist, int currentIndex, bool allowWrap) const
{
    const int trackCount = static_cast<int>(plist.tracks.size());
    if (trackCount == 0) return -1;

    for (int index = currentIndex + 1; index < trackCount; ++index)
    {
        if (IsTrackEligibleForPlayback(plist, index)) return index;
    }

    if (allowWrap)
    {
        const int lastWrappedIndex = std::min(currentIndex, trackCount - 1);
        for (int index = 0; index <= lastWrappedIndex; ++index)
        {
            if (IsTrackEligibleForPlayback(plist, index)) return index;
        }
    }

    return -1;
}

bool PlaylistManager::HasNextTrack(const Playlist& plist) const
{
    if (plist.options.randomOrder)
    {
        if (plist.randomIndices.empty()) return false;
        return plist.randomIndexPos + 1 < static_cast<int>(plist.randomIndices.size()) ||
               plist.options.loopPlaylist;
    }

    return FindNextEligibleIndex(plist, plist.currentIndex, plist.options.loopPlaylist) >= 0;
}

void PlaylistManager::FinishPlaylist(Playlist& plist)
{
    StopOwnedChannelWithFade(plist.currentChannel, plist.expectedCurrentSound);
    StopOwnedChannelWithFade(plist.nextChannel, plist.expectedNextSound);
    ClearLogicalPlaybackState(plist);

    if (m_activePlaylistName == plist.name)
    {
        m_activePlaylistName.clear();
    }
}

const PlaylistManager::Playlist* PlaylistManager::GetPlaylistByName(const std::string& name) const
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&name](const Playlist& p){ return p.name == name; });
    if (it != m_playlists.end()) {
        return &(*it);
    }
    return nullptr;
}

void PlaylistManager::MoveTrackUp(const std::string& playlistName, int index)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p){ return p.name == playlistName; });
    if (it == m_playlists.end()) return;
    auto& plist = *it;
    if (index <= 0 || index >= (int)plist.tracks.size()) return;

    if (plist.isPlaying) Stop(playlistName);

    std::swap(plist.tracks[index], plist.tracks[index-1]);
    NotifyPlaylistChanged();
}

void PlaylistManager::MoveTrackDown(const std::string& playlistName, int index)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p){ return p.name == playlistName; });
    if (it == m_playlists.end()) return;
    auto& plist = *it;
    if (index < 0 || index >= (int)plist.tracks.size() - 1) return;

    if (plist.isPlaying) Stop(playlistName);

    std::swap(plist.tracks[index], plist.tracks[index+1]);
    NotifyPlaylistChanged();
}

void PlaylistManager::PlayFromIndex(const std::string& playlistName, int index)
{
    if (GetActivePlaylist()) Stop(m_activePlaylistName);

    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p){ return p.name == playlistName; });
    if (it == m_playlists.end()) return;

    Playlist& plist = *it;
    if (index < 0 || index >= (int)plist.tracks.size()) return;

    if (plist.isPlaying)
    {
        Stop(playlistName);
    }

    plist.segmentModeActive = plist.options.randomSegment;
    plist.segmentMaxDuration = std::max(plist.options.segmentDuration, 0.0f);
    plist.segmentTimer = 0.0f;

    int selectedIndex = index;
    if (!IsTrackEligibleForPlayback(plist, selectedIndex))
    {
        selectedIndex = FindNextEligibleIndex(plist, selectedIndex, plist.options.loopPlaylist);
    }
    if (selectedIndex < 0)
    {
        spdlog::warn("Playlist '{}' has no playable track from index {}.", playlistName, index);
        FinishPlaylist(plist);
        return;
    }

    m_activePlaylistName = playlistName;
    plist.isPlaying = true;
    plist.currentIndex = selectedIndex;
    plist.nextIndex = -1;
    plist.currentChannel = nullptr;
    plist.nextChannel = nullptr;
    plist.expectedCurrentSound = nullptr;
    plist.expectedNextSound = nullptr;
    plist.isCrossfading = false;
    plist.crossfadeTimer = 0.0f;

    if (plist.options.randomOrder)
    {
        PrepareRandomOrder(plist);
        auto selected = std::find(plist.randomIndices.begin(), plist.randomIndices.end(), selectedIndex);
        if (selected != plist.randomIndices.end())
        {
            std::iter_swap(plist.randomIndices.begin(), selected);
        }
        plist.randomIndexPos = 0;
    }

    StartTrackAtIndex(plist, selectedIndex);
}

std::string PlaylistManager::GetTrackName(int index) const
{
    auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || index < 0 || index >= (int)activePlaylist->tracks.size()) return "";
    return activePlaylist->tracks[index];
}

std::string PlaylistManager::GetTrackDuration(int index) const
{
    auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || index < 0 || index >= (int)activePlaylist->tracks.size()) return "";
    return activePlaylist->tracks[index];
}

void PlaylistManager::SkipToNextTrack(const std::string& playlistName)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it == m_playlists.end() || !it->isPlaying)
    {
        spdlog::error("Playlist '{}' not found or not playing.", playlistName);
        return;
    }

    Playlist& plist = *it;
    
    if (plist.isCrossfading)
    {
        FinishCrossfade(plist);
    }
    
    StartNextTrack(plist, -1.0f, TransitionLogic::Reason::ManualSkip);
    
    spdlog::info("Passage to next track in playlist '{}'.", playlistName);
}

void PlaylistManager::SkipLibrary()
{
    if (!m_libraryPlayback.isPlaying)
    {
        spdlog::error("Music library playback is not active.");
        return;
    }

    if (m_libraryPlayback.isCrossfading) FinishCrossfade(m_libraryPlayback);
    StartNextTrack(
        m_libraryPlayback, -1.0f, TransitionLogic::Reason::ManualSkip);
    spdlog::info("Advanced to the next track in the music library.");
}

} // namespace TSM
