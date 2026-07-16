#include "tsm_mixer.h"

#include "tsm_audio_manager.h"

#include <algorithm>

namespace TSM
{

MixerState& MixerState::GetInstance()
{
    static MixerState instance;
    return instance;
}

void MixerState::SetMasterVolume(float volume)
{
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
}

void MixerState::SetMusicVolume(float volume)
{
    m_musicVolume = std::clamp(volume, 0.0f, 1.0f);
}

void MixerState::SetAnnouncementVolume(float volume)
{
    m_announcementVolume = std::clamp(volume, 0.0f, 3.0f);
}

void MixerState::SetSfxVolume(float volume)
{
    m_sfxVolume = std::clamp(volume, 0.0f, 3.0f);
}

void MixerState::SetDuckFactor(float factor)
{
    m_userDuckFactor = std::clamp(factor, 0.0f, 1.0f);
}

void MixerState::SetAnnouncementDuckFactor(float factor)
{
    m_announcementDuckFactor = std::clamp(factor, 0.0f, 1.0f);
}

void MixerState::SetWeddingDuckFactor(float factor)
{
    m_weddingDuckFactor = std::clamp(factor, 0.0f, 1.0f);
}

float MixerState::GetEffectiveDuckFactor() const
{
    return m_userDuckFactor * m_announcementDuckFactor * m_weddingDuckFactor;
}

void MixerState::ApplyAllVolumes() const
{
    AudioManager::GetInstance().ApplyMixerVolumes(
        m_masterVolume,
        m_musicVolume,
        m_announcementVolume,
        m_sfxVolume,
        GetEffectiveDuckFactor());
}

void MixerState::Reset()
{
    m_masterVolume = 0.5f;
    m_musicVolume = 0.5f;
    m_announcementVolume = 3.0f;
    m_sfxVolume = 3.0f;
    m_userDuckFactor = 1.0f;
    m_announcementDuckFactor = 1.0f;
    m_weddingDuckFactor = 1.0f;
    ApplyAllVolumes();
}

} // namespace TSM
