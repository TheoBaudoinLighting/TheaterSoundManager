#pragma once

#include <string>
#include <vector>
#include <fmod.hpp>
#include <chrono>
#include <ctime>
#include <iostream>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

struct Announcement {
    std::string filepath;
    std::string filename;
    FMOD::Sound* sound;
    int hour;      
    int minute;     
    bool isEnabled; 
    float volume;   
};

struct AnnouncementCallbackData {
    FMOD::Channel* currentMusicChannel;
    float originalVolume;
    bool* isPlaying;
    FMOD::System* system;
    FMOD::Channel* announcementChannel;
};

std::vector<Announcement> announcements;
FMOD::Channel* announcementChannel = nullptr;
float musicDuckingVolume = 0.1f;
bool isAnnouncementPlaying = false;
float currentMusicOriginalVolume = 1.0f; 

const int FADE_DURATION_MS = 1000;
const int FADE_STEPS = 50;

bool isChannelPlaying(FMOD::Channel* channel) {
    if (!channel) return false;
    bool isPlaying = false;
    channel->isPlaying(&isPlaying);
    return isPlaying;
}

void doVolumeFade(FMOD::Channel* channel, float startVolume, float endVolume, int durationMs) {
    if (!channel) return;
    
    bool isPlaying = false;
    channel->isPlaying(&isPlaying);
    if (!isPlaying) return;

    int stepDuration = durationMs / FADE_STEPS;
    float volumeDelta = (endVolume - startVolume) / FADE_STEPS;

    for (int i = 0; i < FADE_STEPS; i++) {
        channel->isPlaying(&isPlaying);
        if (!isPlaying) return;
        
        float currentVolume = startVolume + (volumeDelta * i);
        channel->setVolume(currentVolume);
        std::this_thread::sleep_for(std::chrono::milliseconds(stepDuration));
    }

    channel->isPlaying(&isPlaying);
    if (isPlaying) {
        channel->setVolume(endVolume);
    }
}

bool waitForChannelToFinish(FMOD::Channel* channel) {
    while (isChannelPlaying(channel)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

FMOD_RESULT F_CALLBACK AnnouncementEndCallback(FMOD_CHANNELCONTROL* channelControl, FMOD_CHANNELCONTROL_TYPE controlType,
    FMOD_CHANNELCONTROL_CALLBACK_TYPE callbackType, void* commandData1, void* commandData2) 
{
    if (callbackType == FMOD_CHANNELCONTROL_CALLBACK_END) {
        void* userData = nullptr;
        FMOD::Channel* channel = (FMOD::Channel*)channelControl;
        channel->getUserData(&userData);
        
        AnnouncementCallbackData* data = static_cast<AnnouncementCallbackData*>(userData);
        if (data && data->currentMusicChannel) {
            std::thread fadeThread([data]() {
                float startVolume;
                data->currentMusicChannel->getVolume(&startVolume);
                
                doVolumeFade(data->announcementChannel, 
                            startVolume, 
                            0.0f, 
                            FADE_DURATION_MS);
                
                *(data->isPlaying) = false;
                delete data;
            });
            fadeThread.detach();
        } else if (data) {
            *(data->isPlaying) = false;
            delete data;
        }
        channel->setUserData(nullptr);
    }
    return FMOD_OK;
}


void LoadAnnouncement(const std::string& filepath, FMOD::System* fmodSystem) {
    std::string filename = fs::path(filepath).filename().string();
    
    FMOD::Sound* sound = nullptr;
    FMOD_RESULT result = fmodSystem->createSound(filepath.c_str(), FMOD_DEFAULT, nullptr, &sound);
    if (result == FMOD_OK) {
        Announcement announcement = {
            filepath,
            filename,
            sound,
            12,         
            0,         
            true,     
            1.0f      
        };
        announcements.push_back(announcement);
    } else {
        std::cerr << "Erreur FMOD::createSound pour l'annonce: " << result << std::endl;
    }
}

void PlayAnnouncement(FMOD::System* fmodSystem, FMOD::Sound* announcementSound, float announcementVolume, 
    FMOD::Channel* currentMusicChannel, float duckingVolume) {
    
    if (!fmodSystem || !announcementSound) {
        std::cerr << "Paramètres invalides pour PlayAnnouncement" << std::endl;
        return;
    }

    if (currentMusicChannel) {
        bool isPlaying = false;
        FMOD_RESULT result = currentMusicChannel->isPlaying(&isPlaying);
        
        if (result == FMOD_OK && isPlaying) {
            currentMusicChannel->getVolume(&currentMusicOriginalVolume);
            
            std::thread([currentMusicChannel, duckingVolume]() {
                float startVolume;
                currentMusicChannel->getVolume(&startVolume);
                doVolumeFade(currentMusicChannel, 
                            startVolume,
                            currentMusicOriginalVolume * duckingVolume, 
                            FADE_DURATION_MS);

                bool stillPlaying = false;
                currentMusicChannel->isPlaying(&stillPlaying);
                if (stillPlaying) {
                    currentMusicChannel->setVolume(currentMusicOriginalVolume * duckingVolume);
                }
            }).detach();
        }
    }

    FMOD::Channel* channel = nullptr;
    FMOD_RESULT result = fmodSystem->playSound(announcementSound, nullptr, false, &channel);
    if (result == FMOD_OK && channel) {
        channel->setVolume(announcementVolume);
        
        AnnouncementCallbackData* callbackData = new AnnouncementCallbackData{
            currentMusicChannel,
            currentMusicOriginalVolume,
            &isAnnouncementPlaying,
            fmodSystem,
            channel
        };
        
        channel->setUserData(callbackData);
        channel->setCallback(AnnouncementEndCallback);
        isAnnouncementPlaying = true;
        announcementChannel = channel;
    }
}





void CheckAndPlayAnnouncements(FMOD::System* fmodSystem, FMOD::Channel* currentMusicChannel) {
    auto now = std::chrono::system_clock::now();
    std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime;
    localtime_s(&localTime, &currentTime);
    
    for (const auto& announcement : announcements) {
        if (announcement.isEnabled &&
            localTime.tm_hour == announcement.hour &&
            localTime.tm_min == announcement.minute) {
            
            bool isPlaying = false;
            if (announcementChannel) {
                announcementChannel->isPlaying(&isPlaying);
            }
            
            if (!isPlaying && !isAnnouncementPlaying) {
                PlayAnnouncement(fmodSystem, announcement.sound, announcement.volume, 
                    currentMusicChannel, musicDuckingVolume);
            }
        }
    }
}