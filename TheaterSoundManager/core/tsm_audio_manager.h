// tsm_audio_manager.h
#pragma once

#include <fmod.hpp>
#include <string>
#include <map>
#include <vector>

namespace TSM 
{

class AudioManager 
{
public:
    struct SoundData
    {
        FMOD::Sound* sound = nullptr;
        std::vector<FMOD::Channel*> channels;
        std::string filePath;
    };

    static AudioManager& GetInstance() 
    {
        static AudioManager instance;
        return instance;
    }

    bool LoadSound(const std::string& soundName, const std::string& filePath, bool isStream = false);
    bool UnloadSound(const std::string& soundName);
    bool LoadWeddingPhaseSound(int phase, const std::string& filePath);
    bool LoadWeddingEntranceSound(const std::string& filePath) { return LoadWeddingPhaseSound(1, filePath); }
    bool LoadWeddingCeremonySound(const std::string& filePath) { return LoadWeddingPhaseSound(2, filePath); }
    bool LoadWeddingExitSound(const std::string& filePath) { return LoadWeddingPhaseSound(3, filePath); }
    bool LoadAnnouncement(const std::string& announcementId, const std::string& filePath);
    FMOD::Channel* PlaySound(const std::string& soundName, bool loop = false, float volume = 1.0f, float pitch = 1.0f);
    FMOD::Channel* PlaySoundWithFadeIn(const std::string& soundName, bool loop = false, float volume = 1.0f, float pitch = 1.0f);
    void StopSound(const std::string& soundName);
    void StopSoundWithFadeOut(const std::string& soundName);
    void StopAllSounds();
    void StopAllSoundsWithFadeOut();
    FMOD::Sound* GetSound(const std::string& soundName);
    const std::map<std::string, SoundData>& GetAllSounds() const { return m_sounds; }
    void SetVolume(const std::string& soundName, float volume);
    void SetPitch(const std::string& soundName, float pitch);
    void SetChannelVolume(FMOD::Channel* channel, float volume);
    void SetChannelPitch(FMOD::Channel* channel, float pitch);
    void Update(float deltaTime);
    FMOD::Channel* GetLastChannelOfSound(const std::string& soundName);
private:
    std::map<std::string, SoundData> m_sounds;
    bool m_isFadingIn = false;
    bool m_isFadingOut = false;
    float m_fadeTimer = 0.0f;
    float m_fadeDuration = 1.5f;
    std::string m_fadingSoundName;
    float m_fadeTargetVolume = 1.0f;
public:
    AudioManager() = default;
    ~AudioManager() = default;

    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;
};

} // namespace TSM
