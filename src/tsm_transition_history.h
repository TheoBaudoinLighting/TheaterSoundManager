#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace TSM::TransitionHistory
{

struct Config
{
    static constexpr double DefaultDailyDecayFactor = 0.95;
    static constexpr double DefaultFatigueIncrement = 0.25;

    // Continuous decay is calibrated so one elapsed day multiplies fatigue
    // by this value.
    double dailyDecayFactor = DefaultDailyDecayFactor;
    // Each occurrence consumes this fraction of the remaining headroom.
    double fatigueIncrement = DefaultFatigueIncrement;
};

struct TransitionState
{
    std::uint64_t totalCount = 0;
    double fatigue = 0.0;
    std::int64_t referenceEpochSeconds = 0;
};

// Directional transition history. A -> B and B -> A are deliberately stored
// as independent observations.
class History final
{
public:
    static constexpr int CurrentSchemaVersion = 1;
    // These limits keep the default compact JSON representation below 5 MiB,
    // comfortably inside the playback store's independent 16 MiB guard.
    // Track ids are measured in UTF-8 bytes, not Unicode code points.
    static constexpr std::size_t MaximumEntries = 2048;
    static constexpr std::size_t MaximumTrackIdBytes = 512;
    static constexpr std::size_t MaximumCompactJsonBytes =
        5U * 1024U * 1024U;
    static_assert(
        MaximumEntries * (4U * MaximumTrackIdBytes + 256U) + 512U <=
            MaximumCompactJsonBytes,
        "Transition history limits exceed the compact JSON envelope.");

    explicit History(Config config = {});

    const Config& GetConfig() const noexcept { return m_config; }
    std::size_t Size() const noexcept { return m_entries.size(); }
    void Clear() noexcept { m_entries.clear(); }
    std::size_t RemoveInvolving(const std::string& trackId) noexcept;

    // Returns false for an invalid id or a negative timestamp. Ids must be
    // bounded, valid UTF-8 without control characters. Clock rollback is
    // tolerated by keeping the most recent reference timestamp. When a new
    // pair arrives at capacity, the least relevant existing pair is evicted.
    bool RecordTransition(
        const std::string& fromId,
        const std::string& toId,
        std::int64_t epochSeconds);

    // Combines another bounded history without dropping either side's count.
    // Fatigue values are decayed to a common reference time and composed as
    // saturating independent contributions.
    void MergeFrom(const History& source);

    // The returned state is a decayed snapshot at epochSeconds; querying does
    // not mutate the durable reference state.
    std::optional<TransitionState> GetState(
        const std::string& fromId,
        const std::string& toId,
        std::int64_t epochSeconds) const;
    double GetFatigue(
        const std::string& fromId,
        const std::string& toId,
        std::int64_t epochSeconds) const;
    double GetDiversityScore(
        const std::string& fromId,
        const std::string& toId,
        std::int64_t epochSeconds) const;

    // Export order is stable (from id, then to id) so identical histories
    // produce byte-for-byte identical JSON dumps.
    nlohmann::json ExportState() const;
    // Import is strict and transactional: malformed data never replaces the
    // currently committed history.
    bool ImportState(
        const nlohmann::json& document,
        std::string& errorMessage);

private:
    struct PairKey
    {
        std::string fromId;
        std::string toId;

        bool operator<(const PairKey& other) const noexcept;
    };

    static Config SanitizeConfig(Config config) noexcept;
    double ApplyDecay(
        double fatigue,
        std::int64_t referenceEpochSeconds,
        std::int64_t epochSeconds) const noexcept;

    Config m_config;
    std::map<PairKey, TransitionState> m_entries;
};

} // namespace TSM::TransitionHistory
