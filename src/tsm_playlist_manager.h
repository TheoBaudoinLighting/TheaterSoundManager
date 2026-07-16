#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <random>
#include <fmod.hpp>
#include <map>
#include <functional>
#include <limits>
#include "tsm_transition_logic.h"

namespace TSM
{

struct PlaylistOptions
{
    static constexpr float DefaultSegmentDuration = 150.0f;

    bool randomOrder = false;   
    bool randomSegment = false;  
    float segmentDuration = DefaultSegmentDuration;
    bool loopPlaylist = false;  
};

// Durable, implementation-independent snapshot used to resume music after an
// orderly restart or an unexpected power loss. Channel pointers are
// intentionally excluded: they are only valid for the current FMOD session.
struct PlaybackState
{
    bool isPlaying = false;
    std::string playlistName;
    std::string trackId;
    int trackIndex = -1;
    std::uint32_t positionMs = 0;
    PlaylistOptions options;
    float crossfadeDuration = 10.0f;
    std::vector<int> randomPermutation;
    int randomPermutationIndex = -1;
    bool segmentActive = false;
    std::uint32_t segmentStartMs = 0;
    std::uint32_t segmentElapsedMs = 0;
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
    bool PlayLibrary(const PlaylistOptions& options, float crossfadeDuration);
    void Stop(const std::string& playlistName);
    void StopLibrary();
    void SkipLibrary();
    bool IsLibraryPlaying() const;
    std::vector<std::string> GetLibraryMusicIds() const;
    const std::vector<std::string>& GetLibraryPlaybackSnapshot() const;
    void ConfigureLibraryPlayback(
        const PlaylistOptions& options, float crossfadeDuration);
    PlaylistOptions GetLibraryOptions() const;
    float GetLibraryCrossfadeDuration() const;
    PlaybackState CapturePlaybackState() const;
    bool ResumePlaybackState(const PlaybackState& state, std::string& errorMessage);
    void AbortImmediately();

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
    float GetSegmentDuration() const;
    float GetSegmentRemainingTime() const;

    void SetCrossfadeDuration(float duration);

    FMOD::Channel* GetCurrentChannel() const;
    bool IsInCrossfade() const;
    float GetCrossfadeProgress() const;
    FMOD::Channel* GetNextChannel() const;
    std::string GetNextTrackName() const;
    float GetSecondsUntilTransition() const;
    const char* GetLastTransitionReason() const;
    
    void SkipToNextTrack(const std::string& playlistName);

private:
    PlaylistManager();
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
        int nextIndex = -1;
        std::vector<int> randomIndices;
        int randomIndexPos = 0;

        FMOD::Channel* currentChannel = nullptr;
        FMOD::Channel* nextChannel = nullptr;
        FMOD::Sound* expectedCurrentSound = nullptr;
        FMOD::Sound* expectedNextSound = nullptr;

        float crossfadeDuration = 10.0f;
        float activeCrossfadeDuration = 10.0f;
        float crossfadeTimer = 0.0f;
        bool isCrossfading = false;

        float oldChannelVolume = 1.0f;
        float nextTargetVolume = 1.0f;

        float segmentTimer = 0.0f;
        float segmentMaxDuration = 0.0f;
        bool segmentModeActive = false;
        float chosenStartTime = 0.0f;
        float secondsUntilTransition = (std::numeric_limits<float>::max)();
        TransitionLogic::Reason lastTransitionReason = TransitionLogic::Reason::None;
    };

    std::string m_activePlaylistName;
    Playlist m_libraryPlayback;


    void StartNextTrack(Playlist& plist, float transitionDuration = -1.0f,
                        TransitionLogic::Reason reason = TransitionLogic::Reason::None);
    bool StartPlayback(Playlist& plist, const PlaylistOptions& options);
    void StartTrackAtIndex(Playlist& plist, int index);
    void UpdatePlayback(Playlist& plist, float deltaTime);
    void PrepareRandomOrder(Playlist& plist);
    void FinishCrossfade(Playlist& plist);
    bool IsTrackEligibleForPlayback(const Playlist& plist, int index, float* lengthSeconds = nullptr) const;
    int FindNextEligibleIndex(const Playlist& plist, int currentIndex, bool allowWrap) const;
    bool HasNextTrack(const Playlist& plist) const;
    void FinishPlaylist(Playlist& plist);
    static bool IsOwnedChannel(FMOD::Channel* channel, FMOD::Sound* expectedSound);
    static bool IsOwnedChannelPlaying(FMOD::Channel* channel, FMOD::Sound* expectedSound);
    static void StopOwnedChannelWithFade(FMOD::Channel* channel, FMOD::Sound* expectedSound);
    static void StopOwnedChannelImmediately(FMOD::Channel* channel, FMOD::Sound* expectedSound);
    static void ClearLogicalPlaybackState(Playlist& plist);

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
