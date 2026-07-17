#include "tsm_transition_history.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <iterator>
#include <limits>
#include <tuple>
#include <utility>

namespace TSM::TransitionHistory
{
namespace
{

constexpr double SecondsPerDay = 86400.0;

bool IsContinuationByte(unsigned char value) noexcept
{
    return (value & 0xC0U) == 0x80U;
}

bool IsValidTrackId(const std::string& value) noexcept
{
    if (value.empty() ||
        value.size() > History::MaximumTrackIdBytes)
    {
        return false;
    }

    std::size_t index = 0;
    while (index < value.size())
    {
        const auto lead = static_cast<unsigned char>(value[index]);
        std::uint32_t codePoint = 0;
        std::size_t length = 0;

        if (lead <= 0x7FU)
        {
            codePoint = lead;
            length = 1;
        }
        else if (lead >= 0xC2U && lead <= 0xDFU)
        {
            length = 2;
            if (index + length > value.size()) return false;
            const auto second = static_cast<unsigned char>(value[index + 1]);
            if (!IsContinuationByte(second)) return false;
            codePoint =
                (static_cast<std::uint32_t>(lead & 0x1FU) << 6U) |
                static_cast<std::uint32_t>(second & 0x3FU);
        }
        else if (lead >= 0xE0U && lead <= 0xEFU)
        {
            length = 3;
            if (index + length > value.size()) return false;
            const auto second = static_cast<unsigned char>(value[index + 1]);
            const auto third = static_cast<unsigned char>(value[index + 2]);
            if (!IsContinuationByte(second) ||
                !IsContinuationByte(third) ||
                (lead == 0xE0U && second < 0xA0U) ||
                (lead == 0xEDU && second >= 0xA0U))
            {
                return false;
            }
            codePoint =
                (static_cast<std::uint32_t>(lead & 0x0FU) << 12U) |
                (static_cast<std::uint32_t>(second & 0x3FU) << 6U) |
                static_cast<std::uint32_t>(third & 0x3FU);
        }
        else if (lead >= 0xF0U && lead <= 0xF4U)
        {
            length = 4;
            if (index + length > value.size()) return false;
            const auto second = static_cast<unsigned char>(value[index + 1]);
            const auto third = static_cast<unsigned char>(value[index + 2]);
            const auto fourth = static_cast<unsigned char>(value[index + 3]);
            if (!IsContinuationByte(second) ||
                !IsContinuationByte(third) ||
                !IsContinuationByte(fourth) ||
                (lead == 0xF0U && second < 0x90U) ||
                (lead == 0xF4U && second >= 0x90U))
            {
                return false;
            }
            codePoint =
                (static_cast<std::uint32_t>(lead & 0x07U) << 18U) |
                (static_cast<std::uint32_t>(second & 0x3FU) << 12U) |
                (static_cast<std::uint32_t>(third & 0x3FU) << 6U) |
                static_cast<std::uint32_t>(fourth & 0x3FU);
        }
        else
        {
            return false;
        }

        // JSON can represent controls, but rejecting them keeps ids suitable
        // for logs/UI and bounds escaping to at most two bytes per input byte.
        if (codePoint <= 0x1FU ||
            (codePoint >= 0x7FU && codePoint <= 0x9FU))
        {
            return false;
        }
        index += length;
    }
    return true;
}

bool IsValidConfig(const Config& config) noexcept
{
    return std::isfinite(config.dailyDecayFactor) &&
        config.dailyDecayFactor > 0.0 &&
        config.dailyDecayFactor <= 1.0 &&
        std::isfinite(config.fatigueIncrement) &&
        config.fatigueIncrement > 0.0 &&
        config.fatigueIncrement <= 1.0;
}

bool ReadUnsignedCount(
    const nlohmann::json& value,
    std::uint64_t& result) noexcept
{
    try
    {
        if (value.is_number_unsigned())
        {
            result = value.get<std::uint64_t>();
            return true;
        }
        if (value.is_number_integer())
        {
            const std::int64_t signedValue = value.get<std::int64_t>();
            if (signedValue < 0) return false;
            result = static_cast<std::uint64_t>(signedValue);
            return true;
        }
    }
    catch (...)
    {
    }
    return false;
}

bool ReadEpochSeconds(
    const nlohmann::json& value,
    std::int64_t& result) noexcept
{
    try
    {
        if (value.is_number_unsigned())
        {
            const std::uint64_t unsignedValue = value.get<std::uint64_t>();
            if (unsignedValue >
                static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
            {
                return false;
            }
            result = static_cast<std::int64_t>(unsignedValue);
            return true;
        }
        if (value.is_number_integer())
        {
            result = value.get<std::int64_t>();
            return result >= 0;
        }
    }
    catch (...)
    {
    }
    return false;
}

} // namespace

History::History(Config config)
    : m_config(SanitizeConfig(config))
{
}

std::size_t History::RemoveInvolving(const std::string& trackId) noexcept
{
    if (!IsValidTrackId(trackId)) return 0;
    std::size_t removed = 0;
    for (auto entry = m_entries.begin(); entry != m_entries.end();)
    {
        if (entry->first.fromId == trackId || entry->first.toId == trackId)
        {
            entry = m_entries.erase(entry);
            ++removed;
        }
        else
        {
            ++entry;
        }
    }
    return removed;
}

bool History::PairKey::operator<(const PairKey& other) const noexcept
{
    return std::tie(fromId, toId) < std::tie(other.fromId, other.toId);
}

Config History::SanitizeConfig(Config config) noexcept
{
    if (!std::isfinite(config.dailyDecayFactor) ||
        config.dailyDecayFactor <= 0.0 ||
        config.dailyDecayFactor > 1.0)
    {
        config.dailyDecayFactor = Config::DefaultDailyDecayFactor;
    }
    if (!std::isfinite(config.fatigueIncrement) ||
        config.fatigueIncrement <= 0.0 ||
        config.fatigueIncrement > 1.0)
    {
        config.fatigueIncrement = Config::DefaultFatigueIncrement;
    }
    return config;
}

double History::ApplyDecay(
    double fatigue,
    std::int64_t referenceEpochSeconds,
    std::int64_t epochSeconds) const noexcept
{
    const double safeFatigue = std::isfinite(fatigue)
        ? std::clamp(fatigue, 0.0, 1.0)
        : 0.0;
    if (epochSeconds <= referenceEpochSeconds) return safeFatigue;

    const double elapsedDays =
        static_cast<double>(epochSeconds - referenceEpochSeconds) /
        SecondsPerDay;
    const double multiplier = std::pow(
        m_config.dailyDecayFactor, elapsedDays);
    if (!std::isfinite(multiplier) || multiplier <= 0.0) return 0.0;
    return std::clamp(safeFatigue * multiplier, 0.0, 1.0);
}

bool History::RecordTransition(
    const std::string& fromId,
    const std::string& toId,
    std::int64_t epochSeconds)
{
    if (!IsValidTrackId(fromId) ||
        !IsValidTrackId(toId) ||
        epochSeconds < 0)
    {
        return false;
    }

    const PairKey key{fromId, toId};
    auto [entry, inserted] = m_entries.try_emplace(
        key, TransitionState{0, 0.0, epochSeconds});

    if (inserted && m_entries.size() > MaximumEntries)
    {
        auto victim = m_entries.end();
        const auto lessRelevant = [&](const auto& left, const auto& right) {
            const TransitionState& leftState = left->second;
            const TransitionState& rightState = right->second;
            const double leftFatigue = ApplyDecay(
                leftState.fatigue,
                leftState.referenceEpochSeconds,
                std::max(epochSeconds, leftState.referenceEpochSeconds));
            const double rightFatigue = ApplyDecay(
                rightState.fatigue,
                rightState.referenceEpochSeconds,
                std::max(epochSeconds, rightState.referenceEpochSeconds));
            if (leftFatigue != rightFatigue)
                return leftFatigue < rightFatigue;
            if (leftState.referenceEpochSeconds !=
                rightState.referenceEpochSeconds)
            {
                return leftState.referenceEpochSeconds <
                    rightState.referenceEpochSeconds;
            }
            if (leftState.totalCount != rightState.totalCount)
                return leftState.totalCount < rightState.totalCount;
            return left->first < right->first;
        };

        for (auto candidate = m_entries.begin();
             candidate != m_entries.end(); ++candidate)
        {
            if (candidate == entry) continue;
            if (victim == m_entries.end() ||
                lessRelevant(candidate, victim))
            {
                victim = candidate;
            }
        }
        if (victim == m_entries.end())
        {
            m_entries.erase(entry);
            return false;
        }
        m_entries.erase(victim);
    }

    TransitionState& state = entry->second;
    const std::int64_t effectiveTimestamp =
        std::max(epochSeconds, state.referenceEpochSeconds);
    state.fatigue = ApplyDecay(
        state.fatigue, state.referenceEpochSeconds, effectiveTimestamp);
    state.referenceEpochSeconds = effectiveTimestamp;

    if (state.totalCount < (std::numeric_limits<std::uint64_t>::max)())
        ++state.totalCount;
    state.fatigue +=
        m_config.fatigueIncrement * (1.0 - state.fatigue);
    state.fatigue = std::clamp(state.fatigue, 0.0, 1.0);
    return true;
}

void History::MergeFrom(const History& source)
{
    if (&source == this || source.m_entries.empty()) return;

    for (const auto& [key, sourceState] : source.m_entries)
    {
        auto [entry, inserted] = m_entries.try_emplace(key, sourceState);
        if (inserted) continue;

        TransitionState& destinationState = entry->second;
        const std::int64_t referenceEpochSeconds = std::max(
            destinationState.referenceEpochSeconds,
            sourceState.referenceEpochSeconds);
        const double destinationFatigue = ApplyDecay(
            destinationState.fatigue,
            destinationState.referenceEpochSeconds,
            referenceEpochSeconds);
        const double sourceFatigue = source.ApplyDecay(
            sourceState.fatigue,
            sourceState.referenceEpochSeconds,
            referenceEpochSeconds);
        destinationState.fatigue = std::clamp(
            1.0 - (1.0 - destinationFatigue) * (1.0 - sourceFatigue),
            0.0,
            1.0);
        destinationState.referenceEpochSeconds = referenceEpochSeconds;
        const std::uint64_t remaining =
            (std::numeric_limits<std::uint64_t>::max)() -
            destinationState.totalCount;
        destinationState.totalCount += std::min(
            remaining, sourceState.totalCount);
    }

    while (m_entries.size() > MaximumEntries)
    {
        std::int64_t evaluationEpochSeconds = 0;
        for (const auto& [key, state] : m_entries)
        {
            (void)key;
            evaluationEpochSeconds = std::max(
                evaluationEpochSeconds, state.referenceEpochSeconds);
        }

        auto victim = m_entries.begin();
        for (auto candidate = std::next(m_entries.begin());
             candidate != m_entries.end(); ++candidate)
        {
            const TransitionState& candidateState = candidate->second;
            const TransitionState& victimState = victim->second;
            const double candidateFatigue = ApplyDecay(
                candidateState.fatigue,
                candidateState.referenceEpochSeconds,
                evaluationEpochSeconds);
            const double victimFatigue = ApplyDecay(
                victimState.fatigue,
                victimState.referenceEpochSeconds,
                evaluationEpochSeconds);
            if (candidateFatigue < victimFatigue ||
                (candidateFatigue == victimFatigue &&
                 (candidateState.referenceEpochSeconds <
                      victimState.referenceEpochSeconds ||
                  (candidateState.referenceEpochSeconds ==
                       victimState.referenceEpochSeconds &&
                   (candidateState.totalCount < victimState.totalCount ||
                    (candidateState.totalCount == victimState.totalCount &&
                     candidate->first < victim->first))))))
            {
                victim = candidate;
            }
        }
        m_entries.erase(victim);
    }
}

std::optional<TransitionState> History::GetState(
    const std::string& fromId,
    const std::string& toId,
    std::int64_t epochSeconds) const
{
    if (!IsValidTrackId(fromId) ||
        !IsValidTrackId(toId) ||
        epochSeconds < 0)
    {
        return std::nullopt;
    }
    const auto entry = m_entries.find(PairKey{fromId, toId});
    if (entry == m_entries.end()) return std::nullopt;

    TransitionState state = entry->second;
    state.referenceEpochSeconds = std::max(
        epochSeconds, state.referenceEpochSeconds);
    state.fatigue = ApplyDecay(
        entry->second.fatigue,
        entry->second.referenceEpochSeconds,
        state.referenceEpochSeconds);
    return state;
}

double History::GetFatigue(
    const std::string& fromId,
    const std::string& toId,
    std::int64_t epochSeconds) const
{
    const std::optional<TransitionState> state =
        GetState(fromId, toId, epochSeconds);
    return state ? state->fatigue : 0.0;
}

double History::GetDiversityScore(
    const std::string& fromId,
    const std::string& toId,
    std::int64_t epochSeconds) const
{
    return std::clamp(
        1.0 - GetFatigue(fromId, toId, epochSeconds), 0.0, 1.0);
}

nlohmann::json History::ExportState() const
{
    nlohmann::json transitions = nlohmann::json::array();
    for (const auto& [key, state] : m_entries)
    {
        transitions.push_back({
            {"fromId", key.fromId},
            {"toId", key.toId},
            {"totalCount", state.totalCount},
            {"fatigue", state.fatigue},
            {"referenceEpochSeconds", state.referenceEpochSeconds}
        });
    }

    return {
        {"schemaVersion", CurrentSchemaVersion},
        {"config", {
            {"dailyDecayFactor", m_config.dailyDecayFactor},
            {"fatigueIncrement", m_config.fatigueIncrement}
        }},
        {"transitions", std::move(transitions)}
    };
}

bool History::ImportState(
    const nlohmann::json& document,
    std::string& errorMessage)
{
    errorMessage.clear();
    try
    {
        if (!document.is_object())
        {
            errorMessage = "Transition history must be a JSON object.";
            return false;
        }
        std::uint64_t schemaVersion = 0;
        if (!document.contains("schemaVersion") ||
            !ReadUnsignedCount(document["schemaVersion"], schemaVersion) ||
            schemaVersion != static_cast<std::uint64_t>(CurrentSchemaVersion))
        {
            errorMessage = "Unsupported transition history schema version.";
            return false;
        }
        if (document.size() != 3)
        {
            errorMessage = "Transition history contains unexpected fields.";
            return false;
        }
        if (!document.contains("config") || !document["config"].is_object())
        {
            errorMessage = "Transition history config is missing or invalid.";
            return false;
        }

        const nlohmann::json& configJson = document["config"];
        if (configJson.size() != 2 ||
            !configJson.contains("dailyDecayFactor") ||
            !configJson["dailyDecayFactor"].is_number() ||
            !configJson.contains("fatigueIncrement") ||
            !configJson["fatigueIncrement"].is_number())
        {
            errorMessage = "Transition history config values are missing.";
            return false;
        }
        Config importedConfig;
        importedConfig.dailyDecayFactor =
            configJson["dailyDecayFactor"].get<double>();
        importedConfig.fatigueIncrement =
            configJson["fatigueIncrement"].get<double>();
        if (!IsValidConfig(importedConfig))
        {
            errorMessage = "Transition history config values are invalid.";
            return false;
        }

        if (!document.contains("transitions") ||
            !document["transitions"].is_array())
        {
            errorMessage = "Transition history entries must be an array.";
            return false;
        }

        const nlohmann::json& transitionsJson = document["transitions"];
        if (transitionsJson.size() > MaximumEntries)
        {
            errorMessage =
                "Transition history exceeds the maximum entry count.";
            return false;
        }

        std::map<PairKey, TransitionState> importedEntries;
        for (const nlohmann::json& value : transitionsJson)
        {
            if (!value.is_object() || value.size() != 5 ||
                !value.contains("fromId") || !value["fromId"].is_string() ||
                !value.contains("toId") || !value["toId"].is_string() ||
                !value.contains("totalCount") ||
                !value.contains("fatigue") || !value["fatigue"].is_number() ||
                !value.contains("referenceEpochSeconds"))
            {
                errorMessage = "A transition history entry is malformed.";
                return false;
            }

            const std::string& fromId =
                value["fromId"].get_ref<const std::string&>();
            const std::string& toId =
                value["toId"].get_ref<const std::string&>();
            PairKey key{fromId, toId};
            TransitionState state;
            state.fatigue = value["fatigue"].get<double>();
            if (!IsValidTrackId(key.fromId) ||
                !IsValidTrackId(key.toId) ||
                !ReadUnsignedCount(value["totalCount"], state.totalCount) ||
                state.totalCount == 0 ||
                !std::isfinite(state.fatigue) ||
                state.fatigue < 0.0 || state.fatigue > 1.0 ||
                !ReadEpochSeconds(
                    value["referenceEpochSeconds"],
                    state.referenceEpochSeconds))
            {
                errorMessage = "A transition history entry contains invalid values.";
                return false;
            }
            if (state.fatigue == 0.0) state.fatigue = 0.0;

            const auto [entry, inserted] = importedEntries.emplace(
                std::move(key), state);
            (void)entry;
            if (!inserted)
            {
                errorMessage = "Transition history contains a duplicate pair.";
                return false;
            }
        }

        m_config = importedConfig;
        m_entries = std::move(importedEntries);
        return true;
    }
    catch (const std::exception& error)
    {
        errorMessage = std::string("Invalid transition history: ") + error.what();
        return false;
    }
    catch (...)
    {
        errorMessage = "Invalid transition history.";
        return false;
    }
}

} // namespace TSM::TransitionHistory
