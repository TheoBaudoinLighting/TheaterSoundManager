// tsm_audio_manager.h
#pragma once

#include <fmod.hpp>
#include "tsm_music_analysis.h"
#include "tsm_playback_memory_store.h"
#include "tsm_transition_history.h"
#include <string>
#include <map>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace TSM 
{

class AudioManager 
{
public:
    enum class SoundKind
    {
        SoundEffect,
        Music,
        Announcement,
        Emergency
    };

    enum class LoudnessStatus
    {
        NotQueued,
        Queued,
        Analyzing,
        Ready,
        Failed
    };

    struct ListeningFatigueData
    {
        MusicAnalysis::FileIdentity identity;
        ListeningHeatmap::State state;
        // Writes are authoritative only after the current store answered Hit
        // or Miss. While a read is temporarily unavailable, mutations are
        // accumulated separately so a later retry can merge them with the
        // durable state instead of overwriting data that was never read.
        ListeningHeatmap::State pendingMutations;
        bool readConclusive = false;
        bool hasPendingMutations = false;
        bool dirty = false;
        // Mutations are versioned on the control thread. An asynchronous
        // snapshot may clear dirty only when this value is still unchanged.
        std::uint64_t version = 0;
        // normalized_path is the SQLite primary key. When the file at a path
        // changes while an older FMOD sound is still alive, only the most
        // recently loaded generation is eligible for persistence.
        std::uint64_t pathGeneration = 0;
    };

    struct SoundData
    {
        FMOD::Sound* sound = nullptr;
        std::vector<FMOD::Channel*> channels;
        std::string filePath;
        SoundKind kind = SoundKind::SoundEffect;
        bool isMusic = false;
        LoudnessStatus loudnessStatus = LoudnessStatus::NotQueued;
        float integratedLufs = 0.0f;
        float truePeakDb = 0.0f;
        float normalizationGainDb = 0.0f;
        float normalizationGainLinear = 1.0f;
        bool musicAnalysisReady = false;
        MusicAnalysis::TrackAnalysis musicAnalysis;
        // Aliases that reference the same physical audio file share this
        // object. SQLite is keyed by file identity, so RAM must use the same
        // ownership model to prevent last-writer-wins heatmap loss.
        std::shared_ptr<ListeningFatigueData> listeningFatigue;
    };

    struct LoudnessDiagnostic
    {
        std::string soundName;
        std::string filePath;
        LoudnessStatus status = LoudnessStatus::NotQueued;
        float integratedLufs = 0.0f;
        float truePeakDb = 0.0f;
        float gainDb = 0.0f;
        int queuePosition = -1;
        bool musicAnalysisReady = false;
    };

    struct PlaybackMemoryDiagnostic
    {
        std::string soundName;
        std::size_t bucketCount = 0;
        double coverage = 0.0;
        double meanFatigue = 0.0;
        double maxFatigue = 0.0;
    };

    static AudioManager& GetInstance() 
    {
        static AudioManager instance;
        return instance;
    }

    bool LoadSound(const std::string& soundName, const std::string& filePath,
                   bool isStream = false, SoundKind kind = SoundKind::SoundEffect);
    bool UnloadSound(const std::string& soundName);
    bool LoadAnnouncement(const std::string& announcementId, const std::string& filePath);
    bool LoadEmergencySound(const std::string& soundName, const std::string& filePath);
    FMOD::Channel* PlaySound(const std::string& soundName, bool loop = false, float volume = 1.0f, float pitch = 1.0f);
    FMOD::Channel* PlaySoundWithFadeIn(const std::string& soundName, bool loop = false, float volume = 1.0f, float pitch = 1.0f);
    FMOD::Channel* PlayMusic(const std::string& soundName, bool loop = false, float volume = 1.0f, float pitch = 1.0f);
    FMOD::Channel* PlayMusicWithFadeIn(const std::string& soundName, bool loop = false, float volume = 1.0f, float pitch = 1.0f);
    void StopSound(const std::string& soundName);
    void StopSoundWithFadeOut(const std::string& soundName);
    void StopChannelWithFadeOut(FMOD::Channel* channel);
    bool IsChannelFading(FMOD::Channel* channel) const;
    void StopAllSounds();
    void StopAllNonEmergencyImmediately();
    void StopAllSoundsWithFadeOut();
    FMOD::Sound* GetSound(const std::string& soundName);
    const std::map<std::string, SoundData>& GetAllSounds() const { return m_sounds; }
    void SetVolume(const std::string& soundName, float volume);
    void SetPitch(const std::string& soundName, float pitch);
    void SetChannelVolume(FMOD::Channel* channel, float volume);
    void ReconcileMusicChannelVolume(FMOD::Channel* channel);
    void SetChannelPitch(FMOD::Channel* channel, float pitch);
    void ApplyMixerVolumes(float master, float music, float announcement,
                           float sfx, float effectiveDuck);
    void Update(float deltaTime);
    void RefreshChannelState();
    void SetNormalPlaybackBlocked(bool blocked);
    bool IsNormalPlaybackBlocked() const { return m_normalPlaybackBlocked; }
    FMOD::Channel* GetLastChannelOfSound(const std::string& soundName);
    void QueueLoudnessAnalysis(const std::string& soundName);
    float GetNormalizationGainForChannel(FMOD::Channel* channel) const;
    std::vector<LoudnessDiagnostic> GetLoudnessDiagnostics() const;
    const MusicAnalysis::TrackAnalysis* GetMusicAnalysis(
        const std::string& soundName) const;
    const ListeningHeatmap::State* GetListeningFatigue(
        const std::string& soundName) const;
    bool RecordListeningCoverage(
        const std::string& soundName,
        double startSeconds,
        double endSeconds,
        double audibilityWeight,
        std::int64_t epochSeconds);
    double GetTransitionDiversityScore(
        const std::string& fromSoundName,
        const std::string& toSoundName,
        std::int64_t epochSeconds) const;
    bool RecordMusicTransition(
        const std::string& fromSoundName,
        const std::string& toSoundName,
        std::int64_t epochSeconds);
    std::vector<PlaybackMemoryDiagnostic> GetPlaybackMemoryDiagnostics(
        std::int64_t epochSeconds) const;
    std::optional<TransitionHistory::TransitionState> GetTransitionMemory(
        const std::string& fromSoundName,
        const std::string& toSoundName,
        std::int64_t epochSeconds) const;
    bool ClearPlaybackMemory(
        const std::optional<std::string>& soundName,
        std::string& errorMessage);
    bool FlushPlaybackMemory(std::string& errorMessage);
    bool IsPlaybackMemoryPersistent() const
    {
        return m_playbackMemoryStoreAvailable &&
            m_playbackMemoryPersistenceHealthy;
    }
    const std::string& GetPlaybackMemoryError() const
    {
        return m_playbackMemoryLastError;
    }
    const std::string& GetPlaybackMemoryPath() const
    {
        return m_playbackMemoryDatabasePath;
    }
    const TransitionHistory::History& GetTransitionHistory() const
    {
        return m_transitionHistory;
    }
    static const char* SoundKindToString(SoundKind kind);
    static const char* LoudnessStatusToString(LoudnessStatus status);
    void SetLoudnessTarget(float targetLufs);
    float GetLoudnessTarget() const { return m_loudnessTargetLufs; }
    void ConfigureLoudness(const std::string& cachePath, float targetLufs);
    bool ClearLoudnessCache(bool& removed, std::string& errorMessage);
    const std::string& GetLoudnessCachePath() const { return m_loudnessCachePath; }
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
    FMOD::ChannelGroup* m_announcementChannelGroup = nullptr;
    FMOD::ChannelGroup* m_sfxChannelGroup = nullptr;
    FMOD::ChannelGroup* m_emergencyChannelGroup = nullptr;
    FMOD::DSP* m_musicLimiter = nullptr;
    bool m_normalPlaybackBlocked = false;

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
        bool musicAnalysisSuccess = false;
        MusicAnalysis::TrackAnalysis musicAnalysis;
    };

    std::thread m_loudnessThread;
    mutable std::mutex m_loudnessMutex;
    std::condition_variable m_loudnessCondition;
    std::deque<LoudnessTask> m_loudnessTasks;
    std::deque<LoudnessResult> m_loudnessResults;
    std::string m_activeLoudnessSound;
    std::atomic<bool> m_stopLoudnessThread{false};
    bool m_loudnessThreadStarted = false;
    std::atomic<bool> m_loudnessWorkerExited{false};
    float m_loudnessTargetLufs = -16.0f;
    std::string m_loudnessCachePath = "loudness_cache.json";
    std::string m_musicAnalysisDatabasePath = "music_analysis.sqlite3";
    std::string m_playbackMemoryDatabasePath = "playback_memory.sqlite3";
    std::unique_ptr<PlaybackMemory::Store> m_playbackMemoryStore;
    bool m_playbackMemoryStoreAvailable = false;
    bool m_playbackMemoryPersistenceHealthy = false;
    std::string m_playbackMemoryLastError;
    float m_playbackMemoryFlushTimer = 0.0f;
    TransitionHistory::History m_transitionHistory;
    TransitionHistory::History m_pendingTransitionHistory;
    bool m_transitionHistoryReadConclusive = false;
    bool m_transitionHistoryDirty = false;
    std::uint64_t m_transitionHistoryVersion = 0;

    struct PlaybackMemoryHeatmapSnapshot
    {
        MusicAnalysis::FileIdentity identity;
        ListeningHeatmap::State state;
        std::uint64_t version = 0;
        std::uint64_t pathGeneration = 0;
    };

    struct PlaybackMemoryFlushSnapshot
    {
        std::vector<PlaybackMemoryHeatmapSnapshot> heatmaps;
        bool includesTransitionHistory = false;
        TransitionHistory::History transitionHistory{
            TransitionHistory::Config{}};
        std::uint64_t transitionHistoryVersion = 0;
    };

    struct PlaybackMemoryHeatmapTarget
    {
        std::shared_ptr<ListeningFatigueData> data;
        std::uint64_t version = 0;
        std::uint64_t pathGeneration = 0;
    };

    struct PlaybackMemoryFlushMetadata
    {
        std::vector<PlaybackMemoryHeatmapTarget> heatmaps;
        bool includesTransitionHistory = false;
        std::uint64_t transitionHistoryVersion = 0;
    };

    struct PlaybackMemoryFlushResult
    {
        std::vector<unsigned char> heatmapSucceeded;
        bool transitionHistorySucceeded = false;
        bool allSucceeded = true;
        bool performedWrite = false;
        std::string errorMessage;
    };

    std::future<PlaybackMemoryFlushResult> m_playbackMemoryFlushFuture;
    std::optional<PlaybackMemoryFlushMetadata>
        m_activePlaybackMemoryFlushMetadata;
    std::vector<std::shared_ptr<ListeningFatigueData>>
        m_retainedPlaybackMemory;
    std::unordered_map<
        std::string, std::weak_ptr<ListeningFatigueData>>
        m_currentPlaybackMemoryByPath;
    std::uint64_t m_nextPlaybackMemoryPathGeneration = 0;
    bool m_playbackMemoryStructuralFailure = false;
    bool m_playbackMemoryWriteFailure = false;
    std::string m_playbackMemoryStructuralError;
    std::string m_playbackMemoryWriteError;

    bool LoadSoundInternal(const std::string& soundName, const std::string& filePath,
                           bool isStream, SoundKind kind, bool replaceExisting);
    FMOD::Channel* PlaySoundInternal(const std::string& soundName, bool loop, float volume, float pitch, bool normalizeMusic);
    bool EnsureMusicProcessing();
    bool EnsureChannelGroup(FMOD::ChannelGroup*& group, const char* name);
    void StopLoudnessWorker();
    void StartLoudnessWorker();
    void LoudnessWorkerMain();
    void ApplyLoudnessResults();
    void StartChannelFade(FMOD::Channel* channel, float targetVolume, bool stopWhenComplete);
    void RetargetChannelFade(FMOD::Channel* channel, float targetVolume);
    void AdjustChannelNormalizationGain(FMOD::Channel* channel, float gainRatio);
    void CancelChannelFade(FMOD::Channel* channel);
    void PruneStoppedChannels(SoundData& data);
    static bool IsChannelPlayingSound(FMOD::Channel* channel, FMOD::Sound* expectedSound);
    static bool ApplyPitch(FMOD::Channel* channel, float pitch);
    std::string GetPlaybackMemoryKey(const std::string& soundName) const;
    void LoadPlaybackMemoryForSound(
        SoundData& data,
        bool resetOnMiss = false);
    void ResolveTransitionHistoryRead();
    void RetryPendingPlaybackMemoryReads();
    bool HasUnresolvedPlaybackMemoryMutations() const;
    void RegisterPlaybackMemoryState(
        const std::shared_ptr<ListeningFatigueData>& data);
    bool IsCurrentPlaybackMemoryState(
        const std::shared_ptr<ListeningFatigueData>& data) const;
    void RetainPlaybackMemoryState(
        const std::shared_ptr<ListeningFatigueData>& data);
    void PruneRetainedPlaybackMemory();
    void CapturePlaybackMemoryFlush(
        PlaybackMemoryFlushSnapshot& snapshot,
        PlaybackMemoryFlushMetadata& metadata);
    static PlaybackMemoryFlushResult ExecutePlaybackMemoryFlush(
        PlaybackMemory::Store* store,
        PlaybackMemoryFlushSnapshot snapshot);
    bool ApplyPlaybackMemoryFlushResult(
        PlaybackMemoryFlushMetadata metadata,
        const PlaybackMemoryFlushResult& result,
        std::string& errorMessage);
    bool StartPlaybackMemoryFlush(std::string& errorMessage);
    bool CompletePlaybackMemoryFlush(
        bool wait,
        bool& completed,
        std::string& errorMessage);
    void MarkPlaybackMemoryFailure(
        const std::string& errorMessage,
        bool retryableWriteFailure = false);
    void MarkPlaybackMemoryWriteSuccess();
    void ResetPlaybackMemoryHealth();
    void RefreshPlaybackMemoryHealthDiagnostic();
public:
    AudioManager() = default;
    ~AudioManager();

    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;
};

} // namespace TSM
