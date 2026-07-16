// tsm_announcement_manager.h
#pragma once

#include <fmod.hpp>
#include <cstdint>
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

    FMOD::Channel* PlayAnnouncement(const std::string& announcementId, float volumeDuck, bool useSFXBefore = true, bool useSFXAfter = true);
    
    void ScheduleAnnouncement(int hour, int minute, const std::string& announcementId);
    void AddScheduledAnnouncement(int hour, int minute, const std::string& annID);
    
    bool LoadAnnouncement(const std::string& announcementId, const std::string& filePath);
    
    void Update(float deltaTime);
    void StopAnnouncement();
    
    struct ScheduledAnnouncement
    {
        std::uint64_t scheduleId = 0;
        int hour;
        int minute;
        std::string announcementId;
        bool triggered = false;
        int lastTriggeredYear = -1;
        int lastTriggeredDay = -1;
    };
    
    const std::vector<ScheduledAnnouncement>& GetScheduledAnnouncements() const { return m_scheduled; }
    void RemoveScheduledAnnouncement(size_t index);
    void UpdateScheduledAnnouncement(size_t index, int hour, int minute, const std::string& announcementId);
    bool RemoveScheduledAnnouncementById(std::uint64_t scheduleId);
    bool UpdateScheduledAnnouncementById(
        std::uint64_t scheduleId, int hour, int minute, const std::string& announcementId);
    void ResetTriggeredAnnouncements();
    void RestoreScheduledAnnouncement(const ScheduledAnnouncement& announcement);
    void SetNextScheduleIdAtLeast(std::uint64_t nextScheduleId);
    void ResetSession();
    
    bool IsAnnouncing() const { return m_isAnnouncing; }
    AnnouncementState GetAnnouncementState() const { return m_state; }
    std::string GetCurrentAnnouncementName() const { return m_currentAnnouncementName; }
    float GetAnnouncementProgress() const;
    std::string GetAnnouncementStateString() const;

private:
    AnnouncementManager() = default;
    ~AnnouncementManager() = default;
    AnnouncementManager(const AnnouncementManager&) = delete;
    AnnouncementManager& operator=(const AnnouncementManager&) = delete;

    AnnouncementState m_state = AnnouncementState::IDLE;

    float m_duckVolume       = 0.3f;
    float m_duckFadeDuration = 1.5f;  
    float m_duckTimer        = 0.0f;

    bool  m_useSFXBefore = true;
    bool  m_useSFXAfter  = true;
    FMOD::Channel* m_sfxChannel = nullptr;
    std::string    m_sfxName    = "sfx_shine";

    FMOD::Channel* m_currentAnnouncementChannel = nullptr;
    std::string    m_currentAnnouncementName;
    bool           m_isAnnouncing = false;

private:
    void CheckSchedules(float deltaTime);
    std::vector<ScheduledAnnouncement> m_scheduled;
    std::uint64_t m_nextScheduleId = 1;
};

} // namespace TSM
