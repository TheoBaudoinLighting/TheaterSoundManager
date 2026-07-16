// tsm_announcement_manager.cpp

#include "tsm_announcement_manager.h"
#include "tsm_audio_manager.h"
#include "tsm_mixer.h"

#include <fmod_errors.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <ctime>

namespace TSM
{

void AnnouncementManager::ScheduleAnnouncement(int hour, int minute, const std::string& announcementId)
{
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || announcementId.empty()) {
        spdlog::error("Invalid scheduled announcement '{}': {:02d}:{:02d}", announcementId, hour, minute);
        return;
    }
    
    ScheduledAnnouncement announcement;
    announcement.scheduleId = m_nextScheduleId++;
    announcement.hour = hour;
    announcement.minute = minute;
    announcement.announcementId = announcementId;
    announcement.triggered = false;
    
    m_scheduled.push_back(announcement);
    
    spdlog::info("Announcement '{}' scheduled at {:02d}:{:02d}", announcementId, hour, minute);
}

void AnnouncementManager::UpdateScheduledAnnouncement(size_t index, int hour, int minute, const std::string& announcementId)
{
    if (index >= m_scheduled.size())
    {
        spdlog::error("Invalid scheduled announcement index: {}", index);
        return;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || announcementId.empty())
    {
        spdlog::error("Invalid scheduled announcement '{}': {:02d}:{:02d}", announcementId, hour, minute);
        return;
    }

    m_scheduled[index].hour = hour;
    m_scheduled[index].minute = minute;
    m_scheduled[index].announcementId = announcementId;
    m_scheduled[index].triggered = false;
    m_scheduled[index].lastTriggeredYear = -1;
    m_scheduled[index].lastTriggeredDay = -1;

    spdlog::info("Updated scheduled announcement at index {} to {}:{} with ID '{}'", 
                 index, hour, minute, announcementId);
}

void AnnouncementManager::Update(float deltaTime)
{
    CheckSchedules(deltaTime);
    
    switch (m_state)
    {
        case AnnouncementState::IDLE:
        default:
            break;

        case AnnouncementState::DUCKING_IN:
        {
            m_duckTimer += deltaTime;
            float t = m_duckTimer / m_duckFadeDuration;
            if (t > 1.0f) t = 1.0f;

            float newFactor = 1.0f + (m_duckVolume - 1.0f) * t;
            MixerState::GetInstance().SetAnnouncementDuckFactor(newFactor);
            MixerState::GetInstance().ApplyAllVolumes();

            if (t >= 1.0f)
            {
                if (m_useSFXBefore)
                {
                    m_sfxChannel = AudioManager::GetInstance().PlaySound(m_sfxName);
                    
                    MixerState::GetInstance().ApplyAllVolumes();

                    m_state = AnnouncementState::PLAYING_SFX_BEFORE;
                }
                else
                {
                    m_currentAnnouncementChannel = AudioManager::GetInstance().PlaySound(
                        m_currentAnnouncementName);
                    
                    MixerState::GetInstance().ApplyAllVolumes();

                    m_state = AnnouncementState::PLAYING_ANNOUNCEMENT;
                }
            }
        }
        break;

        case AnnouncementState::PLAYING_SFX_BEFORE:
        {
            if (m_sfxChannel) {
                bool isPlaying = false;
                m_sfxChannel->isPlaying(&isPlaying);

                if (!isPlaying) {
                    m_sfxChannel = nullptr;
                    m_currentAnnouncementChannel = AudioManager::GetInstance().PlaySound(
                        m_currentAnnouncementName);
                    
                    MixerState::GetInstance().ApplyAllVolumes();

                    m_state = AnnouncementState::PLAYING_ANNOUNCEMENT;
                }
            }
            else {
                m_currentAnnouncementChannel = AudioManager::GetInstance().PlaySound(
                    m_currentAnnouncementName);
                
                MixerState::GetInstance().ApplyAllVolumes();

                m_state = AnnouncementState::PLAYING_ANNOUNCEMENT;
            }
        }
        break;

        case AnnouncementState::PLAYING_ANNOUNCEMENT:
        {
            if (m_currentAnnouncementChannel) {
                bool isPlaying = false;
                m_currentAnnouncementChannel->isPlaying(&isPlaying);
                if (!isPlaying) {
                    m_currentAnnouncementChannel = nullptr;
                    if (m_useSFXAfter) {
                        m_sfxChannel = AudioManager::GetInstance().PlaySound(m_sfxName);
                        
                        MixerState::GetInstance().ApplyAllVolumes();

                        m_state = AnnouncementState::PLAYING_SFX_AFTER;
                    }
                    else {
                        m_duckTimer = 0.0f;
                        m_state = AnnouncementState::DUCKING_OUT;
                    }
                }
            }
            else {
                if (m_useSFXAfter) {
                    m_sfxChannel = AudioManager::GetInstance().PlaySound(m_sfxName);
                    
                    MixerState::GetInstance().ApplyAllVolumes();

                    m_state = AnnouncementState::PLAYING_SFX_AFTER;
                }
                else {
                    m_duckTimer = 0.0f;
                    m_state = AnnouncementState::DUCKING_OUT;
                }
            }
        }
        break;

        case AnnouncementState::PLAYING_SFX_AFTER:
        {
            if (m_sfxChannel) {
                bool isPlaying = false;
                m_sfxChannel->isPlaying(&isPlaying);
                if (!isPlaying) {
                    m_sfxChannel = nullptr;
                    m_duckTimer = 0.0f;
                    m_state = AnnouncementState::DUCKING_OUT;
                }
            }
            else {
                m_duckTimer = 0.0f;
                m_state = AnnouncementState::DUCKING_OUT;
            }
        }
        break;

        case AnnouncementState::DUCKING_OUT:
        {
            m_duckTimer += deltaTime;
            float t = m_duckTimer / m_duckFadeDuration;
            if (t > 1.0f) t = 1.0f;

            float newFactor = m_duckVolume + (1.0f - m_duckVolume) * t;
            MixerState::GetInstance().SetAnnouncementDuckFactor(newFactor);
            MixerState::GetInstance().ApplyAllVolumes();

            if (t >= 1.0f) {
                m_state = AnnouncementState::IDLE;
                m_isAnnouncing = false;
                spdlog::info("Announcement sequence finished.");
            }
        }
        break;
    }
}

void AnnouncementManager::StopAnnouncement()
{
    if (m_currentAnnouncementChannel) {
        bool isPlaying = false;
        m_currentAnnouncementChannel->isPlaying(&isPlaying);
        if (isPlaying) {
            m_currentAnnouncementChannel->stop();
        }
        m_currentAnnouncementChannel = nullptr;
    }

    if (m_sfxChannel) {
        bool isPlaying = false;
        m_sfxChannel->isPlaying(&isPlaying);
        if (isPlaying) {
            m_sfxChannel->stop();
        }
        m_sfxChannel = nullptr;
    }

    MixerState::GetInstance().SetAnnouncementDuckFactor(1.0f);
    MixerState::GetInstance().ApplyAllVolumes();

    m_state = AnnouncementState::IDLE;
    m_isAnnouncing = false;
    m_currentAnnouncementName.clear();
    m_duckTimer = 0.0f;

    spdlog::info("Announcement stopped manually.");
}

FMOD::Channel* AnnouncementManager::PlayAnnouncement(const std::string& announcementId, float volumeDuck, bool useSFXBefore, bool useSFXAfter)
{
    StopAnnouncement();

    FMOD::Sound* sound = AudioManager::GetInstance().GetSound(announcementId);
    if (!sound) {
        spdlog::error("Announcement '{}' not loaded or not found.", announcementId);
        return nullptr;
    }

    m_currentAnnouncementName = announcementId;
    m_duckVolume              = volumeDuck;
    m_useSFXBefore            = useSFXBefore;
    m_useSFXAfter             = useSFXAfter;

    m_state        = AnnouncementState::DUCKING_IN;
    m_duckTimer    = 0.0f;
    m_isAnnouncing = true;

    MixerState::GetInstance().SetAnnouncementDuckFactor(1.0f);
    MixerState::GetInstance().ApplyAllVolumes();

    spdlog::info("Starting announcement sequence '{}'. (duckVolume={}, sfxBefore={}, sfxAfter={})",
                 announcementId, volumeDuck, useSFXBefore, useSFXAfter);

    return nullptr;
}

void AnnouncementManager::AddScheduledAnnouncement(int hour, int minute, const std::string& annID)
{
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || annID.empty()) {
        spdlog::error("Invalid scheduled announcement '{}': {:02d}:{:02d}", annID, hour, minute);
        return;
    }
    
    ScheduledAnnouncement announcement;
    announcement.scheduleId = m_nextScheduleId++;
    announcement.hour = hour;
    announcement.minute = minute;
    announcement.announcementId = annID;
    announcement.triggered = false;
    
    m_scheduled.push_back(announcement);
    
    spdlog::info("Announcement '{}' scheduled at {:02d}:{:02d}", annID, hour, minute);
}

void AnnouncementManager::RemoveScheduledAnnouncement(size_t index)
{
    if (index < m_scheduled.size()) {
        auto& ann = m_scheduled[index];
        spdlog::info("Removed scheduled announcement '{}' at {:02d}:{:02d}", 
                     ann.announcementId, ann.hour, ann.minute);
        m_scheduled.erase(m_scheduled.begin() + index);
    }
}

bool AnnouncementManager::RemoveScheduledAnnouncementById(std::uint64_t scheduleId)
{
    const auto it = std::find_if(
        m_scheduled.begin(), m_scheduled.end(),
        [scheduleId](const ScheduledAnnouncement& announcement) {
            return announcement.scheduleId == scheduleId;
        });
    if (it == m_scheduled.end()) return false;
    m_scheduled.erase(it);
    return true;
}

bool AnnouncementManager::UpdateScheduledAnnouncementById(
    std::uint64_t scheduleId, int hour, int minute, const std::string& announcementId)
{
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || announcementId.empty())
        return false;

    const auto it = std::find_if(
        m_scheduled.begin(), m_scheduled.end(),
        [scheduleId](const ScheduledAnnouncement& announcement) {
            return announcement.scheduleId == scheduleId;
        });
    if (it == m_scheduled.end()) return false;

    it->hour = hour;
    it->minute = minute;
    it->announcementId = announcementId;
    it->triggered = false;
    it->lastTriggeredYear = -1;
    it->lastTriggeredDay = -1;
    return true;
}

void AnnouncementManager::ResetTriggeredAnnouncements()
{
    for (auto& ann : m_scheduled) {
        ann.triggered = false;
        ann.lastTriggeredYear = -1;
        ann.lastTriggeredDay = -1;
    }
    spdlog::info("Reset all triggered flags for scheduled announcements");
}

void AnnouncementManager::RestoreScheduledAnnouncement(
    const ScheduledAnnouncement& announcement)
{
    if (announcement.scheduleId == 0 || announcement.hour < 0 ||
        announcement.hour > 23 || announcement.minute < 0 ||
        announcement.minute > 59 || announcement.announcementId.empty())
    {
        return;
    }
    m_scheduled.push_back(announcement);
    m_nextScheduleId = std::max(m_nextScheduleId, announcement.scheduleId + 1);
}

void AnnouncementManager::SetNextScheduleIdAtLeast(std::uint64_t nextScheduleId)
{
    m_nextScheduleId = std::max(m_nextScheduleId, nextScheduleId);
}

void AnnouncementManager::ResetSession()
{
    StopAnnouncement();
    m_scheduled.clear();
    m_nextScheduleId = 1;
    m_currentAnnouncementName.clear();
    m_duckTimer = 0.0f;
    m_duckVolume = 0.3f;
    m_useSFXBefore = true;
    m_useSFXAfter = true;
}

float AnnouncementManager::GetAnnouncementProgress() const
{
    if (!m_isAnnouncing || !m_currentAnnouncementChannel) {
        return 0.0f;
    }
    
    bool isPlaying = false;
    m_currentAnnouncementChannel->isPlaying(&isPlaying);
    
    if (!isPlaying) {
        return 0.0f;
    }
    
    FMOD::Sound* sound = nullptr;
    if (m_currentAnnouncementChannel->getCurrentSound(&sound) != FMOD_OK || !sound) {
        return 0.0f;
    }
    
    unsigned int positionMs = 0;
    unsigned int lengthMs = 0;
    
    m_currentAnnouncementChannel->getPosition(&positionMs, FMOD_TIMEUNIT_MS);
    sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
    
    if (lengthMs == 0) {
        return 0.0f;
    }
    
    return static_cast<float>(positionMs) / static_cast<float>(lengthMs);
}

std::string AnnouncementManager::GetAnnouncementStateString() const
{
    switch (m_state) {
        case AnnouncementState::IDLE:
            return "Idle";
        case AnnouncementState::DUCKING_IN:
            return "Ducking In";
        case AnnouncementState::PLAYING_SFX_BEFORE:
            return "Playing SFX Before";
        case AnnouncementState::PLAYING_ANNOUNCEMENT:
            return "Playing Announcement";
        case AnnouncementState::PLAYING_SFX_AFTER:
            return "Playing SFX After";
        case AnnouncementState::DUCKING_OUT:
            return "Ducking Out";
        default:
            return "Unknown";
    }
}

void AnnouncementManager::CheckSchedules(float /*deltaTime*/)
{
    std::time_t t = std::time(nullptr);
    std::tm localTm;
#ifdef _WIN32
    localtime_s(&localTm, &t);
#else
    localtime_r(&t, &localTm);
#endif
    int currentHour   = localTm.tm_hour;
    int currentMinute = localTm.tm_min;

    for (auto& s : m_scheduled)
    {
        if (s.triggered &&
            (s.lastTriggeredYear != localTm.tm_year || s.lastTriggeredDay != localTm.tm_yday))
        {
            s.triggered = false;
        }
        if (!s.triggered)
        {
            if (s.hour == currentHour && s.minute == currentMinute)
            {
                FMOD::Sound* sound = AudioManager::GetInstance().GetSound(s.announcementId);
                if (!sound) {
                    spdlog::error("Impossible to play scheduled announcement '{}' because it is not loaded or not found.", s.announcementId);
                    s.triggered = true;
                    s.lastTriggeredYear = localTm.tm_year;
                    s.lastTriggeredDay = localTm.tm_yday;
                    continue;
                }
                
                spdlog::info("Auto-playing announcement '{}' scheduled at {:02d}:{:02d}",
                             s.announcementId, s.hour, s.minute);

                PlayAnnouncement(s.announcementId, 0.05f, true, true);
                s.triggered = true;
                s.lastTriggeredYear = localTm.tm_year;
                s.lastTriggeredDay = localTm.tm_yday;
            }
        }
    }
}

bool AnnouncementManager::LoadAnnouncement(const std::string& announcementId, const std::string& filePath)
{
    if (m_isAnnouncing && m_currentAnnouncementName == announcementId)
    {
        spdlog::error("Stop announcement '{}' before replacing its asset.", announcementId);
        return false;
    }
    return AudioManager::GetInstance().LoadAnnouncement(announcementId, filePath);
}

} // namespace TSM
