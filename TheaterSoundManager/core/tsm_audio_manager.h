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
    static AudioManager& GetInstance() 
    {
        static AudioManager instance;
        return instance;
    }

    bool LoadSound(const std::string& soundName, const std::string& filePath, bool isStream = false);
    bool UnloadSound(const std::string& soundName);

    FMOD::Channel* PlaySound(const std::string& soundName, bool loop = false, float volume = 1.0f, float pitch = 1.0f);

    void StopSound(const std::string& soundName);
    void StopAllSounds();

    void SetVolume(const std::string& soundName, float volume);
    void SetPitch(const std::string& soundName, float pitch);
    
    void SetChannelVolume(FMOD::Channel* channel, float volume);
    void SetChannelPitch(FMOD::Channel* channel, float pitch);

    void Update(float deltaTime);

    FMOD::Channel* GetLastChannelOfSound(const std::string& soundName);

private:
    struct SoundData
    {
        FMOD::Sound* sound = nullptr;
        std::vector<FMOD::Channel*> channels;
        std::string filePath;
    };

    std::map<std::string, SoundData> m_sounds;

public:
    const std::map<std::string, SoundData>& GetAllSounds() const { return m_sounds; }

private:
    AudioManager() = default;
    ~AudioManager() = default;
    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;
};

} // namespace TSM
