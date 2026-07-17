// tsm_playlist_manager.cpp

#include "tsm_playlist_manager.h"
#include "tsm_audio_manager.h"
#include "tsm_fmod_wrapper.h"
#include <fstream>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
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
std::int64_t CurrentEpochSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

float CurrentNormalizationGain(FMOD::Channel* channel)
{
    const float gain = AudioManager::GetInstance()
        .GetNormalizationGainForChannel(channel);
    return std::isfinite(gain) &&
            gain > (std::numeric_limits<float>::epsilon)()
        ? gain
        : 1.0f;
}

float ScaleCapturedChannelVolume(
    float capturedVolume,
    float capturedNormalizationGain,
    FMOD::Channel* channel)
{
    if (!std::isfinite(capturedNormalizationGain) ||
        capturedNormalizationGain <= (std::numeric_limits<float>::epsilon)())
    {
        return capturedVolume;
    }
    return capturedVolume *
        CurrentNormalizationGain(channel) / capturedNormalizationGain;
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

std::string ResolveImportedTrackPath(
    const std::string& trackPath, const std::string& playlistFilePath)
{
    std::filesystem::path path = PathFromUtf8(trackPath);
    if (path.is_relative())
        path = PathFromUtf8(playlistFilePath).parent_path() / path;
    return PathToUtf8(path.lexically_normal());
}

bool AreSegmentDurationsValid(const PlaylistOptions& options)
{
    const auto valid = [](float value) {
        return std::isfinite(value) && value >= 0.001f && value <= 86400.0f;
    };
    return valid(options.segmentDuration) &&
        valid(options.minSegmentDuration) &&
        valid(options.maxSegmentDuration) &&
        options.minSegmentDuration <= options.maxSegmentDuration;
}

bool SamePlaylistOptions(
    const PlaylistOptions& left,
    const PlaylistOptions& right)
{
    return left.randomOrder == right.randomOrder &&
        left.randomSegment == right.randomSegment &&
        left.segmentDuration == right.segmentDuration &&
        left.automaticSegmentDuration == right.automaticSegmentDuration &&
        left.minSegmentDuration == right.minSegmentDuration &&
        left.maxSegmentDuration == right.maxSegmentDuration &&
        left.loopPlaylist == right.loopPlaylist;
}

constexpr std::size_t MaximumSmartTrackShortlistSize = 32u;
constexpr std::size_t SmartFineCandidatesPerKind = 128u;
constexpr std::size_t MaximumCoarseEntrySamples = 8u;
constexpr std::size_t CoarseFatigueSamplesPerWindow = 5u;

float UnitScore(float value, float fallback = 0.0f)
{
    return std::isfinite(value)
        ? std::clamp(value, 0.0f, 1.0f)
        : std::clamp(fallback, 0.0f, 1.0f);
}

double CoarseFatigueDecay(
    const ListeningHeatmap::State& state,
    std::int64_t selectionEpochSeconds)
{
    if (selectionEpochSeconds <= state.referenceEpochSeconds) return 1.0;
    if (!std::isfinite(state.dailyDecayFactor) ||
        state.dailyDecayFactor < 0.0 || state.dailyDecayFactor > 1.0)
        return 1.0;
    const double elapsedDays = static_cast<double>(
        selectionEpochSeconds - state.referenceEpochSeconds) / 86400.0;
    return std::clamp(
        std::pow(state.dailyDecayFactor, elapsedDays), 0.0, 1.0);
}

// This intentionally samples a fixed number of heatmap cells. The precise
// prefix-sum scoring remains in ChooseSegment, but coarse track shortlisting
// must never copy or scan a multi-hour heatmap on the audio control thread.
float CoarseExplorationScore(
    const ListeningHeatmap::State* state,
    float startSeconds,
    float durationSeconds,
    std::int64_t selectionEpochSeconds)
{
    if (!state || state->bucketFatigue.empty() ||
        !std::isfinite(state->bucketDurationSeconds) ||
        state->bucketDurationSeconds <= 0.0)
        return 1.0f;

    const double trackDuration = std::max(
        std::isfinite(state->trackDurationSeconds)
            ? state->trackDurationSeconds
            : 0.0,
        0.0);
    const double start = std::clamp(
        static_cast<double>(std::isfinite(startSeconds) ? startSeconds : 0.0f),
        0.0, trackDuration);
    const double end = std::clamp(
        start + static_cast<double>(
            std::isfinite(durationSeconds)
                ? std::max(durationSeconds, 0.0f)
                : 0.0f),
        start, trackDuration);
    if (end <= start) return 1.0f;

    const double decay = CoarseFatigueDecay(*state, selectionEpochSeconds);
    double fatigue = 0.0;
    for (std::size_t sample = 0u;
         sample < CoarseFatigueSamplesPerWindow;
         ++sample)
    {
        const double fraction =
            (static_cast<double>(sample) + 0.5) /
            static_cast<double>(CoarseFatigueSamplesPerWindow);
        const double time = start + (end - start) * fraction;
        const std::size_t bucket = std::min(
            static_cast<std::size_t>(
                std::floor(time / state->bucketDurationSeconds)),
            state->bucketFatigue.size() - 1u);
        const double value = state->bucketFatigue[bucket];
        fatigue += std::isfinite(value)
            ? std::clamp(value, 0.0, 1.0) * decay
            : 0.0;
    }
    const double mean = fatigue /
        static_cast<double>(CoarseFatigueSamplesPerWindow);
    return static_cast<float>(std::clamp(1.0 - mean, 0.0, 1.0));
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
                options, "automaticSegmentDuration",
                imported.options.automaticSegmentDuration, errorMessage) ||
            !ReadBooleanOption(
                options, "loopPlaylist", imported.options.loopPlaylist, errorMessage) ||
            !ReadDurationOption(
                options, "segmentDuration", 0.0, false, 86400.0,
                imported.options.segmentDuration, errorMessage) ||
            !ReadDurationOption(
                options, "minSegmentDuration", 0.0, false, 86400.0,
                imported.options.minSegmentDuration, errorMessage) ||
            !ReadDurationOption(
                options, "maxSegmentDuration", 0.0, false, 86400.0,
                imported.options.maxSegmentDuration, errorMessage) ||
            !ReadDurationOption(
                options, "crossfadeDuration", 0.0, true, 3600.0,
                imported.crossfadeDuration, errorMessage))
        {
            return false;
        }
        if (imported.options.minSegmentDuration >
            imported.options.maxSegmentDuration)
        {
            errorMessage =
                "Playlist option 'minSegmentDuration' cannot exceed "
                "'maxSegmentDuration'.";
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
        j["options"]["automaticSegmentDuration"] =
            it->options.automaticSegmentDuration;
        j["options"]["minSegmentDuration"] =
            it->options.minSegmentDuration;
        j["options"]["maxSegmentDuration"] =
            it->options.maxSegmentDuration;
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
            playlistJson["options"]["automaticSegmentDuration"] =
                playlist.options.automaticSegmentDuration;
            playlistJson["options"]["minSegmentDuration"] =
                playlist.options.minSegmentDuration;
            playlistJson["options"]["maxSegmentDuration"] =
                playlist.options.maxSegmentDuration;
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
    plist.currentSegment = {};
    plist.nextSegment = {};
    plist.lastSmartCandidatePoolSize = 0u;
    plist.lastSmartShortlistSize = 0u;
    plist.lastSmartFineEvaluationCount = 0u;
    plist.lastSmartFineCandidateBudgetPerKind = 0u;
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
    bool capturedNextChannel = false;
    const bool currentPlaying = IsOwnedChannelPlaying(
        playlist->currentChannel, playlist->expectedCurrentSound);
    const bool nextPlaying = playlist->isCrossfading &&
        IsOwnedChannelPlaying(
            playlist->nextChannel, playlist->expectedNextSound);
    bool nextIsDominant = nextPlaying && !currentPlaying;
    if (currentPlaying && nextPlaying)
    {
        // FMOD channel volumes include each track's loudness-normalization
        // gain. Comparing those raw values can therefore select the quieter
        // programme side even when its crossfade envelope is dominant. The
        // equal-power progression is the source of truth for recovery.
        const float progress = playlist->activeCrossfadeDuration <= 0.0f
            ? 1.0f
            : playlist->crossfadeTimer /
                playlist->activeCrossfadeDuration;
        const TransitionLogic::EqualPowerGains gains =
            TransitionLogic::CalculateEqualPowerGains(progress);
        nextIsDominant = gains.incoming >= gains.outgoing;
    }

    if (nextIsDominant)
    {
        channel = playlist->nextChannel;
        expectedSound = playlist->expectedNextSound;
        trackIndex = playlist->nextIndex;
        capturedNextChannel = true;
    }
    else if (currentPlaying)
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
    const Playlist::SegmentRuntime& capturedSegment = capturedNextChannel
        ? playlist->nextSegment
        : playlist->currentSegment;
    state.segmentActive = capturedSegment.active;

    const auto secondsToMilliseconds = [](float seconds) {
        if (!std::isfinite(seconds) || seconds <= 0.0f) return std::uint32_t{0};
        const double milliseconds = static_cast<double>(seconds) * 1000.0;
        return static_cast<std::uint32_t>(std::llround(std::min(
            milliseconds,
            static_cast<double>((std::numeric_limits<std::uint32_t>::max)()))));
    };
    if (state.segmentActive)
    {
        state.segmentStartMs = secondsToMilliseconds(capturedSegment.startSeconds);
        state.segmentElapsedMs = secondsToMilliseconds(capturedSegment.elapsedSeconds);
        state.segmentDurationMs =
            secondsToMilliseconds(capturedSegment.durationSeconds);
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
    if (!std::isfinite(state.options.minSegmentDuration) ||
        state.options.minSegmentDuration <= 0.0f ||
        state.options.minSegmentDuration > 86400.0f ||
        !std::isfinite(state.options.maxSegmentDuration) ||
        state.options.maxSegmentDuration <= 0.0f ||
        state.options.maxSegmentDuration > 86400.0f ||
        state.options.minSegmentDuration > state.options.maxSegmentDuration)
    {
        errorMessage = "The saved automatic segment range is invalid.";
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
        const std::uint64_t segmentEndMs =
            static_cast<std::uint64_t>(state.segmentStartMs) +
            static_cast<std::uint64_t>(state.segmentDurationMs);
        if (state.segmentStartMs >= lengthMs ||
            state.segmentDurationMs == 0 ||
            state.segmentElapsedMs > state.segmentDurationMs ||
            segmentEndMs > static_cast<std::uint64_t>(lengthMs) + 2u)
        {
            errorMessage = "The saved random segment position is invalid.";
            return false;
        }

        const std::uint64_t expectedPositionMs =
            static_cast<std::uint64_t>(state.segmentStartMs) +
            static_cast<std::uint64_t>(state.segmentElapsedMs);
        const std::uint64_t actualPositionMs = state.positionMs;
        const std::uint64_t positionDifferenceMs =
            expectedPositionMs > actualPositionMs
                ? expectedPositionMs - actualPositionMs
                : actualPositionMs - expectedPositionMs;
        // Checkpoints are produced on a periodic control tick and the FMOD
        // cursor can lag logical time briefly. Larger disagreement indicates
        // a corrupt or mismatched checkpoint rather than scheduler jitter.
        if (positionDifferenceMs > 2000u)
        {
            errorMessage =
                "The saved audio position does not match its segment progress.";
            return false;
        }
    }
    else if (state.segmentStartMs != 0 || state.segmentElapsedMs != 0 ||
             state.segmentDurationMs != 0)
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
            static_cast<std::uint64_t>(state.segmentDurationMs);
        const unsigned int maximumSegmentPosition = static_cast<unsigned int>(
            std::min<std::uint64_t>(lengthMs - 1u, segmentEndMs));
        clampedPositionMs = std::clamp(
            clampedPositionMs, state.segmentStartMs, maximumSegmentPosition);
    }
    FMOD::Channel* stagedChannel = AudioManager::GetInstance().PlayMusic(
        state.trackId, false, 0.0f);
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
    if (result == FMOD_OK)
    {
        // PlayMusic creates its FMOD channel paused internally, but normally
        // releases it before returning. Keeping the public staging call at
        // zero volume guarantees that no sample can escape before this seek.
        // Restore the normalized programme gain while it is paused, then make
        // the fully configured channel audible in one final step.
        result = stagedChannel->setVolume(
            AudioManager::GetInstance().GetNormalizationGainForChannel(
                stagedChannel));
    }
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
    playlist->currentSegment.active = state.segmentActive;
    playlist->currentSegment.durationSeconds = state.segmentActive
        ? state.segmentDurationMs / 1000.0f
        : lengthMs / 1000.0f;
    playlist->currentSegment.startSeconds = state.segmentStartMs / 1000.0f;
    playlist->currentSegment.elapsedSeconds = state.segmentActive
        ? state.segmentElapsedMs / 1000.0f
        : clampedPositionMs / 1000.0f;
    if (state.segmentActive)
    {
        SegmentDecisionInfo& decision = playlist->currentSegment.decision;
        decision.active = true;
        decision.mode = "restored_checkpoint";
        decision.startSeconds = playlist->currentSegment.startSeconds;
        decision.durationSeconds = playlist->currentSegment.durationSeconds;
        decision.endSeconds = decision.startSeconds + decision.durationSeconds;
        decision.reasons = {"restored from durable playback checkpoint"};

        if (const auto* analysis =
                AudioManager::GetInstance().GetMusicAnalysis(state.trackId);
            analysis && !analysis->frames.empty())
        {
            const float exitTime = decision.endSeconds;
            const auto frame = std::lower_bound(
                analysis->frames.begin(), analysis->frames.end(), exitTime,
                [](const MusicAnalysis::Frame& candidate, float time) {
                    return candidate.timeSeconds < time;
                });
            const std::size_t index = frame == analysis->frames.end()
                ? analysis->frames.size() - 1u
                : static_cast<std::size_t>(
                      std::distance(analysis->frames.begin(), frame));
            playlist->currentSegment.exitProfile =
                MusicAnalysis::BuildBoundaryProfile(
                    analysis->frames,
                    index,
                    MusicAnalysis::CandidateKind::Exit);
        }
    }
    playlist->nextSegment = {};
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

bool PlaylistManager::ConfigurePlaylistPlayback(
    const std::string& playlistName,
    const PlaylistOptions& options,
    float crossfadeDuration,
    bool restartIfPlaying)
{
    if (!AreSegmentDurationsValid(options) ||
        !std::isfinite(crossfadeDuration) ||
        crossfadeDuration < 0.0f || crossfadeDuration > 3600.0f)
        return false;

    Playlist* playlist = GetPlaylistByName(playlistName);
    if (!playlist) return false;
    const bool wasPlaying = playlist->isPlaying;
    const bool runtimeOptionsChanged =
        !SamePlaylistOptions(playlist->options, options);
    if (wasPlaying && runtimeOptionsChanged && !restartIfPlaying)
        return false;

    if (wasPlaying && runtimeOptionsChanged)
    {
        // Preserve the complete live runtime until the replacement has
        // actually opened a valid FMOD channel. Stop() only schedules its
        // owned channels to fade, so a failed restart can cancel those fades
        // and restore the exact programme state instead of leaving silence.
        const Playlist previousRuntime = *playlist;
        const std::string previousActivePlaylist = m_activePlaylistName;
        const PlaybackState previousCheckpoint = CapturePlaybackState();
        float previousCurrentVolume = 0.0f;
        float previousNextVolume = 0.0f;
        const bool hadCurrentVolume = previousRuntime.currentChannel &&
            previousRuntime.currentChannel->getVolume(
                &previousCurrentVolume) == FMOD_OK;
        const bool hadNextVolume = previousRuntime.nextChannel &&
            previousRuntime.nextChannel->getVolume(&previousNextVolume) == FMOD_OK;

        Stop(playlistName);
        playlist = GetPlaylistByName(playlistName);
        if (!playlist) return false;
        playlist->crossfadeDuration = crossfadeDuration;
        if (!StartPlayback(*playlist, options))
        {
            playlist = GetPlaylistByName(playlistName);
            if (!playlist) return false;
            StopOwnedChannelImmediately(
                playlist->currentChannel, playlist->expectedCurrentSound);
            StopOwnedChannelImmediately(
                playlist->nextChannel, playlist->expectedNextSound);
            *playlist = previousRuntime;
            m_activePlaylistName = previousActivePlaylist;

            const bool currentSurvived = IsOwnedChannelPlaying(
                playlist->currentChannel, playlist->expectedCurrentSound);
            const bool nextSurvived = IsOwnedChannelPlaying(
                playlist->nextChannel, playlist->expectedNextSound);
            if (currentSurvived || nextSurvived)
            {
                if (currentSurvived)
                {
                    AudioManager::GetInstance().SetChannelVolume(
                        playlist->currentChannel,
                        hadCurrentVolume
                            ? previousCurrentVolume
                            : AudioManager::GetInstance()
                                .GetNormalizationGainForChannel(
                                    playlist->currentChannel));
                    if (!playlist->isCrossfading)
                    {
                        AudioManager::GetInstance().ReconcileMusicChannelVolume(
                            playlist->currentChannel);
                    }
                }
                if (nextSurvived)
                {
                    AudioManager::GetInstance().SetChannelVolume(
                        playlist->nextChannel,
                        hadNextVolume ? previousNextVolume : 0.0f);
                }
                spdlog::error(
                    "Unable to apply playback options for '{}'; the previous "
                    "live programme state was restored.",
                    playlistName);
                return false;
            }

            std::string restoreError;
            if (previousCheckpoint.isPlaying &&
                ResumePlaybackState(previousCheckpoint, restoreError))
            {
                spdlog::error(
                    "Unable to apply playback options for '{}'; playback was "
                    "restored from its checkpoint.",
                    playlistName);
                return false;
            }
            ClearLogicalPlaybackState(*playlist);
            m_activePlaylistName.clear();
            spdlog::critical(
                "Unable to apply playback options for '{}' or restore its "
                "previous channel: {}",
                playlistName,
                restoreError.empty() ? "no valid recovery checkpoint" : restoreError);
            return false;
        }
    }
    else
    {
        playlist->options = options;
        playlist->crossfadeDuration = crossfadeDuration;
    }
    NotifyPlaylistChanged();
    return true;
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
    if (!AreSegmentDurationsValid(options))
    {
        spdlog::error(
            "Playback source '{}' has invalid segment duration settings.",
            plist.name);
        return false;
    }

    if (GetActivePlaylist()) Stop(m_activePlaylistName);
    
    plist.options = options;
    plist.currentSegment = {};
    plist.nextSegment = {};

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
            auto& audioManager = AudioManager::GetInstance();
            const float safeDeltaTime = std::max(deltaTime, 0.0f);
            const float previousCurrentElapsed =
                plist.currentSegment.elapsedSeconds;
            const float previousNextElapsed = plist.nextSegment.elapsedSeconds;
            if (plist.currentSegment.durationSeconds > 0.0f)
                plist.currentSegment.elapsedSeconds = std::min(
                    plist.currentSegment.elapsedSeconds + safeDeltaTime,
                    plist.currentSegment.durationSeconds);
            if (plist.nextSegment.durationSeconds > 0.0f)
                plist.nextSegment.elapsedSeconds = std::min(
                    plist.nextSegment.elapsedSeconds + safeDeltaTime,
                    plist.nextSegment.durationSeconds);
            plist.crossfadeTimer += safeDeltaTime;
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
                // Scale from the volume that was actually audible at the
                // transition boundary. This prevents a jump if a transition
                // begins while another controlled gain change is settling.
                const float volOld = gains.outgoing * ScaleCapturedChannelVolume(
                    plist.oldChannelVolume,
                    plist.oldChannelNormalizationGain,
                    plist.currentChannel);
                audioManager.SetChannelVolume(plist.currentChannel, volOld);
                if (safeDeltaTime > 0.0f && plist.currentIndex >= 0 &&
                    plist.currentIndex < static_cast<int>(plist.tracks.size()))
                {
                    audioManager.RecordListeningCoverage(
                        plist.tracks[plist.currentIndex],
                        plist.currentSegment.startSeconds + previousCurrentElapsed,
                        plist.currentSegment.startSeconds +
                            plist.currentSegment.elapsedSeconds,
                        gains.outgoing * gains.outgoing,
                        CurrentEpochSeconds());
                }
            }

            if (IsOwnedChannelPlaying(plist.nextChannel, plist.expectedNextSound))
            {
                nextChannelValid = true;
                const float volNext = gains.incoming * baseMusicVol *
                    CurrentNormalizationGain(plist.nextChannel);
                audioManager.SetChannelVolume(plist.nextChannel, volNext);
                if (safeDeltaTime > 0.0f && plist.nextIndex >= 0 &&
                    plist.nextIndex < static_cast<int>(plist.tracks.size()))
                {
                    audioManager.RecordListeningCoverage(
                        plist.tracks[plist.nextIndex],
                        plist.nextSegment.startSeconds + previousNextElapsed,
                        plist.nextSegment.startSeconds +
                            plist.nextSegment.elapsedSeconds,
                        gains.incoming * gains.incoming,
                        CurrentEpochSeconds());
                }
            }

            if (!currentChannelValid && nextChannelValid)
            {
                // The prepared channel is already the only audible survivor;
                // promote it immediately instead of stretching a fade from
                // silence for the remainder of the configured overlap.
                audioManager.SetChannelVolume(
                    plist.nextChannel,
                    CurrentNormalizationGain(plist.nextChannel));
                FinishCrossfade(plist);
                return;
            }
            if (currentChannelValid && !nextChannelValid)
            {
                // Keep programme audio continuous when the staged channel
                // fails. The next boundary may attempt another transition,
                // but the current track is restored now without a silent gap.
                StopOwnedChannelImmediately(
                    plist.nextChannel, plist.expectedNextSound);
                plist.nextChannel = nullptr;
                plist.expectedNextSound = nullptr;
                plist.nextIndex = -1;
                plist.nextSegment = {};
                plist.isCrossfading = false;
                plist.crossfadeTimer = 0.0f;
                plist.lastTransitionReason =
                    TransitionLogic::Reason::PlaybackFailure;
                // The staged programme disappeared. Restore the surviving
                // track to its current normalized target immediately; the
                // captured boundary volume may legitimately be zero when an
                // operator skips during the initial fade-in.
                audioManager.SetChannelVolume(
                    plist.currentChannel,
                    CurrentNormalizationGain(plist.currentChannel));
                return;
            }
            if (t >= 1.0f || (!currentChannelValid && !nextChannelValid))
            {
                FinishCrossfade(plist);
                return;
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
            const float previousElapsed = plist.currentSegment.elapsedSeconds;
            const float safeDeltaTime = std::max(deltaTime, 0.0f);
            if (plist.currentSegment.durationSeconds > 0.0f)
            {
                plist.currentSegment.elapsedSeconds = std::min(
                    plist.currentSegment.elapsedSeconds + safeDeltaTime,
                    plist.currentSegment.durationSeconds);
            }
            if (plist.currentSegment.active)
            {
                segmentRemaining = std::max(
                    plist.currentSegment.durationSeconds -
                        plist.currentSegment.elapsedSeconds,
                    0.0f);
            }
            if (safeDeltaTime > 0.0f && plist.currentIndex >= 0 &&
                plist.currentIndex < static_cast<int>(plist.tracks.size()))
            {
                AudioManager::GetInstance().RecordListeningCoverage(
                    plist.tracks[plist.currentIndex],
                    plist.currentSegment.startSeconds + previousElapsed,
                    plist.currentSegment.startSeconds +
                        plist.currentSegment.elapsedSeconds,
                    1.0,
                    CurrentEpochSeconds());
            }

            const TransitionLogic::Decision decision = TransitionLogic::Evaluate(
                trackRemaining, segmentRemaining, plist.currentSegment.active,
                plist.currentSegment.active
                    ? std::min(
                          plist.crossfadeDuration,
                          plist.currentSegment.durationSeconds * 0.25f)
                    : plist.crossfadeDuration,
                HasNextTrack(plist), isPlaying);
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
    if (!activePlaylist || !activePlaylist->currentSegment.active) return 0.0f;
    if (activePlaylist->currentSegment.durationSeconds <= 0.0f) return 0.0f;
    return std::clamp(
        activePlaylist->currentSegment.elapsedSeconds /
            activePlaylist->currentSegment.durationSeconds,
        0.0f, 1.0f);
}

float PlaylistManager::GetSegmentDuration() const
{
    const auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || !activePlaylist->currentSegment.active) return 0.0f;
    return std::max(activePlaylist->currentSegment.durationSeconds, 0.0f);
}

float PlaylistManager::GetSegmentRemainingTime() const
{
    const auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || !activePlaylist->currentSegment.active) return 0.0f;
    return std::max(
        activePlaylist->currentSegment.durationSeconds -
            activePlaylist->currentSegment.elapsedSeconds,
        0.0f);
}

SegmentDecisionInfo PlaylistManager::GetSegmentDecisionInfo() const
{
    const Playlist* playlist = GetActivePlaylist();
    return playlist ? playlist->currentSegment.decision : SegmentDecisionInfo{};
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
    std::optional<MusicAnalysis::SegmentDecision> plannedDecision;
    if (plist.options.randomOrder)
    {
        if (plist.randomIndices.empty())
        {
            FinishPlaylist(plist);
            return;
        }

        int nextPosition = plist.randomIndexPos + 1;
        if (nextPosition >= static_cast<int>(plist.randomIndices.size()))
        {
            if (plist.options.loopPlaylist) {
                nextPosition = 0;
            } else {
                FinishPlaylist(plist);
                return;
            }
        }
        if (plist.options.randomSegment &&
            plist.options.automaticSegmentDuration)
        {
            nextIndex = SelectCompatibleRandomTrack(
                plist, nextPosition, plannedDecision);
        }
        else
        {
            nextIndex = plist.randomIndices[nextPosition];
        }
        plist.randomIndexPos = nextPosition;
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
        : (reason == TransitionLogic::Reason::ManualSkip
               ? std::min(std::max(plist.crossfadeDuration, 0.0f), 2.0f)
               : std::max(plist.crossfadeDuration, 0.0f));

    plist.oldChannelVolume = 1.0f;
    plist.oldChannelNormalizationGain = 1.0f;
    if (IsOwnedChannelPlaying(plist.currentChannel, plist.expectedCurrentSound))
    {
        float vol = 1.0f;
        plist.currentChannel->getVolume(&vol);
        plist.oldChannelVolume = vol;
        plist.oldChannelNormalizationGain =
            CurrentNormalizationGain(plist.currentChannel);
    }

    std::string nextTrack = plist.tracks[nextIndex];
    FMOD::Sound* expectedSound = AudioManager::GetInstance().GetSound(nextTrack);
    FMOD::Channel* ch = AudioManager::GetInstance().PlayMusic(nextTrack, false, 0.0f);
    if (!ch || !IsOwnedChannelPlaying(ch, expectedSound))
    {
        StopOwnedChannelImmediately(ch, expectedSound);
        spdlog::error("Failed to start next track '{}' in playlist '{}'.", nextTrack, plist.name);
        plist.nextIndex = -1;
        plist.nextSegment = {};
        plist.isCrossfading = false;
        plist.crossfadeTimer = 0.0f;
        plist.lastTransitionReason = TransitionLogic::Reason::PlaybackFailure;
        if (IsOwnedChannelPlaying(
                plist.currentChannel, plist.expectedCurrentSound))
        {
            // Staging failed before the playlist took ownership of the
            // outgoing envelope. Preserve (or resume) its existing fade-in
            // instead of cancelling it at a possibly silent captured volume.
            AudioManager::GetInstance().ReconcileMusicChannelVolume(
                plist.currentChannel);
        }
        else
        {
            FinishPlaylist(plist);
        }
        return;
    }
    plist.nextChannel = ch;
    plist.expectedNextSound = expectedSound;

    plist.nextIndex = nextIndex;

    plist.nextSegment = PrepareRandomSegment(
        plist, nextTrack, ch, expectedSound, plannedDecision,
        plist.currentSegment.exitProfile);

    // Complete the overlap in the first half of the incoming material. This
    // guarantees that even a very short track or selected segment reaches its
    // normalized programme level before its own boundary. Unknown, invalid or
    // effectively empty durations are promoted immediately instead of risking
    // a fade whose staged channel has already ended.
    constexpr float MinimumIncomingDurationSeconds = 0.001f;
    const float incomingAvailable =
        plist.nextSegment.durationSeconds - plist.nextSegment.elapsedSeconds;
    if (!std::isfinite(incomingAvailable) ||
        incomingAvailable <= MinimumIncomingDurationSeconds)
    {
        plist.activeCrossfadeDuration = 0.0f;
    }
    else
    {
        plist.activeCrossfadeDuration = std::min(
            plist.activeCrossfadeDuration,
            incomingAvailable * 0.5f);
    }
    if (plist.activeCrossfadeDuration <= 0.0f)
    {
        // A zero-length transition is a hard handoff, not a one-frame fade
        // from silence. Make the prepared channel audible before promotion.
        AudioManager::GetInstance().SetChannelVolume(
            plist.nextChannel,
            CurrentNormalizationGain(plist.nextChannel));
        FinishCrossfade(plist);
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

    plist.currentSegment = PrepareRandomSegment(
        plist, track, ch, expectedSound);
}

PlaylistManager::Playlist::SegmentRuntime PlaylistManager::PrepareRandomSegment(
    Playlist& plist, const std::string& trackId,
    FMOD::Channel* channel, FMOD::Sound* expectedSound,
    const std::optional<MusicAnalysis::SegmentDecision>& plannedDecision,
    const std::optional<MusicAnalysis::Profile>& outgoingExit)
{
    Playlist::SegmentRuntime segment;

    if (!channel || !expectedSound) return segment;
    if (!AreSegmentDurationsValid(plist.options))
    {
        spdlog::error(
            "Playback source '{}' has invalid segment duration settings.",
            plist.name);
        return segment;
    }

    unsigned int lengthMs = 0;
    if (expectedSound->getLength(&lengthMs, FMOD_TIMEUNIT_MS) != FMOD_OK ||
        lengthMs == 0)
    {
        spdlog::warn(
            "Unable to determine track duration for random segment playback in '{}'.",
            plist.name);
        return segment;
    }

    const float lengthSeconds = lengthMs / 1000.0f;
    segment.durationSeconds = lengthSeconds;
    if (!plist.options.randomSegment) return segment;
    float requestedDuration = plist.options.segmentDuration;
    if (plist.options.automaticSegmentDuration)
    {
        const bool decisionWasPlanned = plannedDecision.has_value();
        std::optional<MusicAnalysis::SegmentDecision> smartDecision =
            plannedDecision;
        if (!smartDecision)
        {
            if (const auto* analysis =
                    AudioManager::GetInstance().GetMusicAnalysis(trackId))
            {
                MusicAnalysis::SegmentSelectionOptions selection;
                selection.minDurationSeconds = plist.options.minSegmentDuration;
                selection.targetDurationSeconds = std::clamp(
                    plist.options.segmentDuration,
                    plist.options.minSegmentDuration,
                    plist.options.maxSegmentDuration);
                selection.maxDurationSeconds = plist.options.maxSegmentDuration;
                selection.listeningFatigue =
                    AudioManager::GetInstance().GetListeningFatigue(trackId);
                selection.selectionEpochSeconds = CurrentEpochSeconds();
                selection.maxCandidatesPerKind =
                    SmartFineCandidatesPerKind;
                smartDecision = MusicAnalysis::ChooseSegment(
                    *analysis, selection, m_rng, outgoingExit);
            }
        }

        if (smartDecision && !decisionWasPlanned && outgoingExit &&
            plist.currentIndex >= 0 &&
            plist.currentIndex < static_cast<int>(plist.tracks.size()))
        {
            smartDecision->transitionDiversityScore = static_cast<float>(
                AudioManager::GetInstance().GetTransitionDiversityScore(
                    plist.tracks[plist.currentIndex],
                    trackId,
                    CurrentEpochSeconds()));
            smartDecision->score = std::clamp(
                0.80f * smartDecision->score +
                    0.20f * smartDecision->transitionDiversityScore,
                0.0f, 1.0f);
            if (smartDecision->transitionDiversityScore >= 0.70f)
            {
                smartDecision->reasons.push_back(
                    MusicAnalysis::SegmentReason::FreshTransitionPair);
            }
        }

        if (smartDecision && smartDecision->durationSeconds > 0.0f &&
            smartDecision->startSeconds >= 0.0f &&
            smartDecision->endSeconds <= lengthSeconds + 0.01f)
        {
            segment.startSeconds = std::clamp(
                smartDecision->startSeconds, 0.0f,
                std::max(lengthSeconds - 0.001f, 0.0f));
            segment.durationSeconds = std::min(
                smartDecision->durationSeconds,
                lengthSeconds - segment.startSeconds);
            segment.active = segment.durationSeconds > 0.0f;
            segment.exitProfile = smartDecision->selectedExit.profile;
            segment.decision.active = segment.active;
            segment.decision.smartAnalysis = true;
            segment.decision.mode = "smart";
            segment.decision.fullTrack = smartDecision->fullTrack;
            segment.decision.startSeconds = segment.startSeconds;
            segment.decision.endSeconds =
                segment.startSeconds + segment.durationSeconds;
            segment.decision.durationSeconds = segment.durationSeconds;
            segment.decision.entryScore = smartDecision->selectedEntry.score;
            segment.decision.exitScore = smartDecision->selectedExit.score;
            segment.decision.totalScore = smartDecision->score;
            segment.decision.explorationScore =
                smartDecision->explorationScore;
            segment.decision.transitionDiversityScore =
                smartDecision->transitionDiversityScore;
            if (outgoingExit)
            {
                segment.decision.transitionScore =
                    MusicAnalysis::ScoreCompatibility(
                        *outgoingExit,
                        smartDecision->selectedEntry.profile).total;
            }
            for (const MusicAnalysis::SegmentReason reason : smartDecision->reasons)
                segment.decision.reasons.emplace_back(MusicAnalysis::ToString(reason));
        }
        else
        {
            const float maximumDuration = std::min(
                plist.options.maxSegmentDuration, lengthSeconds);
            const float minimumDuration = std::min(
                plist.options.minSegmentDuration, maximumDuration);
            std::uniform_real_distribution<float> durationDistribution(
                minimumDuration, maximumDuration);
            requestedDuration = durationDistribution(m_rng);
            segment.durationSeconds = std::min(requestedDuration, lengthSeconds);
            segment.active = true;
            const float maximumStart =
                std::max(lengthSeconds - segment.durationSeconds, 0.0f);
            std::uniform_real_distribution<float> startDistribution(
                0.0f, maximumStart);
            segment.startSeconds = startDistribution(m_rng);
            segment.decision.active = true;
            segment.decision.mode = "bounded_random_fallback";
            segment.decision.startSeconds = segment.startSeconds;
            segment.decision.endSeconds =
                segment.startSeconds + segment.durationSeconds;
            segment.decision.durationSeconds = segment.durationSeconds;
            segment.decision.fullTrack =
                segment.startSeconds <= 0.001f &&
                segment.durationSeconds + 0.001f >= lengthSeconds;
            segment.decision.reasons = {
                "analysis unavailable: bounded random fallback"};
        }
    }
    else
    {
        segment.durationSeconds = std::min(requestedDuration, lengthSeconds);
        segment.active = true;
        const float maximumStart =
            std::max(lengthSeconds - segment.durationSeconds, 0.0f);
        std::uniform_real_distribution<float> startDistribution(0.0f, maximumStart);
        segment.startSeconds = startDistribution(m_rng);
        segment.decision.active = true;
        segment.decision.mode = "fixed_random_entry";
        segment.decision.startSeconds = segment.startSeconds;
        segment.decision.endSeconds =
            segment.startSeconds + segment.durationSeconds;
        segment.decision.durationSeconds = segment.durationSeconds;
        segment.decision.fullTrack =
            segment.startSeconds <= 0.001f &&
            segment.durationSeconds + 0.001f >= lengthSeconds;
        segment.decision.reasons = {"fixed duration with random entry"};
    }

    const auto startMs = static_cast<unsigned int>(std::min(
        static_cast<double>(lengthMs - 1u),
        static_cast<double>(segment.startSeconds) * 1000.0));
    if (channel->setPosition(startMs, FMOD_TIMEUNIT_MS) != FMOD_OK)
    {
        spdlog::warn(
            "Unable to seek to a random segment in playback source '{}'.",
            plist.name);
        return {};
    }
    return segment;
}

int PlaylistManager::SelectCompatibleRandomTrack(
    Playlist& plist, int firstCandidatePosition,
    std::optional<MusicAnalysis::SegmentDecision>& selectedDecision)
{
    struct CoarseTrack
    {
        int permutationPosition = -1;
        int trackIndex = -1;
        const MusicAnalysis::TrackAnalysis* analysis = nullptr;
        float entryQuality = 0.0f;
        float acousticCompatibility = 0.5f;
        float transitionDiversity = 1.0f;
        float exploration = 1.0f;
        float combinedScore = 0.0f;
    };

    struct RankedTrack
    {
        int permutationPosition = -1;
        int trackIndex = -1;
        MusicAnalysis::SegmentDecision decision;
    };

    selectedDecision.reset();
    plist.lastSmartCandidatePoolSize = 0u;
    plist.lastSmartShortlistSize = 0u;
    plist.lastSmartFineEvaluationCount = 0u;
    plist.lastSmartFineCandidateBudgetPerKind = 0u;
    if (firstCandidatePosition < 0 ||
        firstCandidatePosition >= static_cast<int>(plist.randomIndices.size()))
        return plist.currentIndex;

    const std::optional<MusicAnalysis::Profile> outgoing =
        plist.currentSegment.exitProfile;
    MusicAnalysis::SegmentSelectionOptions selection;
    selection.minDurationSeconds = plist.options.minSegmentDuration;
    selection.targetDurationSeconds = std::clamp(
        plist.options.segmentDuration,
        plist.options.minSegmentDuration,
        plist.options.maxSegmentDuration);
    selection.maxDurationSeconds = plist.options.maxSegmentDuration;
    selection.selectionEpochSeconds = CurrentEpochSeconds();
    selection.maxCandidatesPerKind = SmartFineCandidatesPerKind;
    plist.lastSmartFineCandidateBudgetPerKind =
        selection.maxCandidatesPerKind;

    AudioManager& audioManager = AudioManager::GetInstance();
    std::vector<CoarseTrack> candidatePool;
    candidatePool.reserve(
        plist.randomIndices.size() -
        static_cast<std::size_t>(firstCandidatePosition));
    int fallbackPosition = -1;

    for (int position = firstCandidatePosition;
         position < static_cast<int>(plist.randomIndices.size()); ++position)
    {
        const int index = plist.randomIndices[position];
        if (plist.randomIndices.size() > 1u && index == plist.currentIndex) continue;
        if (fallbackPosition < 0) fallbackPosition = position;
        const auto* analysis = audioManager.GetMusicAnalysis(plist.tracks[index]);
        if (!analysis) continue;

        CoarseTrack candidate;
        candidate.permutationPosition = position;
        candidate.trackIndex = index;
        candidate.analysis = analysis;
        const ListeningHeatmap::State* fatigue =
            audioManager.GetListeningFatigue(plist.tracks[index]);

        const std::size_t entryCount = analysis->entryCandidates.size();
        const std::size_t sampleCount = std::min(
            entryCount, MaximumCoarseEntrySamples);
        candidate.exploration = fatigue ? 0.0f : 1.0f;
        for (std::size_t sample = 0u; sample < sampleCount; ++sample)
        {
            const std::size_t entryIndex = sampleCount <= 1u
                ? 0u
                : sample * (entryCount - 1u) / (sampleCount - 1u);
            const MusicAnalysis::Candidate& entry =
                analysis->entryCandidates[entryIndex];
            candidate.entryQuality = std::max(
                candidate.entryQuality, UnitScore(entry.score));
            if (outgoing)
            {
                candidate.acousticCompatibility = std::max(
                    sample == 0u ? 0.0f : candidate.acousticCompatibility,
                    UnitScore(MusicAnalysis::ScoreCompatibility(
                        *outgoing, entry.profile).total));
            }
            candidate.exploration = std::max(
                candidate.exploration,
                CoarseExplorationScore(
                    fatigue,
                    entry.timeSeconds,
                    selection.targetDurationSeconds,
                    selection.selectionEpochSeconds));
        }
        if (sampleCount == 0u)
        {
            candidate.exploration = CoarseExplorationScore(
                fatigue, 0.0f, selection.targetDurationSeconds,
                selection.selectionEpochSeconds);
        }

        if (plist.currentIndex >= 0 &&
            plist.currentIndex < static_cast<int>(plist.tracks.size()))
        {
            candidate.transitionDiversity = UnitScore(static_cast<float>(
                audioManager.GetTransitionDiversityScore(
                    plist.tracks[plist.currentIndex],
                    plist.tracks[index],
                    selection.selectionEpochSeconds)),
                1.0f);
        }
        candidate.combinedScore = outgoing
            ? 0.35f * candidate.acousticCompatibility +
                0.20f * candidate.entryQuality +
                0.25f * candidate.transitionDiversity +
                0.20f * candidate.exploration
            : 0.30f * candidate.entryQuality +
                0.30f * candidate.transitionDiversity +
                0.40f * candidate.exploration;
        candidate.combinedScore = UnitScore(candidate.combinedScore);
        candidatePool.push_back(candidate);
    }

    plist.lastSmartCandidatePoolSize = candidatePool.size();
    std::vector<const CoarseTrack*> shortlist;
    shortlist.reserve(std::min(
        candidatePool.size(), MaximumSmartTrackShortlistSize));
    const auto alreadySelected = [&](const CoarseTrack& candidate) {
        return std::any_of(
            shortlist.begin(), shortlist.end(), [&](const CoarseTrack* selected) {
                return selected->permutationPosition ==
                    candidate.permutationPosition;
            });
    };
    const auto appendBest = [&](std::size_t quota, const auto& score) {
        std::vector<const CoarseTrack*> best;
        best.reserve(quota);
        const auto better = [&](const CoarseTrack* left, const CoarseTrack* right) {
            const float leftScore = UnitScore(score(*left));
            const float rightScore = UnitScore(score(*right));
            if (std::fabs(leftScore - rightScore) > 0.000001f)
                return leftScore > rightScore;
            return left->permutationPosition < right->permutationPosition;
        };
        for (const CoarseTrack& candidate : candidatePool)
        {
            if (alreadySelected(candidate)) continue;
            const auto insertion = std::find_if(
                best.begin(), best.end(), [&](const CoarseTrack* existing) {
                    return better(&candidate, existing);
                });
            best.insert(insertion, &candidate);
            if (best.size() > quota) best.pop_back();
        }
        for (const CoarseTrack* candidate : best)
        {
            if (shortlist.size() >= MaximumSmartTrackShortlistSize) break;
            shortlist.push_back(candidate);
        }
    };

    // Acoustic/quality winners form the core, while explicit quotas prevent
    // frequently excellent transitions from starving fresh pairings or
    // underexplored tracks. Remaining slots follow the already-shuffled
    // permutation, preserving seeded/checkpointed random order without an
    // additional unbounded RNG pass.
    appendBest(16u, [](const CoarseTrack& candidate) {
        return candidate.combinedScore;
    });
    appendBest(8u, [](const CoarseTrack& candidate) {
        return candidate.transitionDiversity;
    });
    appendBest(4u, [](const CoarseTrack& candidate) {
        return candidate.exploration;
    });
    for (const CoarseTrack& candidate : candidatePool)
    {
        if (shortlist.size() >= MaximumSmartTrackShortlistSize) break;
        if (!alreadySelected(candidate)) shortlist.push_back(&candidate);
    }
    plist.lastSmartShortlistSize = shortlist.size();

    if (shortlist.empty())
    {
        const int position = fallbackPosition >= 0
            ? fallbackPosition
            : firstCandidatePosition;
        std::swap(
            plist.randomIndices[firstCandidatePosition],
            plist.randomIndices[position]);
        return plist.randomIndices[firstCandidatePosition];
    }

    std::vector<RankedTrack> ranked;
    ranked.reserve(shortlist.size());
    for (const CoarseTrack* candidate : shortlist)
    {
        selection.listeningFatigue = audioManager.GetListeningFatigue(
            plist.tracks[candidate->trackIndex]);
        ++plist.lastSmartFineEvaluationCount;
        MusicAnalysis::SegmentDecision decision = MusicAnalysis::ChooseSegment(
            *candidate->analysis, selection, m_rng, outgoing);
        decision.transitionDiversityScore = static_cast<float>(
            audioManager.GetTransitionDiversityScore(
                plist.tracks[plist.currentIndex],
                plist.tracks[candidate->trackIndex],
                selection.selectionEpochSeconds));
        decision.transitionDiversityScore = UnitScore(
            decision.transitionDiversityScore, 1.0f);
        decision.score = UnitScore(
            0.80f * decision.score +
                0.20f * decision.transitionDiversityScore);
        if (decision.transitionDiversityScore >= 0.70f)
        {
            decision.reasons.push_back(
                MusicAnalysis::SegmentReason::FreshTransitionPair);
        }
        if (decision.durationSeconds > 0.0f)
        {
            ranked.push_back({
                candidate->permutationPosition,
                candidate->trackIndex,
                std::move(decision)});
        }
    }

    if (ranked.empty())
    {
        std::swap(
            plist.randomIndices[firstCandidatePosition],
            plist.randomIndices[shortlist.front()->permutationPosition]);
        return plist.randomIndices[firstCandidatePosition];
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        if (left.decision.score != right.decision.score)
            return left.decision.score > right.decision.score;
        return left.trackIndex < right.trackIndex;
    });
    if (ranked.size() > 10u) ranked.resize(10u);

    const float bestScore = ranked.front().decision.score;
    std::vector<double> weights;
    weights.reserve(ranked.size());
    for (const RankedTrack& candidate : ranked)
        weights.push_back(std::exp(
            static_cast<double>(candidate.decision.score - bestScore) / 0.2));
    std::discrete_distribution<std::size_t> distribution(
        weights.begin(), weights.end());
    RankedTrack chosen = std::move(ranked[distribution(m_rng)]);

    std::swap(
        plist.randomIndices[firstCandidatePosition],
        plist.randomIndices[chosen.permutationPosition]);
    selectedDecision = std::move(chosen.decision);
    return chosen.trackIndex;
}

void PlaylistManager::FinishCrossfade(Playlist& plist)
{
    const bool incomingSurvived = IsOwnedChannelPlaying(
        plist.nextChannel, plist.expectedNextSound);
    if (incomingSurvived && plist.currentIndex >= 0 && plist.nextIndex >= 0 &&
        plist.currentIndex < static_cast<int>(plist.tracks.size()) &&
        plist.nextIndex < static_cast<int>(plist.tracks.size()))
    {
        AudioManager::GetInstance().RecordMusicTransition(
            plist.tracks[plist.currentIndex],
            plist.tracks[plist.nextIndex],
            CurrentEpochSeconds());
    }
    StopOwnedChannelImmediately(plist.currentChannel, plist.expectedCurrentSound);
    plist.currentChannel = nullptr;
    plist.expectedCurrentSound = nullptr;

    plist.currentChannel = plist.nextChannel;
    plist.expectedCurrentSound = plist.expectedNextSound;
    plist.nextChannel    = nullptr;
    plist.expectedNextSound = nullptr;
    if (plist.nextIndex >= 0) plist.currentIndex = plist.nextIndex;
    plist.currentSegment = plist.nextSegment;
    plist.nextSegment = {};
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
        else
        {
            // The playlist owned the channel envelope for the whole overlap.
            // Hand it back to the loudness controller with a gentle reconcile
            // so an analysis result cannot cause a post-crossfade jump.
            AudioManager::GetInstance().ReconcileMusicChannelVolume(
                plist.currentChannel);
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
    if (!AreSegmentDurationsValid(plist.options))
    {
        spdlog::error(
            "Playlist '{}' has invalid segment duration settings.",
            playlistName);
        return;
    }

    if (plist.isPlaying)
    {
        Stop(playlistName);
    }

    plist.currentSegment = {};
    plist.nextSegment = {};

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
