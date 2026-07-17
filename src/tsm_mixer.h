#pragma once

namespace TSM
{

class MixerState
{
public:
    static MixerState& GetInstance();

    float GetMasterVolume() const { return m_masterVolume; }
    float GetMusicVolume() const { return m_musicVolume; }
    float GetAnnouncementVolume() const { return m_announcementVolume; }
    float GetSfxVolume() const { return m_sfxVolume; }
    float GetDuckFactor() const { return m_userDuckFactor; }
    float GetAnnouncementDuckFactor() const { return m_announcementDuckFactor; }
    float GetEffectiveDuckFactor() const;

    void SetMasterVolume(float volume);
    void SetMusicVolume(float volume);
    void SetAnnouncementVolume(float volume);
    void SetSfxVolume(float volume);
    void SetDuckFactor(float factor);
    void SetAnnouncementDuckFactor(float factor);

    void ApplyAllVolumes() const;
    void Reset();

private:
    MixerState() = default;

    float m_masterVolume = 0.5f;
    float m_musicVolume = 0.5f;
    float m_announcementVolume = 3.0f;
    float m_sfxVolume = 3.0f;
    float m_userDuckFactor = 1.0f;
    float m_announcementDuckFactor = 1.0f;
};

} // namespace TSM
