// tsm_announcement.h
#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <fmod.hpp>
#include "tsm_playback.h"
#include "tsm_logger.h"

class AnnouncementManager {
public:
    struct Announcement {
        std::string filepath;
        std::string filename;
        FMOD::Sound* sound;
        int hour;
        int minute;
        bool isEnabled;
        float volume;
    };

    AnnouncementManager(FMOD::System* system);
    ~AnnouncementManager();

    AnnouncementManager(const AnnouncementManager&) = delete;
    AnnouncementManager& operator=(const AnnouncementManager&) = delete;

    void setMusicDuckFactor(float factor);
    float getMusicDuckFactor() const;
    void setAnnouncementVolumeMultiplier(float multiplier);
    float getAnnouncementVolumeMultiplier() const;
    bool isPlaying() const;

    void loadAnnouncement(const std::string& filepath);
    void checkAndPlayAnnouncements(PlaybackManager& playbackMgr);
    
    void testAnnouncement(size_t index, PlaybackManager& playbackMgr);

    size_t getAnnouncementCount() const;
    Announcement* getAnnouncement(size_t index);
    const Announcement* getAnnouncement(size_t index) const;
    void removeAnnouncement(size_t index);

    void forEachAnnouncement(std::function<void(Announcement&)> func);

private:
    void playAnnouncement(FMOD::Sound* annSound, float announcementVol, FMOD::Channel* currentMusicChannel, float currentMusicVolume);
    bool isChannelPlaying(FMOD::Channel* channel) const;
    void doVolumeFade(FMOD::Channel* channel, float startVolume, float endVolume, int durationMs);
    bool waitForChannelToFinish(FMOD::Channel* channel);

    mutable std::mutex announcementsMutex;
    std::mutex isPlayingMutex;
    std::vector<Announcement> announcements;
    FMOD::Channel* announcementChannel;
    FMOD::System* fmodSystem;
    
    float musicDuckFactor;
    float announcementVolumeMultiplier;
    bool isAnnouncementPlaying;
    float currentMusicOriginalVolume;

    static const int FADE_DURATION_MS = 1000;
    static const int FADE_STEPS = 50;

    static constexpr const char* ANNOUNCEMENT_RESOURCE = "announcements";
    static constexpr const char* PLAYING_RESOURCE = "announcement_playing";
};