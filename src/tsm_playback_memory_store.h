#pragma once

#include "tsm_listening_heatmap.h"
#include "tsm_music_analysis_store.h"
#include "tsm_transition_history.h"

#include <filesystem>
#include <memory>
#include <string>

namespace TSM::PlaybackMemory
{

using StoreLookupStatus = MusicAnalysis::StoreLookupStatus;

// Durable playback-learning state. The expected production filename is
// playback_memory.sqlite3 inside the application's state directory.
class Store final
{
public:
    static constexpr int CurrentSchemaVersion = 1;
    static constexpr const char* DefaultFileName = "playback_memory.sqlite3";

    explicit Store(std::filesystem::path databasePath);
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;

    bool Initialize(std::string& errorMessage) noexcept;

    // Load methods replace their destination only on Hit. Miss and
    // Unavailable are non-fatal fallbacks and preserve the caller's state.
    StoreLookupStatus LoadHeatmap(
        const MusicAnalysis::FileIdentity& identity,
        ListeningHeatmap::State& state,
        std::string& errorMessage) noexcept;
    bool SaveHeatmap(
        const MusicAnalysis::FileIdentity& identity,
        const ListeningHeatmap::State& state,
        std::string& errorMessage) noexcept;

    StoreLookupStatus LoadTransitionHistory(
        TransitionHistory::History& history,
        std::string& errorMessage) noexcept;
    bool SaveTransitionHistory(
        const TransitionHistory::History& history,
        std::string& errorMessage) noexcept;

    // ClearHeatmap affects one normalized track only. The other two methods
    // make transition-only and full reset intent explicit.
    bool ClearHeatmap(
        const std::string& normalizedTrackPath,
        std::string& errorMessage) noexcept;
    // Atomically removes one track heatmap and commits the corresponding
    // transition-history snapshot. Callers never observe a half-reset.
    bool ClearTrackMemory(
        const std::string& normalizedTrackPath,
        const TransitionHistory::History& remainingHistory,
        std::string& errorMessage) noexcept;
    bool ClearTransitionHistory(std::string& errorMessage) noexcept;
    bool ClearAll(std::string& errorMessage) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace TSM::PlaybackMemory
