#pragma once

#include <string>
#include <vector>
#include <random>
#include <fmod.hpp>
#include <map>
#include <functional>

namespace TSM
{

struct PlaylistOptions
{
    bool randomOrder = false;   
    bool randomSegment = false;  
    float segmentDuration = 30.0f; 
    bool loopPlaylist = false;  
};

class PlaylistManager
{
public:
    static PlaylistManager& GetInstance()
    {
        static PlaylistManager instance;
        return instance;
    }

    void CreatePlaylist(const std::string& playlistName);
    void DeletePlaylist(const std::string& playlistName);
    void RenamePlaylist(const std::string& oldName, const std::string& newName);
    void DuplicatePlaylist(const std::string& sourceName, const std::string& destName);
    
    void AddToPlaylist(const std::string& playlistName, const std::string& soundName);
    void RemoveFromPlaylist(const std::string& playlistName, const std::string& soundName);
    void RemoveFromPlaylistAtIndex(const std::string& playlistName, size_t index);
    void ClearPlaylist(const std::string& playlistName);
    
    bool ExportPlaylist(const std::string& playlistName, const std::string& filePath);
    bool ImportPlaylist(const std::string& filePath, const std::string& playlistName = "");

    void Play(const std::string& playlistName, const PlaylistOptions& options);
    void Stop(const std::string& playlistName);

    void Update(float deltaTime);

    std::string GetCurrentTrackName() const;
    std::string GetTrackName(int index) const;
    std::string GetCurrentTrackDuration() const;
    std::string GetTrackDuration(int index) const;
    
    std::vector<std::string> GetPlaylistNames() const;
    bool IsPlaylistPlaying(const std::string& playlistName) const;
    size_t GetPlaylistTrackCount(const std::string& playlistName) const;

    float GetTrackProgress() const;   
    float GetSegmentProgress() const; 

    void SetCrossfadeDuration(float duration);

    FMOD::Channel* GetCurrentChannel() const;
    bool IsInCrossfade() const;
    float GetCrossfadeProgress() const;
    FMOD::Channel* GetNextChannel() const;
    
    

private:
    PlaylistManager() : m_rng(std::random_device{}()) {}
    ~PlaylistManager() = default;
    PlaylistManager(const PlaylistManager&) = delete;
    PlaylistManager& operator=(const PlaylistManager&) = delete;

    struct Playlist
    {
        std::string name;
        std::vector<std::string> tracks;
        bool isPlaying = false;

        PlaylistOptions options;

        int currentIndex = -1;
        std::vector<int> randomIndices;
        int randomIndexPos = 0;

        FMOD::Channel* currentChannel = nullptr;
        FMOD::Channel* nextChannel = nullptr;

        float crossfadeDuration = 10.0f;
        float crossfadeTimer = 0.0f;
        bool isCrossfading = false;

        float oldChannelVolume = 1.0f;
        float nextTargetVolume = 1.0f;

        float segmentTimer = 0.0f;
        float segmentMaxDuration = 0.0f;
        bool segmentModeActive = false;
        float chosenStartTime = 0.0f;
    };

    Playlist* m_currentPlaylist = nullptr;
    std::string m_activePlaylistName;

    

    void StartNextTrack(Playlist& plist);
    void StartTrackAtIndex(Playlist& plist, int index);
    void PrepareRandomOrder(Playlist& plist);
    void FinishCrossfade(Playlist& plist);

    std::vector<Playlist> m_playlists;
    std::mt19937 m_rng;

public:
    void MoveTrackUp(const std::string& playlistName, int index);
    void MoveTrackDown(const std::string& playlistName, int index);
    void MoveTrackToPosition(const std::string& playlistName, int sourceIndex, int targetIndex);

    void PlayFromIndex(const std::string& playlistName, int index);
    
    bool SavePlaylistsToFile(const std::string& filePath);
    bool LoadPlaylistsFromFile(const std::string& filePath);
    
    using PlaylistChangeCallback = std::function<void()>;
    void RegisterPlaylistChangeCallback(PlaylistChangeCallback callback);

    Playlist* GetPlaylistByName(const std::string& name);
    const Playlist* GetPlaylistByName(const std::string& name) const;
    const std::vector<Playlist>& GetAllPlaylists() const { return m_playlists; }

    const Playlist* GetActivePlaylist() const;
    Playlist* GetActivePlaylist();
    
private:
    std::vector<PlaylistChangeCallback> m_changeCallbacks;
    void NotifyPlaylistChanged();
};

} // namespace TSM