// tsm_audio_manager.cpp

#include "tsm_audio_manager.h"
#include "tsm_fmod_wrapper.h"
#include "tsm_mixer.h"
#include <fmod_dsp_effects.h>

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif
#include <utility>

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
    const std::u8string normalized = path.lexically_normal().generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(normalized.data()), normalized.size());
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

struct LoudnessCacheEntry
{
    std::uintmax_t size = 0;
    std::int64_t writeTime = 0;
    float integratedLufs = 0.0f;
    float truePeakDb = 0.0f;
};

bool ReadUnsignedInteger(const nlohmann::json& value, std::uintmax_t& output)
{
    try
    {
        if (value.is_number_unsigned())
        {
            output = value.get<std::uintmax_t>();
            return true;
        }
        if (value.is_number_integer())
        {
            const std::int64_t signedValue = value.get<std::int64_t>();
            if (signedValue < 0) return false;
            output = static_cast<std::uintmax_t>(signedValue);
            return true;
        }
    }
    catch (const std::exception&)
    {
    }
    return false;
}

bool ReadSignedInteger(const nlohmann::json& value, std::int64_t& output)
{
    try
    {
        if (value.is_number_unsigned())
        {
            const std::uint64_t unsignedValue = value.get<std::uint64_t>();
            if (unsignedValue > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()))
                return false;
            output = static_cast<std::int64_t>(unsignedValue);
            return true;
        }
        if (value.is_number_integer())
        {
            output = value.get<std::int64_t>();
            return true;
        }
    }
    catch (const std::exception&)
    {
    }
    return false;
}

bool ReadFiniteFloat(const nlohmann::json& value, float& output)
{
    if (!value.is_number()) return false;
    try
    {
        const double number = value.get<double>();
        if (!std::isfinite(number) ||
            number < static_cast<double>(std::numeric_limits<float>::lowest()) ||
            number > static_cast<double>(std::numeric_limits<float>::max()))
            return false;
        output = static_cast<float>(number);
        return std::isfinite(output);
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool ReadLoudnessCacheEntry(
    const nlohmann::json& value, LoudnessCacheEntry& entry)
{
    if (!value.is_object()) return false;
    const auto size = value.find("size");
    const auto writeTime = value.find("writeTime");
    const auto integratedLufs = value.find("integratedLufs");
    const auto truePeakDb = value.find("truePeakDb");
    if (size == value.end() || writeTime == value.end() ||
        integratedLufs == value.end() || truePeakDb == value.end())
        return false;

    if (!ReadUnsignedInteger(*size, entry.size) ||
        !ReadSignedInteger(*writeTime, entry.writeTime) ||
        !ReadFiniteFloat(*integratedLufs, entry.integratedLufs) ||
        !ReadFiniteFloat(*truePeakDb, entry.truePeakDb))
        return false;

    return entry.integratedLufs > -80.0f;
}

nlohmann::json LoadLoudnessCache(const std::string& cachePath)
{
    nlohmann::json sanitized = nlohmann::json::object();
    try
    {
        std::ifstream input(PathFromUtf8(cachePath), std::ios::binary);
        if (!input.is_open()) return sanitized;

        nlohmann::json raw;
        input >> raw;
        if (!raw.is_object())
        {
            spdlog::warn(
                "Ignoring loudness cache '{}': root value must be an object.", cachePath);
            return sanitized;
        }

        std::size_t rejectedEntries = 0;
        for (const auto& [key, value] : raw.items())
        {
            LoudnessCacheEntry entry;
            if (!ReadLoudnessCacheEntry(value, entry))
            {
                ++rejectedEntries;
                continue;
            }
            sanitized[key] = {
                {"size", entry.size},
                {"writeTime", entry.writeTime},
                {"integratedLufs", entry.integratedLufs},
                {"truePeakDb", entry.truePeakDb}
            };
        }
        if (rejectedEntries > 0)
        {
            spdlog::warn(
                "Ignored {} malformed loudness cache entr{} in '{}'.",
                rejectedEntries, rejectedEntries == 1 ? "y" : "ies", cachePath);
        }
    }
    catch (const std::exception& error)
    {
        spdlog::warn("Ignoring invalid loudness cache '{}': {}", cachePath, error.what());
        sanitized = nlohmann::json::object();
    }
    catch (...)
    {
        spdlog::warn("Ignoring invalid loudness cache '{}': unknown error.", cachePath);
        sanitized = nlohmann::json::object();
    }
    return sanitized;
}

bool SaveLoudnessCacheAtomically(
    const std::string& cachePath, const nlohmann::json& cache, std::string& errorMessage)
{
    std::filesystem::path temporaryPath;
    try
    {
        const std::filesystem::path targetPath = PathFromUtf8(cachePath);
        if (!targetPath.parent_path().empty())
        {
            std::error_code directoryError;
            std::filesystem::create_directories(targetPath.parent_path(), directoryError);
            if (directoryError)
            {
                errorMessage = directoryError.message();
                return false;
            }
        }

        temporaryPath = targetPath;
#ifdef _WIN32
        temporaryPath += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
#else
        temporaryPath += ".tmp";
#endif
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);

        {
            std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!output.is_open())
            {
                errorMessage = "unable to open temporary cache file";
                std::filesystem::remove(temporaryPath, ignored);
                return false;
            }
            output << cache.dump(2);
            output.flush();
            if (!output.good())
            {
                errorMessage = "unable to write temporary cache file";
                output.close();
                std::filesystem::remove(temporaryPath, ignored);
                return false;
            }
        }

#ifdef _WIN32
        if (!MoveFileExW(
                temporaryPath.c_str(), targetPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            errorMessage = std::error_code(
                static_cast<int>(GetLastError()), std::system_category()).message();
            std::filesystem::remove(temporaryPath, ignored);
            return false;
        }
#else
        std::error_code renameError;
        std::filesystem::rename(temporaryPath, targetPath, renameError);
        if (renameError)
        {
            errorMessage = renameError.message();
            std::filesystem::remove(temporaryPath, ignored);
            return false;
        }
#endif
        return true;
    }
    catch (const std::exception& error)
    {
        errorMessage = error.what();
    }
    catch (...)
    {
        errorMessage = "unknown error";
    }

    if (!temporaryPath.empty())
    {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
    }
    return false;
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

bool AudioManager::LoadSound(
    const std::string& soundName, const std::string& filePath, bool isStream, SoundKind kind)
{
    return LoadSoundInternal(soundName, filePath, isStream, kind, false);
}

bool AudioManager::LoadSoundInternal(
    const std::string& soundName, const std::string& filePath, bool isStream,
    SoundKind kind, bool replaceExisting)
{
    auto existing = m_sounds.find(soundName);
    if (existing != m_sounds.end())
    {
        if (existing->second.filePath == filePath && existing->second.kind == kind)
        {
            spdlog::debug("Sound '{}' is already loaded from the requested path.", soundName);
            return true;
        }
        if (!replaceExisting)
        {
            spdlog::error(
                "Sound '{}' is already loaded as '{}' from '{}' and cannot be replaced as '{}' from '{}'.",
                soundName, SoundKindToString(existing->second.kind), existing->second.filePath,
                SoundKindToString(kind), filePath);
            return false;
        }
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
    data.kind = kind;
    data.isMusic = kind == SoundKind::Music || kind == SoundKind::Wedding;

    // Commit a replacement only after FMOD has opened the new resource. A bad
    // path therefore leaves the previous wedding asset available.
    if (existing != m_sounds.end())
    {
        PruneStoppedChannels(existing->second);
        for (auto* channel : existing->second.channels)
        {
            if (!IsChannelPlayingSound(channel, existing->second.sound)) continue;
            CancelChannelFade(channel);
            channel->stop();
        }
        existing->second.channels.clear();

        if (existing->second.sound)
        {
            const FMOD_RESULT releaseResult = existing->second.sound->release();
            if (releaseResult != FMOD_OK)
            {
                spdlog::error(
                    "Failed to release previous sound '{}': {}. Replacement aborted.",
                    soundName, FMOD_ErrorString(releaseResult));
                newSound->release();
                return false;
            }
        }
        existing->second = std::move(data);
    }
    else
    {
        m_sounds.emplace(soundName, std::move(data));
    }

    spdlog::info("Sound loaded successfully: {}", soundName);
    return true;
}

bool AudioManager::UnloadSound(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it != m_sounds.end())
    {
        PruneStoppedChannels(it->second);
        for (auto* channel : it->second.channels)
        {
            if (!IsChannelPlayingSound(channel, it->second.sound)) continue;
            CancelChannelFade(channel);
            channel->stop();
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

    if (m_normalPlaybackBlocked && data.kind != SoundKind::Emergency)
    {
        spdlog::warn(
            "Playback of '{}' was blocked by the cinema safety gate.", soundName);
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
        if (data.kind != SoundKind::Music && data.kind != SoundKind::Wedding)
        {
            spdlog::error(
                "Sound '{}' is classified as '{}' and cannot be played as music.",
                soundName, SoundKindToString(data.kind));
            return nullptr;
        }
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
    if (data.kind == SoundKind::Music || data.kind == SoundKind::Wedding)
    {
        if (!EnsureMusicProcessing()) return nullptr;
        targetGroup = m_musicChannelGroup;
    }
    else if (data.kind == SoundKind::Emergency)
    {
        if (!EnsureChannelGroup(m_emergencyChannelGroup, "TSM Emergency"))
            return nullptr;
        targetGroup = m_emergencyChannelGroup;
    }
    else if (data.kind == SoundKind::Announcement)
    {
        if (!EnsureChannelGroup(m_announcementChannelGroup, "TSM Announcements"))
            return nullptr;
        targetGroup = m_announcementChannelGroup;
    }
    else
    {
        if (!EnsureChannelGroup(m_sfxChannelGroup, "TSM SFX")) return nullptr;
        targetGroup = m_sfxChannelGroup;
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
    CancelChannelFade(channel);
    RefreshChannelState();
    MixerState::GetInstance().ApplyAllVolumes();

    float volumeTemp = 0.0f;
    if (channel->getVolume(&volumeTemp) == FMOD_OK)
    {
        spdlog::info("Volume of {} after configuration: {}", soundName, volumeTemp);
    }

    data.channels.erase(
        std::remove(data.channels.begin(), data.channels.end(), channel),
        data.channels.end());
    data.channels.push_back(channel);

    return channel;
}

void AudioManager::StopSound(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
        return;

    PruneStoppedChannels(it->second);
    for (auto* channel : it->second.channels)
    {
        if (!IsChannelPlayingSound(channel, it->second.sound)) continue;
        CancelChannelFade(channel);
        channel->stop();
    }

    it->second.channels.clear();
}

void AudioManager::StopAllSounds()
{
    for (auto& pair : m_sounds)
    {
        SoundData& data = pair.second;
        PruneStoppedChannels(data);
        for (auto* channel : data.channels)
        {
            if (!IsChannelPlayingSound(channel, data.sound)) continue;
            CancelChannelFade(channel);
            channel->stop();
        }
        data.channels.clear();
    }
}

void AudioManager::StopAllNonEmergencyImmediately()
{
    for (auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        if (data.kind == SoundKind::Emergency) continue;
        PruneStoppedChannels(data);
        for (FMOD::Channel* channel : data.channels)
        {
            if (!IsChannelPlayingSound(channel, data.sound)) continue;
            CancelChannelFade(channel);
            channel->stop();
        }
        data.channels.clear();
    }

    // Channel groups are the final containment boundary, including voices no
    // longer represented in a logical SoundData channel list.
    if (m_musicChannelGroup) m_musicChannelGroup->stop();
    if (m_announcementChannelGroup) m_announcementChannelGroup->stop();
    if (m_sfxChannelGroup) m_sfxChannelGroup->stop();
}

void AudioManager::SetVolume(const std::string& soundName, float volume)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
        return;

    PruneStoppedChannels(it->second);
    for (auto* channel : it->second.channels)
    {
        if (IsChannelPlayingSound(channel, it->second.sound)) channel->setVolume(volume);
    }
}

void AudioManager::SetPitch(const std::string& soundName, float pitch)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
        return;

    PruneStoppedChannels(it->second);
    for (auto* channel : it->second.channels)
    {
        if (IsChannelPlayingSound(channel, it->second.sound)) ApplyPitch(channel, pitch);
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

void AudioManager::ApplyMixerVolumes(
    float master, float music, float announcement, float sfx, float effectiveDuck)
{
    const float safeMaster = std::max(master, 0.0f);
    const float safetyFactor = m_normalPlaybackBlocked ? 0.0f : 1.0f;
    if (m_musicChannelGroup)
        m_musicChannelGroup->setVolume(safetyFactor * safeMaster * std::max(music, 0.0f) *
                                       std::max(effectiveDuck, 0.0f));
    if (m_announcementChannelGroup)
        m_announcementChannelGroup->setVolume(
            safetyFactor * safeMaster * std::max(announcement, 0.0f));
    if (m_sfxChannelGroup)
        m_sfxChannelGroup->setVolume(safetyFactor * safeMaster * std::max(sfx, 0.0f));
    if (m_emergencyChannelGroup) m_emergencyChannelGroup->setVolume(1.0f);
}

void AudioManager::SetNormalPlaybackBlocked(bool blocked)
{
    m_normalPlaybackBlocked = blocked;
    MixerState::GetInstance().ApplyAllVolumes();
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

    RefreshChannelState();
}

void AudioManager::RefreshChannelState()
{
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
    
    const bool success = LoadSoundInternal(
        soundId, filePath, true, SoundKind::Wedding, true);
    
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
    
    bool success = LoadSound(announcementId, filePath, true, SoundKind::Announcement);
    
    if (success) {
        spdlog::info("Announcement '{}' loaded successfully: {}", announcementId, filePath);
    } else {
        spdlog::error("Failed to load announcement '{}': {}", announcementId, filePath);
    }
    
    return success;
}

bool AudioManager::LoadEmergencySound(
    const std::string& soundName, const std::string& filePath)
{
    const bool success = LoadSoundInternal(
        soundName, filePath, true, SoundKind::Emergency, true);
    if (success)
        spdlog::info("Emergency preset '{}' loaded successfully: {}", soundName, filePath);
    else
        spdlog::error("Failed to load emergency preset '{}': {}", soundName, filePath);
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
    if (data.kind != SoundKind::Music && data.kind != SoundKind::Wedding) return;
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
    if (m_loudnessThreadStarted && !m_loudnessWorkerExited.load()) return;
    if (m_loudnessThread.joinable()) m_loudnessThread.join();
    m_loudnessThreadStarted = true;
    m_loudnessWorkerExited.store(false);
    m_stopLoudnessThread.store(false);
    m_loudnessThread = std::thread(&AudioManager::LoudnessWorkerMain, this);
}

void AudioManager::LoudnessWorkerMain()
{
    FMOD::System* analysisSystem = nullptr;
    LoudnessTask activeTask;
    bool hasActiveTask = false;

    const auto failPendingTasks = [this, &activeTask, &hasActiveTask]
    {
        std::lock_guard<std::mutex> lock(m_loudnessMutex);
        if (hasActiveTask)
        {
            m_loudnessResults.push_back(
                {activeTask.soundName, activeTask.filePath, false, 0.0f, 0.0f});
            hasActiveTask = false;
        }
        while (!m_loudnessTasks.empty())
        {
            LoudnessTask task = std::move(m_loudnessTasks.front());
            m_loudnessTasks.pop_front();
            m_loudnessResults.push_back(
                {task.soundName, task.filePath, false, 0.0f, 0.0f});
        }
        m_activeLoudnessSound.clear();
    };

    try
    {
        nlohmann::json cache = LoadLoudnessCache(m_loudnessCachePath);

        FMOD_RESULT result = FMOD::System_Create(&analysisSystem);
        if (result == FMOD_OK && analysisSystem)
        {
            result = analysisSystem->setOutput(FMOD_OUTPUTTYPE_NOSOUND_NRT);
        }
        if (result == FMOD_OK && analysisSystem)
        {
            result = analysisSystem->init(8, FMOD_INIT_STREAM_FROM_UPDATE, nullptr);
        }
        if (result != FMOD_OK || !analysisSystem)
        {
            spdlog::error(
                "Unable to initialize LUFS analysis engine: {}", FMOD_ErrorString(result));
            failPendingTasks();
        }
        else
        {
            while (!m_stopLoudnessThread.load())
            {
                {
                    std::unique_lock<std::mutex> lock(m_loudnessMutex);
                    m_loudnessCondition.wait(lock, [this]
                    {
                        return m_stopLoudnessThread.load() || !m_loudnessTasks.empty();
                    });
                    if (m_stopLoudnessThread.load()) break;
                    activeTask = std::move(m_loudnessTasks.front());
                    m_loudnessTasks.pop_front();
                    hasActiveTask = true;
                    m_activeLoudnessSound = activeTask.soundName;
                }

                LoudnessResult loudnessResult;
                loudnessResult.soundName = activeTask.soundName;
                loudnessResult.filePath = activeTask.filePath;
                try
                {
                    const std::string cacheKey = NormalizeCacheKey(activeTask.filePath);
                    const std::uintmax_t fileSize = GetFileSize(activeTask.filePath);
                    const std::int64_t writeTime = GetFileWriteTime(activeTask.filePath);
                    const auto cacheIt = cache.find(cacheKey);
                    LoudnessCacheEntry cacheEntry;
                    if (cacheIt != cache.end() &&
                        ReadLoudnessCacheEntry(*cacheIt, cacheEntry) &&
                        cacheEntry.size == fileSize && cacheEntry.writeTime == writeTime)
                    {
                        loudnessResult.integratedLufs = cacheEntry.integratedLufs;
                        loudnessResult.truePeakDb = cacheEntry.truePeakDb;
                        loudnessResult.success = true;
                    }
                    else
                    {
                        loudnessResult.success = AnalyzeLoudness(
                            analysisSystem, activeTask.filePath, m_stopLoudnessThread,
                            loudnessResult.integratedLufs, loudnessResult.truePeakDb);
                        if (loudnessResult.success)
                        {
                            cache[cacheKey] = {
                                {"size", fileSize},
                                {"writeTime", writeTime},
                                {"integratedLufs", loudnessResult.integratedLufs},
                                {"truePeakDb", loudnessResult.truePeakDb}
                            };
                            std::string saveError;
                            if (!SaveLoudnessCacheAtomically(
                                    m_loudnessCachePath, cache, saveError))
                            {
                                spdlog::warn(
                                    "Unable to save loudness cache '{}': {}",
                                    m_loudnessCachePath, saveError);
                            }
                        }
                    }
                }
                catch (const std::exception& error)
                {
                    loudnessResult.success = false;
                    spdlog::error(
                        "Unexpected LUFS analysis error for '{}': {}",
                        activeTask.soundName, error.what());
                }
                catch (...)
                {
                    loudnessResult.success = false;
                    spdlog::error(
                        "Unknown LUFS analysis error for '{}'.", activeTask.soundName);
                }

                {
                    std::lock_guard<std::mutex> lock(m_loudnessMutex);
                    m_activeLoudnessSound.clear();
                    hasActiveTask = false;
                    m_loudnessResults.push_back(std::move(loudnessResult));
                }
            }
        }
    }
    catch (const std::exception& error)
    {
        spdlog::error("LUFS worker stopped after an unexpected error: {}", error.what());
        failPendingTasks();
    }
    catch (...)
    {
        spdlog::error("LUFS worker stopped after an unknown error.");
        failPendingTasks();
    }

    if (analysisSystem)
    {
        analysisSystem->close();
        analysisSystem->release();
    }
    m_loudnessWorkerExited.store(true);
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
        PruneStoppedChannels(data);
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
    std::map<std::string, int> queuePositions;
    {
        std::lock_guard<std::mutex> lock(m_loudnessMutex);
        activeSound = m_activeLoudnessSound;
        int position = 1;
        for (const LoudnessTask& task : m_loudnessTasks)
        {
            queuePositions.emplace(task.soundName, position++);
        }
    }

    std::vector<LoudnessDiagnostic> diagnostics;
    for (const auto& [soundName, data] : m_sounds)
    {
        if (!data.isMusic) continue;
        LoudnessStatus status = data.loudnessStatus;
        if (soundName == activeSound) status = LoudnessStatus::Analyzing;
        const auto queueIt = queuePositions.find(soundName);
        const int queuePosition = queueIt != queuePositions.end() ? queueIt->second : -1;
        diagnostics.push_back({
            soundName, data.filePath, status, data.integratedLufs,
            data.truePeakDb, data.normalizationGainDb, queuePosition
        });
    }
    const auto statusRank = [](LoudnessStatus status)
    {
        switch (status)
        {
            case LoudnessStatus::Analyzing: return 0;
            case LoudnessStatus::Queued: return 1;
            case LoudnessStatus::Ready: return 2;
            case LoudnessStatus::Failed: return 3;
            case LoudnessStatus::NotQueued: return 4;
        }
        return 5;
    };
    std::sort(diagnostics.begin(), diagnostics.end(), [&](const auto& left, const auto& right)
    {
        const int leftRank = statusRank(left.status);
        const int rightRank = statusRank(right.status);
        if (leftRank != rightRank) return leftRank < rightRank;
        if (left.status == LoudnessStatus::Queued &&
            left.queuePosition != right.queuePosition)
        {
            return left.queuePosition < right.queuePosition;
        }
        return left.filePath < right.filePath;
    });
    return diagnostics;
}

const char* AudioManager::SoundKindToString(SoundKind kind)
{
    switch (kind)
    {
        case SoundKind::SoundEffect: return "sfx";
        case SoundKind::Music: return "music";
        case SoundKind::Announcement: return "announcement";
        case SoundKind::Wedding: return "wedding";
        case SoundKind::Emergency: return "emergency";
    }
    return "unknown";
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
        PruneStoppedChannels(data);
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

void AudioManager::StopLoudnessWorker()
{
    m_stopLoudnessThread.store(true);
    m_loudnessCondition.notify_all();
    if (m_loudnessThread.joinable()) m_loudnessThread.join();
    m_loudnessThreadStarted = false;
    m_loudnessWorkerExited.store(false);
    {
        std::lock_guard<std::mutex> lock(m_loudnessMutex);
        m_loudnessTasks.clear();
        m_loudnessResults.clear();
        m_activeLoudnessSound.clear();
    }
    m_stopLoudnessThread.store(false);
}

void AudioManager::ConfigureLoudness(const std::string& cachePath, float targetLufs)
{
    StopLoudnessWorker();
    m_loudnessCachePath = cachePath.empty() ? "loudness_cache.json" : cachePath;
    m_loudnessTargetLufs = std::clamp(targetLufs, -30.0f, -8.0f);
}

bool AudioManager::ClearLoudnessCache(bool& removed, std::string& errorMessage)
{
    removed = false;
    errorMessage.clear();
    StopLoudnessWorker();

    std::error_code removeError;
    removed = std::filesystem::remove(PathFromUtf8(m_loudnessCachePath), removeError);
    if (removeError)
    {
        errorMessage = removeError.message();
        for (auto& [soundName, data] : m_sounds)
        {
            (void)soundName;
            if (data.loudnessStatus == LoudnessStatus::Queued ||
                data.loudnessStatus == LoudnessStatus::Analyzing)
                data.loudnessStatus = LoudnessStatus::NotQueued;
        }
        return false;
    }

    for (auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        if (data.kind != SoundKind::Music && data.kind != SoundKind::Wedding) continue;

        const float oldGain = data.normalizationGainLinear;
        data.loudnessStatus = LoudnessStatus::NotQueued;
        data.integratedLufs = 0.0f;
        data.truePeakDb = 0.0f;
        data.normalizationGainDb = 0.0f;
        data.normalizationGainLinear = 1.0f;

        if (oldGain <= 0.0f || std::abs(oldGain - 1.0f) < 0.000001f) continue;
        const float gainRatio = 1.0f / oldGain;
        PruneStoppedChannels(data);
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
    }
    return true;
}

void AudioManager::Shutdown()
{
    StopAllSounds();
    StopLoudnessWorker();

    for (auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        if (data.sound)
        {
            data.sound->release();
            data.sound = nullptr;
        }
        data.channels.clear();
    }
    m_sounds.clear();
    m_channelFades.clear();

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
    if (m_announcementChannelGroup)
    {
        m_announcementChannelGroup->release();
        m_announcementChannelGroup = nullptr;
    }
    if (m_sfxChannelGroup)
    {
        m_sfxChannelGroup->release();
        m_sfxChannelGroup = nullptr;
    }
    if (m_emergencyChannelGroup)
    {
        m_emergencyChannelGroup->release();
        m_emergencyChannelGroup = nullptr;
    }
    m_normalPlaybackBlocked = false;
}

bool AudioManager::EnsureChannelGroup(FMOD::ChannelGroup*& group, const char* name)
{
    if (group) return true;
    FMOD::System* system = FModWrapper::GetInstance().GetSystem();
    if (!system) return false;
    const FMOD_RESULT result = system->createChannelGroup(name, &group);
    if (result == FMOD_OK && group) return true;
    spdlog::error("Failed to create channel group '{}': {}", name, FMOD_ErrorString(result));
    group = nullptr;
    return false;
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

    PruneStoppedChannels(it->second);
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
        if (pair.second.kind == SoundKind::Emergency) continue;
        PruneStoppedChannels(pair.second);
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
        [channel](const ChannelFade& fade)
        {
            return fade.channel == channel &&
                IsChannelPlayingSound(fade.channel, fade.expectedSound);
        });
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
                return !IsChannelPlayingSound(channel, data.sound);
            }),
        data.channels.end());
}

bool AudioManager::IsChannelPlayingSound(
    FMOD::Channel* channel, FMOD::Sound* expectedSound)
{
    if (!channel || !expectedSound) return false;

    bool isPlaying = false;
    FMOD::Sound* currentSound = nullptr;
    return channel->isPlaying(&isPlaying) == FMOD_OK && isPlaying &&
        channel->getCurrentSound(&currentSound) == FMOD_OK &&
        currentSound == expectedSound;
}

bool AudioManager::ApplyPitch(FMOD::Channel* channel, float pitch)
{
    if (!channel || !std::isfinite(pitch) || pitch <= 0.0f)
        return false;
    return channel->setPitch(pitch) == FMOD_OK;
}

} // namespace TSM
