// tsm_audio_manager.cpp

#include "tsm_audio_manager.h"
#include "tsm_fmod_wrapper.h"

#include <spdlog/spdlog.h>

namespace TSM 
{

bool AudioManager::LoadSound(const std::string& soundName, const std::string& filePath, bool isStream)
{
    if (m_sounds.find(soundName) != m_sounds.end())
    {
        spdlog::error("Sound already loaded: {}", soundName);
        return true;
    }

    FMOD_MODE mode = FMOD_DEFAULT;
    if (isStream)
    {
        mode |= FMOD_CREATESTREAM;
    }

    FMOD::Sound* newSound = nullptr;
    FMOD_RESULT result = FModWrapper::GetInstance().GetSystem()->createSound(filePath.c_str(), mode, nullptr, &newSound);
    if (result != FMOD_OK)
    {
        spdlog::error("FMOD createSound failed: {} for file: {}", FMOD_ErrorString(result), filePath);
        return false;
    }
    else
    {
        spdlog::info("FMOD createSound success: {} for file: {}", soundName, filePath);
    }
    
    SoundData data; 
    data.sound = newSound;
    m_sounds[soundName] = data;

    spdlog::info("Sound loaded successfully: {}", soundName);
    return true;
}

FMOD::Channel* AudioManager::PlaySound(const std::string& soundName, bool loop, float volume, float pitch)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
    {
        spdlog::error("Sound not found: {}", soundName);
        return nullptr;
    }
    
    SoundData& data = it->second;
    if (!data.sound)
    {
        spdlog::error("Sound not valid: {}", soundName);
        return nullptr;
    }
    
    FMOD_MODE currentMode;
    data.sound->getMode(&currentMode);
    if (loop) currentMode |= FMOD_LOOP_NORMAL;
    else currentMode &= ~FMOD_LOOP_NORMAL;
    data.sound->setMode(currentMode);

    FMOD::Channel* channel = nullptr;
    FMOD_RESULT result = FModWrapper::GetInstance().GetSystem()->playSound(data.sound, nullptr, true, &channel);
    if (result != FMOD_OK)
    {
        spdlog::error("FMOD playSound failed: {}", FMOD_ErrorString(result));
        return nullptr;
    }
    else
    {
        spdlog::info("FMOD playSound success: {}", soundName);
    }
    
    channel->setVolume(volume);
    float defaultFrequency;
    channel->getFrequency(&defaultFrequency);
    channel->setFrequency(defaultFrequency * pitch);
    channel->setPaused(false);

    float volumeTemp;
    channel->getVolume(&volumeTemp);
    spdlog::info("Volume of {} after configuration : {}", soundName, volumeTemp);

    data.channels.push_back(channel);

    return channel;
}

void AudioManager::StopSound(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
    {
        return;
    }

    for (auto* channel : it->second.channels)
    {
        if (channel)
        {
            bool isPlaying = false;
            channel->isPlaying(&isPlaying);
            if (isPlaying)
            {
                channel->stop();
            }
        }
    }

    it->second.channels.clear();
}

void AudioManager::StopAllSounds()
{
    for (auto& pair : m_sounds)
    {
        SoundData& data = pair.second;
        for (auto* channel : data.channels)
        {
            if (channel)
            {
                bool isPlaying = false;
                channel->isPlaying(&isPlaying);
                if (isPlaying)
                {
                    channel->stop();
                }
            }
        }
        data.channels.clear();
    }
}

void AudioManager::SetVolume(const std::string& soundName, float volume)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
        return;

    for (auto* channel : it->second.channels)
    {
        if (channel)
        {
            channel->setVolume(volume);
        }
    }
}

void AudioManager::SetPitch(const std::string& soundName, float pitch)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
        return;

    for (auto* channel : it->second.channels)
    {
        if (channel)
        {
            float defaultFrequency;
            channel->getFrequency(&defaultFrequency);
            channel->setFrequency(defaultFrequency * pitch);
        }
    }
}

void AudioManager::SetChannelVolume(FMOD::Channel* channel, float volume)
{
    if (channel)
    {
        channel->setVolume(volume);
    }
}

void AudioManager::SetChannelPitch(FMOD::Channel* channel, float pitch)
{
    if (channel)
    {
        float defaultFrequency;
        channel->getFrequency(&defaultFrequency);
        channel->setFrequency(defaultFrequency * pitch);
    }
}

void AudioManager::Update(float deltaTime)
{
    FModWrapper::GetInstance().GetSystem()->update();
}

FMOD::Channel* AudioManager::GetLastChannelOfSound(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end()) return nullptr;
    SoundData& data = it->second;
    if (!data.channels.empty())
        return data.channels.back();
    return nullptr;
}

} // namespace TSM