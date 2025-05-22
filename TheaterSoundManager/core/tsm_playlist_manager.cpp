// tsm_playlist_manager.cpp

#include "tsm_playlist_manager.h"
#include "tsm_audio_manager.h"
#include "tsm_fmod_wrapper.h"
#include "tsm_ui_manager.h"
#include <fstream>
#include <spdlog/spdlog.h>
#include <json/json.hpp>
#include <algorithm>
#include <random>
#include <ctime>
#include <cmath>

namespace TSM
{
    using json = nlohmann::json;

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
        newPlaylist.options.segmentDuration = 900.0f;
        
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
    newPlaylist.isCrossfading = false;
    
    m_playlists.push_back(newPlaylist);
    
    spdlog::info("Playlist '{}' duplicated to '{}'.", sourceName, destName);
    NotifyPlaylistChanged();
}

void PlaylistManager::AddToPlaylist(const std::string& playlistName, const std::string& soundName)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it != m_playlists.end())
    {
        it->tracks.push_back(soundName);
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
            std::string trackName = it->tracks[index];
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
        
        std::ofstream file(filePath.c_str());
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
    try
    {
        std::ifstream file(filePath.c_str());
        if (!file.is_open())
        {
            spdlog::error("Failed to open file '{}' for reading.", filePath);
            return false;
        }
        
        json j;
        file >> j;
        file.close();
        
        std::string importedName = playlistName.empty() ? j["name"].get<std::string>() : playlistName;
        
        auto existingIt = std::find_if(m_playlists.begin(), m_playlists.end(),
            [&importedName](const Playlist& p) { return p.name == importedName; });
            
        if (existingIt != m_playlists.end())
        {
            spdlog::warn("Playlist '{}' already exists. It will be overwritten.", importedName);
            existingIt->tracks.clear();
        }
        else
        {
            CreatePlaylist(importedName);
            existingIt = std::find_if(m_playlists.begin(), m_playlists.end(),
                [&importedName](const Playlist& p) { return p.name == importedName; });
        }
        
        if (existingIt != m_playlists.end())
        {
            // Options par défaut
            existingIt->options.randomOrder = true;
            existingIt->options.randomSegment = true;
            existingIt->options.loopPlaylist = true;
            existingIt->options.segmentDuration = 900.0f;
            
            // Remplacer par les options du fichier si elles existent
            if (j.contains("options"))
            {
                const auto& options = j["options"];
                if (options.contains("randomOrder"))
                    existingIt->options.randomOrder = options["randomOrder"].get<bool>();
                if (options.contains("randomSegment"))
                    existingIt->options.randomSegment = options["randomSegment"].get<bool>();
                if (options.contains("segmentDuration"))
                    existingIt->options.segmentDuration = options["segmentDuration"].get<float>();
                if (options.contains("loopPlaylist"))
                    existingIt->options.loopPlaylist = options["loopPlaylist"].get<bool>();
            }
            
            auto& audioManager = AudioManager::GetInstance();
            
            for (const auto& trackJson : j["tracks"])
            {
                std::string trackId = trackJson["id"].get<std::string>();
                
                if (!audioManager.GetSound(trackId) && trackJson.contains("path"))
                {
                    std::string trackPath = trackJson["path"].get<std::string>();
                    audioManager.LoadSound(trackId, trackPath, true);
                }
                
                if (audioManager.GetSound(trackId))
                {
                    existingIt->tracks.push_back(trackId);
                }
                else
                {
                    spdlog::warn("Track '{}' could not be loaded during import. Skipping.", trackId);
                }
            }
            
            spdlog::info("Playlist '{}' imported from '{}' with {} tracks.", 
                        importedName, filePath, existingIt->tracks.size());
            NotifyPlaylistChanged();
            return true;
        }
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
    
    std::string trackToMove = tracks[sourceIndex];
    
    tracks.erase(tracks.begin() + sourceIndex);
    
    if (targetIndex > sourceIndex)
    {
        targetIndex--;
    }
    
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
        
        std::ofstream file(filePath.c_str());
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
    try
    {
        std::ifstream file(filePath.c_str());
        if (!file.is_open())
        {
            spdlog::error("Failed to open file '{}' for reading.", filePath);
            return false;
        }
        
        json j;
        file >> j;
        file.close();
        
        std::vector<Playlist> newPlaylists;
        
        auto& audioManager = AudioManager::GetInstance();
        
        for (const auto& playlistJson : j)
        {
            Playlist playlist;
            playlist.name = playlistJson["name"].get<std::string>();
            
            // Options par défaut
            playlist.options.randomOrder = true;
            playlist.options.randomSegment = true;
            playlist.options.loopPlaylist = true;
            playlist.options.segmentDuration = 900.0f;
            
            // Remplacer par les options du fichier si elles existent
            if (playlistJson.contains("options"))
            {
                const auto& options = playlistJson["options"];
                if (options.contains("randomOrder"))
                    playlist.options.randomOrder = options["randomOrder"].get<bool>();
                if (options.contains("randomSegment"))
                    playlist.options.randomSegment = options["randomSegment"].get<bool>();
                if (options.contains("segmentDuration"))
                    playlist.options.segmentDuration = options["segmentDuration"].get<float>();
                if (options.contains("loopPlaylist"))
                    playlist.options.loopPlaylist = options["loopPlaylist"].get<bool>();
            }
            
            for (const auto& trackJson : playlistJson["tracks"])
            {
                std::string trackId = trackJson["id"].get<std::string>();
                
                if (!audioManager.GetSound(trackId) && trackJson.contains("path"))
                {
                    std::string trackPath = trackJson["path"].get<std::string>();
                    audioManager.LoadSound(trackId, trackPath, true);
                }
                
                if (audioManager.GetSound(trackId))
                {
                    playlist.tracks.push_back(trackId);
                }
                else
                {
                    spdlog::warn("Track '{}' in playlist '{}' could not be loaded. Skipping.", 
                                trackId, playlist.name);
                }
            }
            
            newPlaylists.push_back(playlist);
        }
        
        m_playlists = newPlaylists;
        m_activePlaylistName = "";
        
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

void PlaylistManager::Play(const std::string& playlistName, const PlaylistOptions& options)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it == m_playlists.end())
    {
        spdlog::error("Playlist '{}' not found.", playlistName);
        return;
    }

    Playlist& plist = *it;
    if (plist.tracks.empty())
    {
        spdlog::error("Playlist '{}' is empty.", playlistName);
        return;
    }

    Stop(m_activePlaylistName);
    
    m_activePlaylistName = playlistName;
    plist.options = options;
    plist.isPlaying = true;

    if (options.randomOrder)
    {
        PrepareRandomOrder(plist);
        plist.randomIndexPos = 0;
        plist.currentIndex = plist.randomIndices[0];
    }
    else
    {
        plist.currentIndex = 0;
    }

    plist.currentChannel = nullptr;
    plist.nextChannel = nullptr;
    plist.isCrossfading = false;
    plist.crossfadeTimer = 0.0f;

    plist.segmentModeActive = options.randomSegment;
    plist.segmentMaxDuration = options.segmentDuration;
    plist.segmentTimer = 0.0f;
    plist.chosenStartTime = 0.0f;

    StartTrackAtIndex(plist, plist.currentIndex);

    spdlog::info("Playlist '{}' started.", playlistName);
}

void PlaylistManager::Stop(const std::string& playlistName)
{
    if (playlistName.empty())
    {
        for (auto& plist : m_playlists)
        {
            if (plist.isPlaying)
            {
                if (plist.currentChannel)
                {
                    bool isPlaying = false;
                    plist.currentChannel->isPlaying(&isPlaying);
                    if (isPlaying)
                    {
                        std::string currentTrack = plist.tracks[plist.currentIndex];
                        AudioManager::GetInstance().StopSoundWithFadeOut(currentTrack);
                    }
                }
                
                if (plist.nextChannel)
                {
                    bool isPlaying = false;
                    plist.nextChannel->isPlaying(&isPlaying);
                    if (isPlaying)
                    {
                        int nextIndex = (plist.currentIndex + 1) % plist.tracks.size();
                        std::string nextTrack = plist.tracks[nextIndex];
                        AudioManager::GetInstance().StopSoundWithFadeOut(nextTrack);
                    }
                }
                
                plist.isPlaying = false;
                plist.isCrossfading = false;
                plist.currentChannel = nullptr;
                plist.nextChannel = nullptr;
            }
        }
        
        spdlog::info("All playlists stopped with fade-out");
    }
    else
    {
        for (auto& plist : m_playlists)
        {
            if (plist.name == playlistName && plist.isPlaying)
            {
                if (plist.currentChannel)
                {
                    bool isPlaying = false;
                    plist.currentChannel->isPlaying(&isPlaying);
                    if (isPlaying)
                    {
                        std::string currentTrack = plist.tracks[plist.currentIndex];
                        AudioManager::GetInstance().StopSoundWithFadeOut(currentTrack);
                    }
                }
                
                if (plist.nextChannel)
                {
                    bool isPlaying = false;
                    plist.nextChannel->isPlaying(&isPlaying);
                    if (isPlaying)
                    {
                        int nextIndex = (plist.currentIndex + 1) % plist.tracks.size();
                        std::string nextTrack = plist.tracks[nextIndex];
                        AudioManager::GetInstance().StopSoundWithFadeOut(nextTrack);
                    }
                }
                
                plist.isPlaying = false;
                plist.isCrossfading = false;
                plist.currentChannel = nullptr;
                plist.nextChannel = nullptr;
                
                spdlog::info("Playlist '{}' stopped with fade-out", playlistName);
                break;
            }
        }
    }
}

void PlaylistManager::Update(float deltaTime)
{
    for (auto& plist : m_playlists)
    {
        if (!plist.isPlaying) continue;

        float baseMusicVol = UIManager::GetInstance().GetMasterVolume() 
                           * UIManager::GetInstance().GetMusicVolume()
                           * UIManager::GetInstance().GetDuckFactor();

        if (plist.isCrossfading)
        {
            plist.crossfadeTimer += deltaTime;
            float t = plist.crossfadeTimer / plist.crossfadeDuration;
            if (t > 1.0f) t = 1.0f;

            bool currentChannelValid = false;
            bool nextChannelValid    = false;

            if (plist.currentChannel)
            {
                bool isPlaying = false;
                FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
                currentChannelValid = (result == FMOD_OK && isPlaying);
                
                if (currentChannelValid) {
                    float volOld = (1.0f - t) * baseMusicVol;
                    plist.currentChannel->setVolume(volOld);
                }
            }

            if (plist.nextChannel)
            {
                bool isPlaying = false;
                FMOD_RESULT result = plist.nextChannel->isPlaying(&isPlaying);
                nextChannelValid = (result == FMOD_OK && isPlaying);
                
                if (nextChannelValid) {
                    
                    float volNext = t * baseMusicVol;
                    plist.nextChannel->setVolume(volNext);
                }
            }

            if (t >= 1.0f || (!currentChannelValid && !nextChannelValid))
            {
                FinishCrossfade(plist);
            }
        }
        else if (plist.currentChannel)
        {
            bool isPlaying = false;
            FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
            
            if (result != FMOD_OK)
            {
                spdlog::error("Channel state check failed, restarting track");
                StartTrackAtIndex(plist, plist.currentIndex);
                continue;
            }

            if (!isPlaying && !plist.segmentModeActive)
            {
                if (plist.options.loopPlaylist && plist.tracks.size() == 1)
                {
                    StartTrackAtIndex(plist, plist.currentIndex);
                }
                else if (plist.options.loopPlaylist || plist.tracks.size() > 1)
                {
                    StartNextTrack(plist);
                }
                else
                {
                    plist.isPlaying = false;
                    spdlog::info("Playlist '{}' finished playing (no loop option).", plist.name);
                }
                continue;
            }

            if (plist.segmentModeActive)
            {
                FMOD::Sound* sound = nullptr;
                result = plist.currentChannel->getCurrentSound(&sound);
                if (result == FMOD_OK && sound)
                {
                    unsigned int lengthMs = 0;
                    if (sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS) == FMOD_OK)
                    {
                        float lengthSec = lengthMs / 1000.0f;
                        float effectiveSegmentDuration = std::min(plist.segmentMaxDuration, lengthSec);
                        
                        unsigned int positionMs = 0;
                        bool trackFinished = false;
                        if (plist.currentChannel->getPosition(&positionMs, FMOD_TIMEUNIT_MS) == FMOD_OK) {
                         
                            if (lengthMs > 0 && (lengthMs - positionMs) < 500) { // 500ms threshold
                                trackFinished = true;
                            }
                        }
                        
                        plist.segmentTimer += deltaTime;
                        if (trackFinished || !isPlaying) {
                            // If track finished naturally, move to next one
                            if (plist.tracks.size() == 1)
                            {
                                StartTrackAtIndex(plist, plist.currentIndex);
                            }
                            else
                            {
                                StartNextTrack(plist);
                            }
                            continue;
                        }
                        else if (plist.segmentTimer >= effectiveSegmentDuration && lengthSec > effectiveSegmentDuration) {
                            // Only switch based on segment timer if the track is actually longer than segment duration
                            if (plist.tracks.size() == 1)
                            {
                                StartTrackAtIndex(plist, plist.currentIndex);
                            }
                            else
                            {
                                StartNextTrack(plist);
                            }
                            continue;
                        }
                    }
                }
            }
        }
        else
        {
            spdlog::warn("Invalid channel state detected, attempting to recover");
            StartTrackAtIndex(plist, plist.currentIndex);
        }
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
    if (!activePlaylist || !activePlaylist->currentChannel) 
        return 0.0f;

    unsigned int positionMs = 0;
    unsigned int lengthMs   = 0;
    FMOD::Sound* sound      = nullptr;

    FMOD_RESULT result = activePlaylist->currentChannel->getPosition(&positionMs, FMOD_TIMEUNIT_MS);
    if (result != FMOD_OK) return 0.0f;

    result = activePlaylist->currentChannel->getCurrentSound(&sound);
    if (result != FMOD_OK || !sound) return 0.0f;

    result = sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
    if (result != FMOD_OK || lengthMs == 0) return 0.0f;

    return static_cast<float>(positionMs) / static_cast<float>(lengthMs);
}

float PlaylistManager::GetSegmentProgress() const
{
    auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || !activePlaylist->segmentModeActive) return 0.0f;
    if (activePlaylist->segmentMaxDuration <= 0.0f) return 0.0f;
    return activePlaylist->segmentTimer / activePlaylist->segmentMaxDuration;
}

void PlaylistManager::SetCrossfadeDuration(float duration)
{
    if (m_currentPlaylist) {
        m_currentPlaylist->crossfadeDuration = duration;
    }
}

FMOD::Channel* PlaylistManager::GetCurrentChannel() const
{
    auto* activePlaylist = GetActivePlaylist();
    return activePlaylist ? activePlaylist->currentChannel : nullptr;
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
    return activePlaylist->crossfadeTimer / activePlaylist->crossfadeDuration;
}

FMOD::Channel* PlaylistManager::GetNextChannel() const
{
    auto* activePlaylist = GetActivePlaylist();
    return activePlaylist ? activePlaylist->nextChannel : nullptr;
}

const PlaylistManager::Playlist* PlaylistManager::GetActivePlaylist() const
{
    if (m_activePlaylistName.empty()) return nullptr;
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [this](const Playlist& p) { return p.name == m_activePlaylistName; });
    return (it != m_playlists.end()) ? &(*it) : nullptr;
}

PlaylistManager::Playlist* PlaylistManager::GetActivePlaylist()
{
    if (m_activePlaylistName.empty()) return nullptr;
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [this](Playlist& p) { return p.name == m_activePlaylistName; });
    return (it != m_playlists.end()) ? &(*it) : nullptr;
}

void PlaylistManager::StartNextTrack(Playlist& plist)
{
    int nextIndex = 0;
    if (plist.options.randomOrder)
    {
        plist.randomIndexPos++;
        if (plist.randomIndexPos >= (int)plist.randomIndices.size())
        {
            if (plist.options.loopPlaylist) {
                plist.randomIndexPos = 0;
            } else {
                plist.isPlaying = false;
                return;
            }
        }
        nextIndex = plist.randomIndices[plist.randomIndexPos];
    }
    else
    {
        nextIndex = plist.currentIndex + 1;
        if (nextIndex >= (int)plist.tracks.size())
        {
            if (plist.options.loopPlaylist) {
                nextIndex = 0;
            } else {
                plist.isPlaying = false;
                return;
            }
        }
    }

    plist.isCrossfading   = true;
    plist.crossfadeTimer  = 0.0f;

    plist.oldChannelVolume = 1.0f;
    if (plist.currentChannel)
    {
        float vol = 1.0f;
        plist.currentChannel->getVolume(&vol);
        plist.oldChannelVolume = vol;
    }

    float userVolume = UIManager::GetInstance().GetMasterVolume() 
                     * UIManager::GetInstance().GetMusicVolume();
    plist.nextTargetVolume = userVolume;

    std::string nextTrack = plist.tracks[nextIndex];
    FMOD::Channel* ch = AudioManager::GetInstance().PlaySound(nextTrack, false, 0.0f);
    plist.nextChannel = ch;

    plist.currentIndex = nextIndex;

    plist.segmentTimer = 0.0f;
    if (plist.segmentModeActive && ch)
    {
        FMOD::Sound* sound = nullptr;
        ch->getCurrentSound(&sound);
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

    float userVolume = UIManager::GetInstance().GetMasterVolume() 
                     * UIManager::GetInstance().GetMusicVolume();

    bool doLoop = (!plist.options.randomSegment && plist.options.loopPlaylist && plist.tracks.size() == 1);
    
    FMOD::Channel* ch = AudioManager::GetInstance().PlaySound(track, doLoop, userVolume);
    if (!ch) {
        spdlog::error("Failed to start track at index {}", index);
        return;
    }

    if (plist.currentChannel) {
        bool isPlaying = false;
        FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
        if (result == FMOD_OK && isPlaying) {
            plist.currentChannel->stop();
        }
    }

    plist.currentChannel = ch;

    bool isPlaying = false;
    FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
    if (result != FMOD_OK) {
        spdlog::error("Channel validation failed after start");
        return;
    }

    plist.segmentTimer = 0.0f;
    
    if (plist.options.randomSegment && ch)
    {
        plist.segmentModeActive = true;
        
        FMOD::Sound* sound = nullptr;
        ch->getCurrentSound(&sound);
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
    if (plist.currentChannel)
    {
        bool isPlaying = false;
        FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
        if (result == FMOD_OK && isPlaying) {
            plist.currentChannel->stop();
        }
        plist.currentChannel = nullptr;
    }

    plist.currentChannel = plist.nextChannel;
    plist.nextChannel    = nullptr;
    plist.isCrossfading  = false;
    plist.crossfadeTimer = 0.0f;

    if (plist.currentChannel)
    {
        bool isPlaying = false;
        FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
        if (result != FMOD_OK || !isPlaying)
        {
            spdlog::warn("New current channel invalid after crossfade, restarting track");
            StartTrackAtIndex(plist, plist.currentIndex);
        }
    }
}

void PlaylistManager::PrepareRandomOrder(Playlist& plist)
{
    int size = (int)plist.tracks.size();
    plist.randomIndices.resize(size);
    for (int i = 0; i < size; i++)
        plist.randomIndices[i] = i;

    std::shuffle(plist.randomIndices.begin(), plist.randomIndices.end(), m_rng);
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

    std::swap(plist.tracks[index], plist.tracks[index-1]);
}

void PlaylistManager::MoveTrackDown(const std::string& playlistName, int index)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p){ return p.name == playlistName; });
    if (it == m_playlists.end()) return;
    auto& plist = *it;
    if (index < 0 || index >= (int)plist.tracks.size() - 1) return;

    std::swap(plist.tracks[index], plist.tracks[index+1]);
}

void PlaylistManager::PlayFromIndex(const std::string& playlistName, int index)
{
    Stop(playlistName);

    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p){ return p.name == playlistName; });
    if (it == m_playlists.end()) return;

    Playlist& plist = *it;
    if (index < 0 || index >= (int)plist.tracks.size()) return;

    if (!plist.isPlaying) {
        PlaylistOptions opts;
        plist.options = opts;
    }
    
    plist.isPlaying = true;
    plist.currentIndex = index;
    plist.currentChannel = nullptr;
    plist.nextChannel = nullptr;
    plist.isCrossfading = false;
    plist.crossfadeTimer = 0.0f;

    plist.segmentModeActive = plist.options.randomSegment;
    plist.segmentTimer = 0.0f;

    StartTrackAtIndex(plist, index);
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
    
    StartNextTrack(plist);
    
    spdlog::info("Passage to next track in playlist '{}'.", playlistName);
}

} // namespace TSM
