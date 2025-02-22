// tsm_playlist_manager.h
#pragma once

#include <string>
#include <vector>
#include <random>
#include <fmod.hpp>

namespace TSM
{

struct PlaylistOptions
{
    bool randomOrder       = false;   
    bool randomSegment     = false;  
    float segmentDuration  = 30.0f; 
    bool loopPlaylist      = false;  
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
    void AddToPlaylist(const std::string& playlistName, const std::string& soundName);
    void RemoveFromPlaylist(const std::string& playlistName, const std::string& soundName);

    void Play(const std::string& playlistName, const PlaylistOptions& options);
    void Stop(const std::string& playlistName);

    void Update(float deltaTime);

    std::string GetCurrentTrackName() const;
    std::string GetTrackName(int index) const;
    std::string GetCurrentTrackDuration() const;
    std::string GetTrackDuration(int index) const;

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
        FMOD::Channel* nextChannel    = nullptr;

        float crossfadeDuration = 10.0f;
        float crossfadeTimer    = 0.0f;
        bool  isCrossfading     = false;

        float oldChannelVolume  = 1.0f;
        float nextTargetVolume  = 1.0f;

        float segmentTimer      = 0.0f;
        float segmentMaxDuration= 0.0f;
        bool  segmentModeActive = false;
        float chosenStartTime   = 0.0f;
    };

    Playlist* m_currentPlaylist = nullptr;
    std::string m_activePlaylistName;

    const Playlist* GetActivePlaylist() const;
    Playlist* GetActivePlaylist();

    void StartNextTrack(Playlist& plist);
    void StartTrackAtIndex(Playlist& plist, int index);
    void PrepareRandomOrder(Playlist& plist);
    void FinishCrossfade(Playlist& plist);

    std::vector<Playlist> m_playlists;
    std::mt19937 m_rng;

public:
    const Playlist* GetPlaylistByName(const std::string& name) const;

    void MoveTrackUp(const std::string& playlistName, int index);
    void MoveTrackDown(const std::string& playlistName, int index);

    void PlayFromIndex(const std::string& playlistName, int index);
};

} // namespace TSM
