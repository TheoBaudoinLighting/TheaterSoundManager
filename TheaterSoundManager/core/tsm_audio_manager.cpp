// tsm_audio_manager.cpp

#include "tsm_audio_manager.h"
#include "tsm_fmod_wrapper.h"

#include <spdlog/spdlog.h>
#include <algorithm>

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

    FMOD::System* system = FModWrapper::GetInstance().GetSystem();
    if (!system)
    {
        spdlog::error("Cannot load sound '{}': FMOD system is not initialized.", soundName);
        return false;
    }

    FMOD::Sound* newSound = nullptr;
    FMOD_RESULT result = system->createSound(filePath.c_str(), mode, nullptr, &newSound);
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
        for (auto* channel : it->second.channels)
        {
            if (channel)
            {
                CancelChannelFade(channel);
                bool isPlaying = false;
                FMOD_RESULT stateResult = channel->isPlaying(&isPlaying);
                if (stateResult == FMOD_OK && isPlaying)
                {
                    channel->stop();
                }
            }
        }
        it->second.channels.clear();

        if (it->second.sound)
        {
            FMOD_RESULT result = it->second.sound->release();
            if (result != FMOD_OK)
            {
                spdlog::error("Failed to release sound {}: {}", soundName, FMOD_ErrorString(result));
                return false;
            }
            it->second.sound = nullptr;
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
    
    FMOD_MODE currentMode = FMOD_DEFAULT;
    FMOD_RESULT modeResult = data.sound->getMode(&currentMode);
    if (modeResult != FMOD_OK)
    {
        spdlog::error("FMOD getMode failed for '{}': {}", soundName, FMOD_ErrorString(modeResult));
        return nullptr;
    }
    if (loop)
        currentMode |= FMOD_LOOP_NORMAL;
    else
        currentMode &= ~FMOD_LOOP_NORMAL;
    data.sound->setMode(currentMode);

    FMOD::System* system = FModWrapper::GetInstance().GetSystem();
    if (!system)
    {
        spdlog::error("Cannot play sound '{}': FMOD system is not initialized.", soundName);
        return nullptr;
    }

    FMOD::Channel* channel = nullptr;
    FMOD_RESULT result = system->playSound(data.sound, nullptr, true, &channel);
    if (result != FMOD_OK || !channel)
    {
        spdlog::error("FMOD playSound failed: {}", FMOD_ErrorString(result));
        return nullptr;
    }
    else
    {
        spdlog::info("FMOD playSound success: {}", soundName);
    }
    
    channel->setVolume(volume);
    ApplyPitch(channel, pitch);
    channel->setPaused(false);

    float volumeTemp = 0.0f;
    if (channel->getVolume(&volumeTemp) == FMOD_OK)
    {
        spdlog::info("Volume of {} after configuration: {}", soundName, volumeTemp);
    }

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
            CancelChannelFade(channel);
            bool isPlaying = false;
            FMOD_RESULT result = channel->isPlaying(&isPlaying);
            if (result == FMOD_OK && isPlaying)
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
                CancelChannelFade(channel);
                bool isPlaying = false;
                FMOD_RESULT result = channel->isPlaying(&isPlaying);
                if (result == FMOD_OK && isPlaying)
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
            bool isPlaying = false;
            if (channel->isPlaying(&isPlaying) == FMOD_OK && isPlaying)
            {
                channel->setVolume(volume);
            }
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
        ApplyPitch(channel, pitch);
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
    ApplyPitch(channel, pitch);
}
void AudioManager::Update(float deltaTime)
{
    FMOD::System* system = FModWrapper::GetInstance().GetSystem();
    if (system)
    {
        system->update();
    }

    for (auto fadeIt = m_channelFades.begin(); fadeIt != m_channelFades.end();)
    {
        ChannelFade& fade = *fadeIt;
        bool isPlaying = false;
        FMOD::Sound* currentSound = nullptr;
        const bool valid = fade.channel &&
            fade.channel->isPlaying(&isPlaying) == FMOD_OK && isPlaying &&
            fade.channel->getCurrentSound(&currentSound) == FMOD_OK &&
            currentSound == fade.expectedSound;

        if (!valid)
        {
            fadeIt = m_channelFades.erase(fadeIt);
            continue;
        }

        fade.timer += deltaTime;
        const float progress = fade.duration <= 0.0f
            ? 1.0f
            : std::min(fade.timer / fade.duration, 1.0f);
        const float volume = fade.startVolume + (fade.targetVolume - fade.startVolume) * progress;
        fade.channel->setVolume(volume);

        if (progress >= 1.0f)
        {
            if (fade.stopWhenComplete)
            {
                fade.channel->stop();
            }
            fadeIt = m_channelFades.erase(fadeIt);
        }
        else
        {
            ++fadeIt;
        }
    }

    for (auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        PruneStoppedChannels(data);
    }
}

FMOD::Channel* AudioManager::GetLastChannelOfSound(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
        return nullptr;

    PruneStoppedChannels(it->second);
    if (it->second.channels.empty())
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
            spdlog::error("Invalid wedding phase: {}", phase);
            return false;
    }
    
    StopSound(soundId);
    if (m_sounds.find(soundId) != m_sounds.end()) {
        UnloadSound(soundId);
    }
    
    bool success = LoadSound(soundId, filePath, true);
    
    if (success) {
        spdlog::info("Wedding phase {} sound loaded successfully: {}", phase, filePath);
    } else {
        spdlog::error("Failed to load wedding phase {} sound: {}", phase, filePath);
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
        spdlog::info("Announcement '{}' loaded successfully: {}", announcementId, filePath);
    } else {
        spdlog::error("Failed to load announcement '{}': {}", announcementId, filePath);
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
    FMOD::Channel* channel = PlaySound(soundName, loop, 0.0f, pitch);
    if (!channel)
        return nullptr;

    StartChannelFade(channel, volume, false);
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
    
    for (auto* channel : it->second.channels)
    {
        StopChannelWithFadeOut(channel);
    }

    spdlog::info("Starting fade-out for sound: {}", soundName);
}

void AudioManager::StopAllSoundsWithFadeOut()
{
    for (auto& pair : m_sounds)
    {
        for (auto* channel : pair.second.channels)
        {
            StopChannelWithFadeOut(channel);
        }
    }
}

void AudioManager::StopChannelWithFadeOut(FMOD::Channel* channel)
{
    if (!channel)
        return;

    bool isPlaying = false;
    if (channel->isPlaying(&isPlaying) != FMOD_OK || !isPlaying)
        return;

    StartChannelFade(channel, 0.0f, true);
}

bool AudioManager::IsChannelFading(FMOD::Channel* channel) const
{
    return std::any_of(m_channelFades.begin(), m_channelFades.end(),
        [channel](const ChannelFade& fade) { return fade.channel == channel; });
}

void AudioManager::StartChannelFade(FMOD::Channel* channel, float targetVolume, bool stopWhenComplete)
{
    if (!channel)
        return;

    bool isPlaying = false;
    FMOD::Sound* sound = nullptr;
    float currentVolume = 0.0f;
    if (channel->isPlaying(&isPlaying) != FMOD_OK || !isPlaying ||
        channel->getCurrentSound(&sound) != FMOD_OK || !sound ||
        channel->getVolume(&currentVolume) != FMOD_OK)
    {
        return;
    }

    CancelChannelFade(channel);
    m_channelFades.push_back(ChannelFade{
        channel,
        sound,
        0.0f,
        m_fadeDuration,
        currentVolume,
        targetVolume,
        stopWhenComplete
    });
}

void AudioManager::CancelChannelFade(FMOD::Channel* channel)
{
    m_channelFades.erase(
        std::remove_if(m_channelFades.begin(), m_channelFades.end(),
            [channel](const ChannelFade& fade) { return fade.channel == channel; }),
        m_channelFades.end());
}

void AudioManager::PruneStoppedChannels(SoundData& data)
{
    data.channels.erase(
        std::remove_if(data.channels.begin(), data.channels.end(),
            [&data](FMOD::Channel* channel)
            {
                if (!channel)
                    return true;

                bool isPlaying = false;
                FMOD::Sound* currentSound = nullptr;
                return channel->isPlaying(&isPlaying) != FMOD_OK || !isPlaying ||
                    channel->getCurrentSound(&currentSound) != FMOD_OK ||
                    currentSound != data.sound;
            }),
        data.channels.end());
}

bool AudioManager::ApplyPitch(FMOD::Channel* channel, float pitch)
{
    if (!channel)
        return false;

    FMOD::Sound* sound = nullptr;
    if (channel->getCurrentSound(&sound) != FMOD_OK || !sound)
        return false;

    float defaultFrequency = 0.0f;
    int priority = 0;
    if (sound->getDefaults(&defaultFrequency, &priority) != FMOD_OK || defaultFrequency <= 0.0f)
        return false;

    return channel->setFrequency(defaultFrequency * pitch) == FMOD_OK;
}

} // namespace TSM
