// tsm_audio_manager.h
#pragma once

#include <fmod.hpp>
#include <string>
#include <map>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace TSM 
{

class AudioManager 
{
public:
    enum class LoudnessStatus
    {
        NotQueued,
        Queued,
        Analyzing,
        Ready,
        Failed
    };

    struct SoundData
    {
        FMOD::Sound* sound = nullptr;
        std::vector<FMOD::Channel*> channels;
        std::string filePath;
        bool isMusic = false;
        LoudnessStatus loudnessStatus = LoudnessStatus::NotQueued;
        float integratedLufs = 0.0f;
        float truePeakDb = 0.0f;
        float normalizationGainDb = 0.0f;
        float normalizationGainLinear = 1.0f;
    };

    struct LoudnessDiagnostic
    {
        std::string soundName;
        std::string filePath;
        LoudnessStatus status = LoudnessStatus::NotQueued;
        float integratedLufs = 0.0f;
        float truePeakDb = 0.0f;
        float gainDb = 0.0f;
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
    FMOD::Channel* PlayMusic(const std::string& soundName, bool loop = false, float volume = 1.0f, float pitch = 1.0f);
    FMOD::Channel* PlayMusicWithFadeIn(const std::string& soundName, bool loop = false, float volume = 1.0f, float pitch = 1.0f);
    void StopSound(const std::string& soundName);
    void StopSoundWithFadeOut(const std::string& soundName);
    void StopChannelWithFadeOut(FMOD::Channel* channel);
    bool IsChannelFading(FMOD::Channel* channel) const;
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
    void QueueLoudnessAnalysis(const std::string& soundName);
    float GetNormalizationGainForChannel(FMOD::Channel* channel) const;
    std::vector<LoudnessDiagnostic> GetLoudnessDiagnostics() const;
    static const char* LoudnessStatusToString(LoudnessStatus status);
    void SetLoudnessTarget(float targetLufs);
    float GetLoudnessTarget() const { return m_loudnessTargetLufs; }
    void Shutdown();
private:
    struct ChannelFade
    {
        FMOD::Channel* channel = nullptr;
        FMOD::Sound* expectedSound = nullptr;
        float timer = 0.0f;
        float duration = 1.5f;
        float startVolume = 0.0f;
        float targetVolume = 0.0f;
        bool stopWhenComplete = false;
    };

    std::map<std::string, SoundData> m_sounds;
    std::vector<ChannelFade> m_channelFades;
    float m_fadeDuration = 1.5f;
    FMOD::ChannelGroup* m_musicChannelGroup = nullptr;
    FMOD::DSP* m_musicLimiter = nullptr;

    struct LoudnessTask
    {
        std::string soundName;
        std::string filePath;
    };

    struct LoudnessResult
    {
        std::string soundName;
        std::string filePath;
        bool success = false;
        float integratedLufs = 0.0f;
        float truePeakDb = 0.0f;
    };

    std::thread m_loudnessThread;
    mutable std::mutex m_loudnessMutex;
    std::condition_variable m_loudnessCondition;
    std::deque<LoudnessTask> m_loudnessTasks;
    std::deque<LoudnessResult> m_loudnessResults;
    std::string m_activeLoudnessSound;
    std::atomic<bool> m_stopLoudnessThread{false};
    bool m_loudnessThreadStarted = false;
    float m_loudnessTargetLufs = -16.0f;
    std::string m_loudnessCachePath = "loudness_cache.json";

    FMOD::Channel* PlaySoundInternal(const std::string& soundName, bool loop, float volume, float pitch, bool normalizeMusic);
    bool EnsureMusicProcessing();
    void StartLoudnessWorker();
    void LoudnessWorkerMain();
    void ApplyLoudnessResults();
    void StartChannelFade(FMOD::Channel* channel, float targetVolume, bool stopWhenComplete);
    void CancelChannelFade(FMOD::Channel* channel);
    void PruneStoppedChannels(SoundData& data);
    static bool ApplyPitch(FMOD::Channel* channel, float pitch);
public:
    AudioManager() = default;
    ~AudioManager() = default;

    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;
};

} // namespace TSM
