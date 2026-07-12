// tsm_audio_manager.cpp

#include "tsm_audio_manager.h"
#include "tsm_fmod_wrapper.h"
#include <fmod_dsp_effects.h>

#include <spdlog/spdlog.h>
#include <json/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace TSM 
{

namespace
{
std::filesystem::path PathFromUtf8(const std::string& filePath)
{
    const auto* begin = reinterpret_cast<const char8_t*>(filePath.data());
    return std::filesystem::path(std::u8string(begin, begin + filePath.size()));
}

std::string NormalizeCacheKey(const std::string& filePath)
{
    std::error_code error;
    std::filesystem::path path = std::filesystem::absolute(PathFromUtf8(filePath), error);
    if (error) path = PathFromUtf8(filePath);
    return path.lexically_normal().generic_string();
}

std::uintmax_t GetFileSize(const std::string& filePath)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(PathFromUtf8(filePath), error);
    return error ? 0 : size;
}

std::int64_t GetFileWriteTime(const std::string& filePath)
{
    std::error_code error;
    const auto time = std::filesystem::last_write_time(PathFromUtf8(filePath), error);
    return error ? 0 : static_cast<std::int64_t>(time.time_since_epoch().count());
}

bool AnalyzeLoudness(FMOD::System* system, const std::string& filePath,
                     const std::atomic<bool>& stopRequested,
                     float& integratedLufs, float& truePeakDb)
{
    FMOD::Sound* sound = nullptr;
    FMOD_RESULT result = system->createSound(
        filePath.c_str(), FMOD_2D | FMOD_CREATESTREAM, nullptr, &sound);
    if (result != FMOD_OK || !sound) return false;

    FMOD::DSP* meter = nullptr;
    FMOD::Channel* channel = nullptr;
    result = system->createDSPByType(FMOD_DSP_TYPE_LOUDNESS_METER, &meter);
    if (result == FMOD_OK && meter)
    {
        result = system->playSound(sound, nullptr, true, &channel);
    }
    if (result == FMOD_OK && channel)
    {
        result = channel->addDSP(0, meter);
    }
    if (result == FMOD_OK)
    {
        meter->setActive(true);
        channel->setPaused(false);
    }

    unsigned int lengthMs = 0;
    sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
    const std::uint64_t maxUpdates = std::max<std::uint64_t>(
        1000, static_cast<std::uint64_t>(lengthMs / 1000 + 1) * 200);

    bool isPlaying = result == FMOD_OK;
    std::uint64_t updates = 0;
    while (isPlaying && !stopRequested.load() && updates++ < maxUpdates)
    {
        if (system->update() != FMOD_OK || channel->isPlaying(&isPlaying) != FMOD_OK)
        {
            isPlaying = false;
            result = FMOD_ERR_INTERNAL;
        }
    }

    bool success = false;
    if (!stopRequested.load() && result == FMOD_OK && updates < maxUpdates)
    {
        void* data = nullptr;
        unsigned int dataLength = 0;
        if (meter->getParameterData(
                FMOD_DSP_LOUDNESS_METER_INFO, &data, &dataLength, nullptr, 0) == FMOD_OK &&
            data && dataLength >= sizeof(FMOD_DSP_LOUDNESS_METER_INFO_TYPE))
        {
            const auto* info = static_cast<const FMOD_DSP_LOUDNESS_METER_INFO_TYPE*>(data);
            integratedLufs = info->integratedloudness;
            truePeakDb = info->maxtruepeak;
            success = std::isfinite(integratedLufs) && integratedLufs > -80.0f &&
                      std::isfinite(truePeakDb);
        }
    }

    if (channel)
    {
        channel->stop();
        if (meter) channel->removeDSP(meter);
    }
    if (meter) meter->release();
    sound->release();
    system->update();
    return success;
}
}

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
    return PlaySoundInternal(soundName, loop, volume, pitch, false);
}

FMOD::Channel* AudioManager::PlayMusic(const std::string& soundName, bool loop, float volume, float pitch)
{
    return PlaySoundInternal(soundName, loop, volume, pitch, true);
}

FMOD::Channel* AudioManager::PlaySoundInternal(
    const std::string& soundName, bool loop, float volume, float pitch, bool normalizeMusic)
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

    if (normalizeMusic)
    {
        data.isMusic = true;
        QueueLoudnessAnalysis(soundName);
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

    FMOD::ChannelGroup* targetGroup = nullptr;
    if (normalizeMusic && EnsureMusicProcessing())
    {
        targetGroup = m_musicChannelGroup;
    }

    FMOD::Channel* channel = nullptr;
    FMOD_RESULT result = system->playSound(data.sound, targetGroup, true, &channel);
    if (result != FMOD_OK || !channel)
    {
        spdlog::error("FMOD playSound failed: {}", FMOD_ErrorString(result));
        return nullptr;
    }
    else
    {
        spdlog::info("FMOD playSound success: {}", soundName);
    }
    
    const float normalizedVolume = normalizeMusic
        ? volume * data.normalizationGainLinear
        : volume;
    channel->setVolume(normalizedVolume);
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
    ApplyLoudnessResults();

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
        QueueLoudnessAnalysis(soundId);
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
    FMOD::Channel* channel = PlaySoundInternal(soundName, loop, 0.0f, pitch, false);
    if (!channel)
        return nullptr;

    StartChannelFade(channel, volume, false);
    spdlog::info("Starting fade-in for sound: {} (target volume: {})", soundName, volume);
    return channel;
}

FMOD::Channel* AudioManager::PlayMusicWithFadeIn(const std::string& soundName, bool loop, float volume, float pitch)
{
    FMOD::Channel* channel = PlaySoundInternal(soundName, loop, 0.0f, pitch, true);
    if (!channel) return nullptr;

    float normalizationGain = 1.0f;
    const auto soundIt = m_sounds.find(soundName);
    if (soundIt != m_sounds.end()) normalizationGain = soundIt->second.normalizationGainLinear;
    StartChannelFade(channel, volume * normalizationGain, false);
    spdlog::info("Starting normalized music fade-in: {} (target volume: {})", soundName, volume);
    return channel;
}

void AudioManager::QueueLoudnessAnalysis(const std::string& soundName)
{
    auto soundIt = m_sounds.find(soundName);
    if (soundIt == m_sounds.end() || soundIt->second.filePath.empty()) return;

    SoundData& data = soundIt->second;
    data.isMusic = true;
    if (data.loudnessStatus == LoudnessStatus::Queued ||
        data.loudnessStatus == LoudnessStatus::Analyzing ||
        data.loudnessStatus == LoudnessStatus::Ready)
    {
        return;
    }

    data.loudnessStatus = LoudnessStatus::Queued;
    {
        std::lock_guard<std::mutex> lock(m_loudnessMutex);
        m_loudnessTasks.push_back({soundName, data.filePath});
    }
    StartLoudnessWorker();
    m_loudnessCondition.notify_one();
}

void AudioManager::StartLoudnessWorker()
{
    if (m_loudnessThreadStarted) return;
    m_loudnessThreadStarted = true;
    m_stopLoudnessThread.store(false);
    m_loudnessThread = std::thread(&AudioManager::LoudnessWorkerMain, this);
}

void AudioManager::LoudnessWorkerMain()
{
    nlohmann::json cache = nlohmann::json::object();
    try
    {
        std::ifstream cacheFile(m_loudnessCachePath);
        if (cacheFile.is_open()) cacheFile >> cache;
    }
    catch (const std::exception& error)
    {
        spdlog::warn("Ignoring invalid loudness cache '{}': {}", m_loudnessCachePath, error.what());
        cache = nlohmann::json::object();
    }

    FMOD::System* analysisSystem = nullptr;
    FMOD_RESULT result = FMOD::System_Create(&analysisSystem);
    if (result == FMOD_OK && analysisSystem)
    {
        result = analysisSystem->setOutput(FMOD_OUTPUTTYPE_NOSOUND_NRT);
    }
    if (result == FMOD_OK)
    {
        result = analysisSystem->init(8, FMOD_INIT_STREAM_FROM_UPDATE, nullptr);
    }
    if (result != FMOD_OK || !analysisSystem)
    {
        spdlog::error("Unable to initialize LUFS analysis engine: {}", FMOD_ErrorString(result));
        if (analysisSystem) analysisSystem->release();
        return;
    }

    while (!m_stopLoudnessThread.load())
    {
        LoudnessTask task;
        {
            std::unique_lock<std::mutex> lock(m_loudnessMutex);
            m_loudnessCondition.wait(lock, [this]
            {
                return m_stopLoudnessThread.load() || !m_loudnessTasks.empty();
            });
            if (m_stopLoudnessThread.load()) break;
            task = std::move(m_loudnessTasks.front());
            m_loudnessTasks.pop_front();
            m_activeLoudnessSound = task.soundName;
        }

        LoudnessResult loudnessResult;
        loudnessResult.soundName = task.soundName;
        loudnessResult.filePath = task.filePath;

        const std::string cacheKey = NormalizeCacheKey(task.filePath);
        const std::uintmax_t fileSize = GetFileSize(task.filePath);
        const std::int64_t writeTime = GetFileWriteTime(task.filePath);
        const auto cacheIt = cache.find(cacheKey);
        if (cacheIt != cache.end() &&
            cacheIt->value("size", std::uintmax_t{0}) == fileSize &&
            cacheIt->value("writeTime", std::int64_t{0}) == writeTime)
        {
            loudnessResult.integratedLufs = cacheIt->value("integratedLufs", 0.0f);
            loudnessResult.truePeakDb = cacheIt->value("truePeakDb", 0.0f);
            loudnessResult.success = std::isfinite(loudnessResult.integratedLufs) &&
                                     loudnessResult.integratedLufs > -80.0f;
        }
        else
        {
            loudnessResult.success = AnalyzeLoudness(
                analysisSystem, task.filePath, m_stopLoudnessThread,
                loudnessResult.integratedLufs, loudnessResult.truePeakDb);
            if (loudnessResult.success)
            {
                cache[cacheKey] = {
                    {"size", fileSize},
                    {"writeTime", writeTime},
                    {"integratedLufs", loudnessResult.integratedLufs},
                    {"truePeakDb", loudnessResult.truePeakDb}
                };
                try
                {
                    std::ofstream cacheFile(m_loudnessCachePath);
                    if (cacheFile.is_open()) cacheFile << cache.dump(2);
                }
                catch (const std::exception& error)
                {
                    spdlog::warn("Unable to save loudness cache: {}", error.what());
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_loudnessMutex);
            m_activeLoudnessSound.clear();
            m_loudnessResults.push_back(std::move(loudnessResult));
        }
    }

    analysisSystem->close();
    analysisSystem->release();
}

void AudioManager::ApplyLoudnessResults()
{
    std::deque<LoudnessResult> results;
    {
        std::lock_guard<std::mutex> lock(m_loudnessMutex);
        results.swap(m_loudnessResults);
    }

    for (const LoudnessResult& result : results)
    {
        auto soundIt = m_sounds.find(result.soundName);
        if (soundIt == m_sounds.end() || soundIt->second.filePath != result.filePath) continue;

        SoundData& data = soundIt->second;
        if (!result.success)
        {
            data.loudnessStatus = LoudnessStatus::Failed;
            spdlog::warn("LUFS analysis failed for '{}'", result.soundName);
            continue;
        }

        const float oldGain = data.normalizationGainLinear;
        float gainDb = std::clamp(m_loudnessTargetLufs - result.integratedLufs, -12.0f, 12.0f);
        if (result.truePeakDb + gainDb > -1.0f)
        {
            gainDb = -1.0f - result.truePeakDb;
        }

        data.integratedLufs = result.integratedLufs;
        data.truePeakDb = result.truePeakDb;
        data.normalizationGainDb = gainDb;
        data.normalizationGainLinear = std::pow(10.0f, gainDb / 20.0f);
        data.loudnessStatus = LoudnessStatus::Ready;

        const float gainRatio = oldGain > 0.0f ? data.normalizationGainLinear / oldGain : 1.0f;
        for (FMOD::Channel* channel : data.channels)
        {
            bool isPlaying = false;
            float currentVolume = 0.0f;
            if (channel && channel->isPlaying(&isPlaying) == FMOD_OK && isPlaying &&
                channel->getVolume(&currentVolume) == FMOD_OK)
            {
                StartChannelFade(channel, currentVolume * gainRatio, false);
            }
        }

        spdlog::info("LUFS '{}' = {:.2f}, true peak {:.2f} dB, gain {:+.2f} dB",
                     result.soundName, result.integratedLufs, result.truePeakDb, gainDb);
    }
}

float AudioManager::GetNormalizationGainForChannel(FMOD::Channel* channel) const
{
    if (!channel) return 1.0f;
    FMOD::Sound* sound = nullptr;
    if (channel->getCurrentSound(&sound) != FMOD_OK || !sound) return 1.0f;

    for (const auto& [name, data] : m_sounds)
    {
        (void)name;
        if (data.sound == sound && data.isMusic) return data.normalizationGainLinear;
    }
    return 1.0f;
}

std::vector<AudioManager::LoudnessDiagnostic> AudioManager::GetLoudnessDiagnostics() const
{
    std::string activeSound;
    {
        std::lock_guard<std::mutex> lock(m_loudnessMutex);
        activeSound = m_activeLoudnessSound;
    }

    std::vector<LoudnessDiagnostic> diagnostics;
    for (const auto& [soundName, data] : m_sounds)
    {
        if (!data.isMusic) continue;
        LoudnessStatus status = data.loudnessStatus;
        if (soundName == activeSound) status = LoudnessStatus::Analyzing;
        diagnostics.push_back({
            soundName, data.filePath, status, data.integratedLufs,
            data.truePeakDb, data.normalizationGainDb
        });
    }
    return diagnostics;
}

const char* AudioManager::LoudnessStatusToString(LoudnessStatus status)
{
    switch (status)
    {
        case LoudnessStatus::NotQueued: return "not queued";
        case LoudnessStatus::Queued: return "queued";
        case LoudnessStatus::Analyzing: return "analyzing";
        case LoudnessStatus::Ready: return "ready";
        case LoudnessStatus::Failed: return "failed";
    }
    return "unknown";
}

void AudioManager::SetLoudnessTarget(float targetLufs)
{
    m_loudnessTargetLufs = std::clamp(targetLufs, -30.0f, -8.0f);
    for (auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        if (!data.isMusic || data.loudnessStatus != LoudnessStatus::Ready) continue;

        const float oldGain = data.normalizationGainLinear;
        float gainDb = std::clamp(m_loudnessTargetLufs - data.integratedLufs, -12.0f, 12.0f);
        if (data.truePeakDb + gainDb > -1.0f) gainDb = -1.0f - data.truePeakDb;
        data.normalizationGainDb = gainDb;
        data.normalizationGainLinear = std::pow(10.0f, gainDb / 20.0f);

        const float ratio = oldGain > 0.0f ? data.normalizationGainLinear / oldGain : 1.0f;
        for (FMOD::Channel* channel : data.channels)
        {
            bool isPlaying = false;
            float volume = 0.0f;
            if (channel && channel->isPlaying(&isPlaying) == FMOD_OK && isPlaying &&
                channel->getVolume(&volume) == FMOD_OK)
            {
                StartChannelFade(channel, volume * ratio, false);
            }
        }
    }
}

void AudioManager::Shutdown()
{
    StopAllSounds();

    m_stopLoudnessThread.store(true);
    m_loudnessCondition.notify_all();
    if (m_loudnessThread.joinable())
    {
        m_loudnessThread.join();
    }

    if (m_musicChannelGroup && m_musicLimiter)
    {
        m_musicChannelGroup->removeDSP(m_musicLimiter);
    }
    if (m_musicLimiter)
    {
        m_musicLimiter->release();
        m_musicLimiter = nullptr;
    }
    if (m_musicChannelGroup)
    {
        m_musicChannelGroup->release();
        m_musicChannelGroup = nullptr;
    }
}

bool AudioManager::EnsureMusicProcessing()
{
    if (m_musicChannelGroup && m_musicLimiter) return true;

    FMOD::System* system = FModWrapper::GetInstance().GetSystem();
    if (!system) return false;

    FMOD_RESULT result = system->createChannelGroup("TSM Music", &m_musicChannelGroup);
    if (result != FMOD_OK || !m_musicChannelGroup)
    {
        spdlog::error("Failed to create the music channel group: {}", FMOD_ErrorString(result));
        m_musicChannelGroup = nullptr;
        return false;
    }

    result = system->createDSPByType(FMOD_DSP_TYPE_LIMITER, &m_musicLimiter);
    if (result != FMOD_OK || !m_musicLimiter)
    {
        spdlog::error("Failed to create the music limiter: {}", FMOD_ErrorString(result));
        m_musicChannelGroup->release();
        m_musicChannelGroup = nullptr;
        m_musicLimiter = nullptr;
        return false;
    }

    m_musicLimiter->setParameterFloat(FMOD_DSP_LIMITER_RELEASETIME, 50.0f);
    m_musicLimiter->setParameterFloat(FMOD_DSP_LIMITER_CEILING, -1.0f);
    m_musicLimiter->setParameterFloat(FMOD_DSP_LIMITER_MAXIMIZERGAIN, 0.0f);
    m_musicLimiter->setParameterBool(FMOD_DSP_LIMITER_MODE, true);

    result = m_musicChannelGroup->addDSP(0, m_musicLimiter);
    if (result != FMOD_OK)
    {
        spdlog::error("Failed to attach the music limiter: {}", FMOD_ErrorString(result));
        m_musicLimiter->release();
        m_musicChannelGroup->release();
        m_musicLimiter = nullptr;
        m_musicChannelGroup = nullptr;
        return false;
    }

    m_musicLimiter->setActive(true);
    spdlog::info("Music true-peak limiter enabled at -1 dB");
    return true;
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
