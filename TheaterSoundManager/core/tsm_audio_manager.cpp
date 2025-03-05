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
    data.filePath = filePath;
    m_sounds[soundName] = data;

    spdlog::info("Sound loaded successfully: {}", soundName);
    return true;
}

bool AudioManager::UnloadSound(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it != m_sounds.end())
    {
        if (it->second.sound)
        {
            FMOD_RESULT result = it->second.sound->release();
            if (result != FMOD_OK)
            {
                spdlog::error("Failed to release sound {}: {}", soundName, FMOD_ErrorString(result));
                return false;
            }
        }

        for (auto channel : it->second.channels)
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

        m_sounds.erase(it);
        spdlog::info("Sound {} unloaded successfully", soundName);
        return true;
    }
    
    spdlog::warn("Attempted to unload non-existent sound: {}", soundName);
    return false;
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
    if (loop)
        currentMode |= FMOD_LOOP_NORMAL;
    else
        currentMode &= ~FMOD_LOOP_NORMAL;
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
    spdlog::info("Volume of {} after configuration: {}", soundName, volumeTemp);

    data.channels.push_back(channel);

    return channel;
}

void AudioManager::StopSound(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
        return;

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
    
    if (m_isFadingIn)
    {
        m_fadeTimer += deltaTime;
        float progress = m_fadeTimer / m_fadeDuration;
        
        if (progress >= 1.0f)
        {
            auto it = m_sounds.find(m_fadingSoundName);
            if (it != m_sounds.end())
            {
                for (auto* channel : it->second.channels)
                {
                    if (channel)
                    {
                        bool isPlaying = false;
                        channel->isPlaying(&isPlaying);
                        if (isPlaying)
                        {
                            channel->setVolume(m_fadeTargetVolume);
                        }
                    }
                }
            }
            
            m_isFadingIn = false;
            spdlog::info("Fade-in completed for sound: {}", m_fadingSoundName);
        }
        else
        {
            float currentVolume = progress * m_fadeTargetVolume;
            
            auto it = m_sounds.find(m_fadingSoundName);
            if (it != m_sounds.end())
            {
                for (auto* channel : it->second.channels)
                {
                    if (channel)
                    {
                        bool isPlaying = false;
                        channel->isPlaying(&isPlaying);
                        if (isPlaying)
                        {
                            channel->setVolume(currentVolume);
                        }
                    }
                }
            }
        }
    }
    
    if (m_isFadingOut)
    {
        m_fadeTimer += deltaTime;
        float progress = m_fadeTimer / m_fadeDuration;
        
        if (progress >= 1.0f)
        {
            auto it = m_sounds.find(m_fadingSoundName);
            if (it != m_sounds.end())
            {
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
            
            m_isFadingOut = false;
            spdlog::info("Fade-out completed for sound: {}", m_fadingSoundName);
        }
        else
        {
            float currentVolume = (1.0f - progress) * m_fadeTargetVolume;
            
            auto it = m_sounds.find(m_fadingSoundName);
            if (it != m_sounds.end())
            {
                for (auto* channel : it->second.channels)
                {
                    if (channel)
                    {
                        bool isPlaying = false;
                        channel->isPlaying(&isPlaying);
                        if (isPlaying)
                        {
                            channel->setVolume(currentVolume);
                        }
                    }
                }
            }
        }
    }
}

FMOD::Channel* AudioManager::GetLastChannelOfSound(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end() || it->second.channels.empty())
        return nullptr;
    
    return it->second.channels.back();
}

bool AudioManager::LoadWeddingPhaseSound(int phase, const std::string& filePath)
{
    std::string soundId;
    
    switch (phase) {
        case 1:
            soundId = "wedding_entrance_sound";
            break;
        case 2:
            soundId = "wedding_ceremony_sound";
            break;
        case 3:
            soundId = "wedding_exit_sound";
            break;
        default:
            spdlog::error("Phase de mariage invalide: {}", phase);
            return false;
    }
    
    StopSound(soundId);
    if (m_sounds.find(soundId) != m_sounds.end()) {
        UnloadSound(soundId);
    }
    
    bool success = LoadSound(soundId, filePath, true);
    
    if (success) {
        spdlog::info("Son de mariage phase {} chargé avec succès: {}", phase, filePath);
    } else {
        spdlog::error("Échec du chargement du son de mariage phase {}: {}", phase, filePath);
    }
    
    return success;
}

bool AudioManager::LoadAnnouncement(const std::string& announcementId, const std::string& filePath)
{
    StopSound(announcementId);
    if (m_sounds.find(announcementId) != m_sounds.end()) {
        UnloadSound(announcementId);
    }
    
    bool success = LoadSound(announcementId, filePath, true);
    
    if (success) {
        spdlog::info("Annonce '{}' chargée avec succès: {}", announcementId, filePath);
    } else {
        spdlog::error("Échec du chargement de l'annonce '{}': {}", announcementId, filePath);
    }
    
    return success;
}

FMOD::Sound* AudioManager::GetSound(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it != m_sounds.end()) {
        return it->second.sound;
    }
    return nullptr;
}

FMOD::Channel* AudioManager::PlaySoundWithFadeIn(const std::string& soundName, bool loop, float volume, float pitch)
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
    if (loop)
        currentMode |= FMOD_LOOP_NORMAL;
    else
        currentMode &= ~FMOD_LOOP_NORMAL;
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
        spdlog::info("FMOD playSound success with fade-in: {}", soundName);
    }
    
    channel->setVolume(0.0f);
    float defaultFrequency;
    channel->getFrequency(&defaultFrequency);
    channel->setFrequency(defaultFrequency * pitch);
    channel->setPaused(false);

    data.channels.push_back(channel);
    
    m_isFadingIn = true;
    m_isFadingOut = false;
    m_fadeTimer = 0.0f;
    m_fadeDuration = 1.5f;
    m_fadingSoundName = soundName;
    m_fadeTargetVolume = volume;
    
    spdlog::info("Starting fade-in for sound: {} (target volume: {})", soundName, volume);

    return channel;
}

void AudioManager::StopSoundWithFadeOut(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
    {
        spdlog::warn("Attempted to stop non-existent sound: {}", soundName);
        return;
    }
    
    if (it->second.channels.empty())
    {
        spdlog::warn("No active channels for sound: {}", soundName);
        return;
    }
    
    m_isFadingOut = true;
    m_isFadingIn = false;
    m_fadeTimer = 0.0f;
    m_fadeDuration = 1.5f;
    m_fadingSoundName = soundName;
    
    float currentVolume = 0.0f;
    for (auto* channel : it->second.channels)
    {
        if (channel)
        {
            bool isPlaying = false;
            channel->isPlaying(&isPlaying);
            if (isPlaying)
            {
                channel->getVolume(&currentVolume);
                break;
            }
        }
    }
    
    m_fadeTargetVolume = currentVolume;
    
    spdlog::info("Starting fade-out for sound: {} (from volume: {})", soundName, currentVolume);
}

void AudioManager::StopAllSoundsWithFadeOut()
{
    for (auto& pair : m_sounds)
    {
        const std::string& soundName = pair.first;
        
        if (soundName.find("sfx") != std::string::npos || 
            soundName.find("announce") != std::string::npos)
        {
            continue;
        }
        
        if (!pair.second.channels.empty())
        {
            bool hasActiveChannel = false;
            for (auto* channel : pair.second.channels)
            {
                if (channel)
                {
                    bool isPlaying = false;
                    channel->isPlaying(&isPlaying);
                    if (isPlaying)
                    {
                        hasActiveChannel = true;
                        break;
                    }
                }
            }
            
            if (hasActiveChannel)
            {
                StopSoundWithFadeOut(soundName);
                break;
            }
        }
    }
}

} // namespace TSM
