// tsm_playback.h

#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <random>
#include <fmod.hpp>
#include <SDL.h>
#include "tsm_lock_manager.h"
#include "tsm_channels.h"
#include "tsm_logger.h"

struct MusicTrack {
    std::string filepath;
    std::string filename;
    FMOD::Sound* sound;
    unsigned int lengthMs;
};

class PlaybackManager {
public:
    PlaybackManager(FMOD::System* system);
    ~PlaybackManager();

    bool playTrack(size_t index, bool fadeIn = true, unsigned int offsetMs = 0);

    bool stopPlayback();
    bool togglePause();

    void setVolume(float newVolume);
    float getVolume() const { return volume; }
    
    bool isPlaying() const { return channelManager.isPlaying(); }
    bool isActuallyPlaying() const;

    bool isPaused() const { return channelManager.isPaused(); }

    void setFadeDuration(int durationMs) { fadeDuration = durationMs; }
    int getFadeDuration() const { return fadeDuration; }

    void setSegmentDuration(int durationMs) { segmentDuration = durationMs; }
    int getSegmentDuration() const { return segmentDuration; }

    bool addTrack(const std::string& filepath, const std::string& filename, FMOD::Sound* sound, unsigned int lengthMs);
    bool removeTrack(size_t index);
    
    const std::vector<MusicTrack>& getTracks() const { return musicTracks; }
    std::vector<MusicTrack>& getTracksRef() { return musicTracks; }

    int getCurrentTrackIndex() const { return currentTrackIndex; }
    unsigned int getPosition() const;
    Uint32 getSegmentStartTime() const { return segmentStartTime; }

    void setRandomOrder(bool random) { randomOrder = random; }
    bool isRandomOrder() const { return randomOrder; }

    void setRandomSegment(bool random) { randomSegment = random; }
    bool isRandomSegment() const { return randomSegment; }

    FMOD::Channel* getCurrentChannel() const { 
        return channelManager.getCurrentChannel(); 
    }

    void update();

    FMOD::Channel* getAudibleMusicChannel(PlaybackManager& pb);

    size_t getLastTrackIndex() const { return lastTrackIndex; }
    unsigned int getLastSegmentOffset() const { return lastSegmentOffset; }

    bool playLastSegment();

    size_t getNextTrackIndex();
    ChannelManager& getNextChannelManager() { return nextChannelManager; }

private:
    void internalUpdateFade(); 
    void startFadeOut();

    struct ChannelInfo {
        bool isPlaying;
        unsigned int position;
        float volume;
    };
    ChannelInfo getChannelInfo() const;

    mutable std::mutex tracksMutex;
    mutable std::mutex fadeMutex;

    ChannelManager channelManager;
    ChannelManager nextChannelManager;

    FMOD::System* fmodSystem;
    std::vector<MusicTrack> musicTracks;
    int currentTrackIndex;    

    float volume;
    bool randomOrder;
    bool randomSegment;

    bool isFadingOut;
    bool isFadingIn;
    Uint32 fadeStartTime;
    Uint32 segmentStartTime;

    int fadeDuration;
    int segmentDuration; 

    size_t lastTrackIndex;
    unsigned int lastSegmentOffset;

    std::mt19937 rng;

    static constexpr const char* TRACKS_RESOURCE = "playback_tracks";
    static constexpr const char* FADE_RESOURCE = "playback_fade";
};
