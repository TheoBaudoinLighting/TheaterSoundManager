// tsm_announcement_manager.cpp

#include "tsm_announcement_manager.h"
#include "tsm_audio_manager.h"
#include "tsm_ui_manager.h"

#include <fmod_errors.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <ctime>

namespace TSM
{

bool AnnouncementManager::LoadAnnouncement(const std::string& announceName, const std::string& filePath)
{
    if (!AudioManager::GetInstance().LoadSound(announceName, filePath, true))
    {
        spdlog::error("Failed to load announcement: {}", announceName);
        return false;
    }

    AnnounceData data;
    data.sound = AudioManager::GetInstance().GetAllSounds().at(announceName).sound;
    m_announcements[announceName] = data;

    return true;
}

FMOD::Channel* AnnouncementManager::PlayAnnouncement(const std::string& announceName,
                                                       float volumeDuck,
                                                       bool useSFXBefore,
                                                       bool useSFXAfter)
{
    StopAnnouncement();

    auto it = m_announcements.find(announceName);
    if (it == m_announcements.end()) {
        spdlog::error("Announcement '{}' not loaded or not found.", announceName);
        return nullptr;
    }

    m_currentAnnouncementName = announceName;
    m_duckVolume              = volumeDuck;
    m_useSFXBefore            = useSFXBefore;
    m_useSFXAfter             = useSFXAfter;

    m_state        = AnnouncementState::DUCKING_IN;
    m_duckTimer    = 0.0f;
    m_isAnnouncing = true;

    UIManager::GetInstance().SetDuckFactor(1.0f);
    UIManager::GetInstance().ForceUpdateAllVolumes();

    spdlog::info("Starting announcement sequence. (duckVolume={}, sfxBefore={}, sfxAfter={})",
                  volumeDuck, useSFXBefore, useSFXAfter);

    return nullptr;
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
            UIManager::GetInstance().SetDuckFactor(newFactor);
            UIManager::GetInstance().ForceUpdateAllVolumes();

            if (t >= 1.0f)
            {
                if (m_useSFXBefore)
                {
                    m_sfxChannel = AudioManager::GetInstance().PlaySound(m_sfxName, false, 1.0f);
                    UIManager::GetInstance().ForceUpdateAllVolumes();

                    m_state = AnnouncementState::PLAYING_SFX_BEFORE;
                }
                else
                {
                    m_currentAnnouncementChannel = AudioManager::GetInstance().PlaySound(
                        m_currentAnnouncementName, false, 1.0f);
                    UIManager::GetInstance().ForceUpdateAllVolumes();

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
                        m_currentAnnouncementName, false, 1.0f);
                    UIManager::GetInstance().ForceUpdateAllVolumes();

                    m_state = AnnouncementState::PLAYING_ANNOUNCEMENT;
                }
            }
            else {
                m_currentAnnouncementChannel = AudioManager::GetInstance().PlaySound(
                    m_currentAnnouncementName, false, 1.0f);
                UIManager::GetInstance().ForceUpdateAllVolumes();

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
                        m_sfxChannel = AudioManager::GetInstance().PlaySound(m_sfxName, false, 1.0f);
                        UIManager::GetInstance().ForceUpdateAllVolumes();

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
                    m_sfxChannel = AudioManager::GetInstance().PlaySound(m_sfxName, false, 1.0f);
                    UIManager::GetInstance().ForceUpdateAllVolumes();

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
            UIManager::GetInstance().SetDuckFactor(newFactor);
            UIManager::GetInstance().ForceUpdateAllVolumes();

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

    UIManager::GetInstance().SetDuckFactor(1.0f);
    UIManager::GetInstance().ForceUpdateAllVolumes();

    m_state = AnnouncementState::IDLE;
    m_isAnnouncing = false;

    spdlog::info("Announcement stopped manually.");
}

void AnnouncementManager::AddScheduledAnnouncement(int hour, int minute, const std::string& annID)
{
    ScheduledAnnouncement sa;
    sa.hour = hour;
    sa.minute = minute;
    sa.announceID = annID;
    sa.triggered = false;
    m_scheduled.push_back(sa);
    spdlog::info("Scheduled announce '{}' at {:02d}:{:02d}", annID, hour, minute);
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
        if (!s.triggered)
        {
            if (s.hour == currentHour && s.minute == currentMinute)
            {
                spdlog::info("Auto-playing announcement '{}' scheduled at {:02d}:{:02d}",
                             s.announceID, s.hour, s.minute);

                PlayAnnouncement(s.announceID, 0.3f, false, false);
                s.triggered = true;
            }
        }
    }
}

} // namespace TSM
