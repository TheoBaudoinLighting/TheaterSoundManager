// tsm_announcement_manager.h
#pragma once

#include <fmod.hpp>
#include <string>
#include <vector>
#include <map>

namespace TSM
{

enum class AnnouncementState {
    IDLE = 0,
    DUCKING_IN,
    PLAYING_SFX_BEFORE,
    PLAYING_ANNOUNCEMENT,
    PLAYING_SFX_AFTER,
    DUCKING_OUT
};

class AnnouncementManager
{
public:
    static AnnouncementManager& GetInstance()
    {
        static AnnouncementManager instance;
        return instance;
    }

    FMOD::Channel* PlayAnnouncement(const std::string& announceName, float volumeDuck, bool useSFXBefore = true, bool useSFXAfter = true);
    bool LoadAnnouncement(const std::string& announceName, const std::string& filePath);
    void Update(float deltaTime);
    void StopAnnouncement();

    void AddScheduledAnnouncement(int hour, int minute, const std::string& annID);

private:
    AnnouncementManager() = default;
    ~AnnouncementManager() = default;
    AnnouncementManager(const AnnouncementManager&) = delete;
    AnnouncementManager& operator=(const AnnouncementManager&) = delete;

    struct ScheduledAnnouncement
    {
        int hour;
        int minute;
        std::string announceID;
        bool triggered = false;
    };

    AnnouncementState m_state = AnnouncementState::IDLE;

    float m_duckVolume       = 0.3f;   // 0.3 => 30%
    float m_duckFadeDuration = 1.5f;   // 1.5 secondes
    float m_duckTimer        = 0.0f;   // compteur d'interpolation

    bool  m_useSFXBefore = true;
    bool  m_useSFXAfter  = true;
    FMOD::Channel* m_sfxChannel = nullptr;
    std::string    m_sfxName    = "sfx_shine";

    FMOD::Channel* m_currentAnnouncementChannel = nullptr;
    std::string    m_currentAnnouncementName;
    bool           m_isAnnouncing = false;

    struct AnnounceData
    {
        FMOD::Sound* sound = nullptr;
    };
    std::map<std::string, AnnounceData> m_announcements;

private:
    void CheckSchedules(float deltaTime);
    std::vector<ScheduledAnnouncement> m_scheduled;
};

} // namespace TSM
