// tsm_channels.h

#pragma once

#include <mutex>
#include <fmod.hpp>
#include <SDL.h>
#include "tsm_logger.h"
#include "tsm_lock_manager.h"
class ChannelManager {
public:
    ChannelManager();
    ~ChannelManager();

    void setSystem(FMOD::System* system) { fmodSystem = system; }
    bool startPlayback(FMOD::Sound* sound, float initialVolume = 0.0f);
    bool stopPlayback();
    bool setVolume(float volume);
    bool isPaused() const;
    bool setPaused(bool pause);
    bool isPlaying() const;
    bool getPosition(unsigned int* position) const;
    
    static bool isChannelValid(FMOD::Channel* channel);

    ChannelManager(const ChannelManager& other);
    ChannelManager& operator=(const ChannelManager& other);

    bool getChannelState(bool* isPlaying, unsigned int* position = nullptr, float* volume = nullptr) const {
        TSM_LOCK(PLAYBACK, GLOBAL_PLAYBACK_RESOURCE);
        if (!currentChannel) {
            if (isPlaying) *isPlaying = false;
            if (position) *position = 0;
            if (volume) *volume = 0.0f;
            return false;
        }
        
        FMOD_RESULT result = FMOD_OK;
        if (isPlaying) {
            result = currentChannel->isPlaying(isPlaying);
            if (result != FMOD_OK) return false;
        }
        
        if (position) {
            result = currentChannel->getPosition(position, FMOD_TIMEUNIT_MS);
            if (result != FMOD_OK) return false;
        }
        
        if (volume) {
            result = currentChannel->getVolume(volume);
            if (result != FMOD_OK) return false;
        }
        
        return true;
    }

private:
    FMOD::System* fmodSystem;
    FMOD::Channel* currentChannel;

    static constexpr const char* GLOBAL_PLAYBACK_RESOURCE = "PlaybackResource";

public:
    FMOD::Channel* getCurrentChannel() const { 
        try {
            TSM_LOCK(PLAYBACK, GLOBAL_PLAYBACK_RESOURCE);
            return currentChannel;
        } catch (const std::exception& e) {
            spdlog::error("Lock acquisition failed in getCurrentChannel: {}", e.what());
            return nullptr;
        }
    }
};

inline ChannelManager::ChannelManager() 
    : fmodSystem(nullptr), currentChannel(nullptr) {}

inline ChannelManager::~ChannelManager() {
    TSM_LOCK(PLAYBACK, GLOBAL_PLAYBACK_RESOURCE);
    if (currentChannel) {
        currentChannel->stop();
        currentChannel = nullptr;
    }
}

inline ChannelManager::ChannelManager(const ChannelManager& other)
    : fmodSystem(other.fmodSystem), currentChannel(nullptr)
{
}

inline ChannelManager& ChannelManager::operator=(const ChannelManager& other) {
    if (this != &other) {
        TSM_LOCK(PLAYBACK, GLOBAL_PLAYBACK_RESOURCE);
        stopPlayback();
        fmodSystem = other.fmodSystem;
        currentChannel = nullptr;
    }
    return *this;
}

inline bool ChannelManager::isChannelValid(FMOD::Channel* channel) {
    if (!channel) return false;
    bool isPlaying = false;
    FMOD_RESULT result = channel->isPlaying(&isPlaying);
    return (result == FMOD_OK && isPlaying);
}

inline bool ChannelManager::startPlayback(FMOD::Sound* sound, float initialVolume) {
    if (!fmodSystem || !sound) return false;
    
    TSM_LOCK(PLAYBACK, GLOBAL_PLAYBACK_RESOURCE);

    if (currentChannel) {
        currentChannel->stop();
        currentChannel = nullptr;
    }
    
    FMOD_RESULT result = fmodSystem->playSound(sound, nullptr, true, &currentChannel);
    if (result != FMOD_OK || !currentChannel) {
        spdlog::error("[ChannelManager] Échec du démarrage de la lecture (FMOD error: {})", static_cast<int>(result));
        return false;
    }
    
    currentChannel->setVolume(initialVolume);
    currentChannel->setPaused(false);
    return true;
}


inline bool ChannelManager::stopPlayback() {
    TSM_LOCK(PLAYBACK, GLOBAL_PLAYBACK_RESOURCE);
    if (currentChannel) {
        FMOD_RESULT result = currentChannel->stop();
        currentChannel = nullptr;
        return (result == FMOD_OK);
    }
    return true; 
}

inline bool ChannelManager::setVolume(float volume) {
    TSM_LOCK(PLAYBACK, GLOBAL_PLAYBACK_RESOURCE);
    if (currentChannel) {
        return (currentChannel->setVolume(volume) == FMOD_OK);
    }
    return false;
}

inline bool ChannelManager::isPaused() const {
    TSM_LOCK(PLAYBACK, GLOBAL_PLAYBACK_RESOURCE);
    if (currentChannel) {
        bool paused;
        if (currentChannel->getPaused(&paused) == FMOD_OK) {
            return paused;
        }
    }
    return true;
}

inline bool ChannelManager::setPaused(bool pause) {
    TSM_LOCK(PLAYBACK, GLOBAL_PLAYBACK_RESOURCE);
    if (currentChannel) {
        return (currentChannel->setPaused(pause) == FMOD_OK);
    }
    return false;
}

inline bool ChannelManager::isPlaying() const {
    TSM_LOCK(PLAYBACK, GLOBAL_PLAYBACK_RESOURCE);
    if (currentChannel) {
        bool playing;
        if (currentChannel->isPlaying(&playing) == FMOD_OK) {
            return playing;
        }
    }
    return false;
}

inline bool ChannelManager::getPosition(unsigned int* position) const {
    TSM_LOCK(PLAYBACK, GLOBAL_PLAYBACK_RESOURCE);
    if (currentChannel) {
        return (currentChannel->getPosition(position, FMOD_TIMEUNIT_MS) == FMOD_OK);
    }
    return false;
}