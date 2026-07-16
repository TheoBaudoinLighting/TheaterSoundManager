#include "tsm_safety.h"

#include "tsm_atomic_file.h"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

namespace TSM
{
namespace
{

constexpr int SafetyStateSchemaVersion = 1;
constexpr std::size_t MaximumPersistedCauses = 32;
constexpr std::size_t MaximumRetiredSessions = 32;

bool HasExactKeys(
    const nlohmann::json& value,
    std::initializer_list<const char*> expectedKeys)
{
    if (!value.is_object() || value.size() != expectedKeys.size()) return false;
    for (const char* key : expectedKeys)
    {
        if (!value.contains(key)) return false;
    }
    return true;
}

bool ReadUnsigned64(const nlohmann::json& value, std::uint64_t& result)
{
    if (value.is_number_unsigned())
    {
        result = value.get<std::uint64_t>();
        return true;
    }
    if (!value.is_number_integer()) return false;
    const std::int64_t signedValue = value.get<std::int64_t>();
    if (signedValue < 0) return false;
    result = static_cast<std::uint64_t>(signedValue);
    return true;
}

bool IsBoundedText(const std::string& value, std::size_t maximumLength, bool allowEmpty)
{
    if ((!allowEmpty && value.empty()) || value.size() > maximumLength) return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20 || character == 0x7f;
    });
}

struct ParsedSafetyState
{
    std::uint64_t generation = 0;
    bool baselineEstablished = false;
    std::optional<SafetyIncident> incident;
    std::string lastResetIncidentId;
    std::string lastResetOperator;
    std::string lastResetReason;
};

bool ParsePersistedState(
    const nlohmann::json& document,
    ParsedSafetyState& state,
    std::string& errorMessage)
{
    if (!HasExactKeys(
            document,
            {"schemaVersion", "generation", "baselineEstablished", "latched",
             "incident", "lastReset"}))
    {
        errorMessage = "Safety state must contain exactly the versioned state fields.";
        return false;
    }
    if (!document["schemaVersion"].is_number_integer() ||
        document["schemaVersion"].get<int>() != SafetyStateSchemaVersion)
    {
        errorMessage = "Unsupported safety state schema version.";
        return false;
    }
    if (!ReadUnsigned64(document["generation"], state.generation))
    {
        errorMessage = "Safety state generation must be an unsigned integer.";
        return false;
    }
    if (!document["baselineEstablished"].is_boolean() ||
        !document["latched"].is_boolean())
    {
        errorMessage = "Safety state flags must be booleans.";
        return false;
    }
    state.baselineEstablished = document["baselineEstablished"].get<bool>();
    const bool latched = document["latched"].get<bool>();

    const auto& incident = document["incident"];
    if (latched)
    {
        if (!HasExactKeys(incident, {"id", "causes"}) ||
            !incident["id"].is_string() || !incident["causes"].is_array())
        {
            errorMessage = "A latched safety state requires a valid incident.";
            return false;
        }
        SafetyIncident parsedIncident;
        parsedIncident.id = incident["id"].get<std::string>();
        if (!IsBoundedText(parsedIncident.id, 128, false) ||
            incident["causes"].empty() ||
            incident["causes"].size() > MaximumPersistedCauses)
        {
            errorMessage = "Safety incident ID or cause count is invalid.";
            return false;
        }
        std::set<std::string> uniqueCauses;
        for (const auto& causeValue : incident["causes"])
        {
            if (!causeValue.is_string())
            {
                errorMessage = "Safety incident causes must be strings.";
                return false;
            }
            std::string cause = causeValue.get<std::string>();
            if (!IsBoundedText(cause, 256, false) || !uniqueCauses.insert(cause).second)
            {
                errorMessage = "Safety incident causes must be unique bounded strings.";
                return false;
            }
            parsedIncident.causes.push_back(std::move(cause));
        }
        state.incident = std::move(parsedIncident);
        if (!state.baselineEstablished)
        {
            errorMessage = "A latched safety state must establish a baseline.";
            return false;
        }
    }
    else if (!incident.is_null())
    {
        errorMessage = "An unlatched safety state cannot contain an incident.";
        return false;
    }

    const auto& lastReset = document["lastReset"];
    if (!lastReset.is_null())
    {
        if (!HasExactKeys(lastReset, {"incidentId", "operatorId", "reason"}) ||
            !lastReset["incidentId"].is_string() ||
            !lastReset["operatorId"].is_string() ||
            !lastReset["reason"].is_string())
        {
            errorMessage = "Safety lastReset must be null or a complete reset record.";
            return false;
        }
        state.lastResetIncidentId = lastReset["incidentId"].get<std::string>();
        state.lastResetOperator = lastReset["operatorId"].get<std::string>();
        state.lastResetReason = lastReset["reason"].get<std::string>();
        if (!IsBoundedText(state.lastResetIncidentId, 128, false) ||
            !IsBoundedText(state.lastResetOperator, 128, false) ||
            !IsBoundedText(state.lastResetReason, 512, false))
        {
            errorMessage = "Safety reset record contains invalid text.";
            return false;
        }
    }
    return true;
}

std::string CauseWithSource(const std::string& cause, const std::string& source)
{
    return source.empty() ? cause : cause + "@" + source;
}

} // namespace

SafetyResult SafetyResult::Success(std::string code)
{
    SafetyResult result;
    result.ok = true;
    result.code = std::move(code);
    return result;
}

SafetyResult SafetyResult::Failure(std::string code, std::string message)
{
    SafetyResult result;
    result.code = std::move(code);
    result.message = std::move(message);
    return result;
}

const char* SafetyModeToString(SafetyMode mode)
{
    switch (mode)
    {
        case SafetyMode::Inhibited: return "inhibited";
        case SafetyMode::Ready: return "ready";
        case SafetyMode::Latched: return "latched";
    }
    return "inhibited";
}

const char* InterlockSignalToString(InterlockSignal signal)
{
    switch (signal)
    {
        case InterlockSignal::Safe: return "safe";
        case InterlockSignal::Alarm: return "alarm";
        case InterlockSignal::Fault: return "fault";
    }
    return "fault";
}

SafetyResult SafetyController::Initialize(
    SafetyConfig config,
    std::filesystem::path statePath,
    TimePoint /*now*/)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_config = std::move(config);
    m_statePath = std::move(statePath);
    m_initialized = true;
    m_baselineEstablished = false;
    m_generation = 0;
    m_incident.reset();
    m_inhibitReason = "startup_validation";
    m_persistenceError.clear();
    m_hasHeartbeat = false;
    m_heartbeatSource.clear();
    m_heartbeatSession.clear();
    m_heartbeatSequence = 0;
    m_interlockSignal = InterlockSignal::Fault;
    m_safeSince.reset();
    m_retiredHeartbeatSessions.clear();
    m_lastResetIncidentId.clear();
    m_lastResetOperator.clear();
    m_lastResetReason.clear();
    SetModeLocked(SafetyMode::Inhibited);

    constexpr auto MaximumDuration = std::chrono::hours(24);
    const bool invalidConfiguration = m_config.enabled &&
       (
        m_config.heartbeatTimeout <= std::chrono::milliseconds::zero() ||
        m_config.heartbeatTimeout > MaximumDuration ||
        m_config.healthyBeforeReady < std::chrono::milliseconds::zero() ||
        m_config.healthyBeforeReady > MaximumDuration ||
        m_config.healthyBeforeReset < std::chrono::milliseconds::zero() ||
        m_config.healthyBeforeReset > MaximumDuration ||
        (!m_config.expectedHeartbeatSource.empty() &&
         !IsValidIdentifier(m_config.expectedHeartbeatSource, 128)));
    if (invalidConfiguration)
    {
        m_inhibitReason = "invalid_configuration";
        const SafetyResult trip = TripLocked("safety_config_invalid", {});
        SafetyResult failure = SafetyResult::Failure(
            "invalid_safety_configuration",
            "Safety timing or expected heartbeat source is invalid.");
        failure.tripped = true;
        if (!trip.ok) failure.message += " " + trip.message;
        return failure;
    }

    if (!m_config.enabled)
    {
        m_baselineEstablished = true;
        m_inhibitReason.clear();
        SetModeLocked(SafetyMode::Ready);
        return SafetyResult::Success("safety_disabled");
    }
    if (m_statePath.empty())
    {
        const SafetyResult trip = TripLocked("state_path_invalid", {});
        SafetyResult failure = SafetyResult::Failure(
            "invalid_state_path", "Safety state path is empty.");
        failure.tripped = true;
        if (!trip.ok) failure.message += " " + trip.message;
        return failure;
    }

    const JsonFileReadResult readResult = ReadJsonFileStrict(m_statePath);
    if (readResult.status == JsonFileReadStatus::Missing)
    {
        if (!m_config.heartbeatRequired)
        {
            m_baselineEstablished = true;
            std::string persistenceError;
            if (!PersistLocked(persistenceError))
            {
                m_baselineEstablished = false;
                m_persistenceError = persistenceError;
                m_inhibitReason = "state_persistence_failed";
                return SafetyResult::Failure("state_persistence_failed", persistenceError);
            }
            m_inhibitReason.clear();
            SetModeLocked(SafetyMode::Ready);
            return SafetyResult::Success("baseline_created");
        }
        m_inhibitReason = "state_missing";
        return SafetyResult::Success("state_missing");
    }
    if (!readResult.IsSuccess())
    {
        const std::string cause = readResult.status == JsonFileReadStatus::Invalid
            ? "state_corrupt"
            : "state_io_error";
        const SafetyResult trip = TripLocked(cause, {});
        SafetyResult failure = SafetyResult::Failure(cause, readResult.error);
        failure.tripped = true;
        if (!trip.ok && failure.message.empty()) failure.message = trip.message;
        return failure;
    }

    ParsedSafetyState parsed;
    std::string parseError;
    if (!ParsePersistedState(readResult.document, parsed, parseError))
    {
        const SafetyResult trip = TripLocked("state_corrupt", {});
        SafetyResult failure = SafetyResult::Failure("state_corrupt", parseError);
        failure.tripped = true;
        if (!trip.ok && failure.message.empty()) failure.message = trip.message;
        return failure;
    }

    m_generation = parsed.generation;
    m_baselineEstablished = parsed.baselineEstablished;
    m_incident = std::move(parsed.incident);
    m_lastResetIncidentId = std::move(parsed.lastResetIncidentId);
    m_lastResetOperator = std::move(parsed.lastResetOperator);
    m_lastResetReason = std::move(parsed.lastResetReason);
    if (m_incident)
    {
        m_inhibitReason = "incident_latched";
        SetModeLocked(SafetyMode::Latched);
        SafetyResult result = SafetyResult::Success("incident_restored");
        result.tripped = true;
        return result;
    }

    if (!m_config.heartbeatRequired)
    {
        if (!m_baselineEstablished)
        {
            m_baselineEstablished = true;
            std::string persistenceError;
            if (!PersistLocked(persistenceError))
            {
                m_baselineEstablished = false;
                m_persistenceError = persistenceError;
                m_inhibitReason = "state_persistence_failed";
                return SafetyResult::Failure("state_persistence_failed", persistenceError);
            }
        }
        m_inhibitReason.clear();
        SetModeLocked(SafetyMode::Ready);
        return SafetyResult::Success("ready_without_interlock");
    }

    m_inhibitReason = m_baselineEstablished
        ? "awaiting_interlock"
        : "baseline_required";
    return SafetyResult::Success("startup_inhibited");
}

SafetyResult SafetyController::ObserveHeartbeat(
    const SafetyHeartbeat& heartbeat,
    TimePoint now)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized)
        return SafetyResult::Failure("not_initialized", "Safety controller is not initialized.");
    if (!m_config.enabled)
        return SafetyResult::Failure("safety_disabled", "Safety interlock is disabled.");
    if (!IsValidIdentifier(heartbeat.sourceId, 128) ||
        !IsValidIdentifier(heartbeat.sessionId, 128))
    {
        return SafetyResult::Failure(
            "invalid_heartbeat", "Heartbeat source and session must be bounded identifiers.");
    }
    if (!m_config.expectedHeartbeatSource.empty() &&
        heartbeat.sourceId != m_config.expectedHeartbeatSource)
    {
        return SafetyResult::Failure(
            "unexpected_heartbeat_source", "Heartbeat source is not authorized by configuration.");
    }
    if (m_hasHeartbeat && heartbeat.sourceId != m_heartbeatSource)
    {
        return SafetyResult::Failure(
            "heartbeat_source_changed", "Heartbeat source cannot change during a run.");
    }

    const bool sameSession = m_hasHeartbeat && heartbeat.sessionId == m_heartbeatSession;
    const bool replayed = sameSession && heartbeat.sequence <= m_heartbeatSequence;
    const bool retiredSession = !sameSession && std::find(
        m_retiredHeartbeatSessions.begin(), m_retiredHeartbeatSessions.end(),
        heartbeat.sessionId) != m_retiredHeartbeatSessions.end();
    if (replayed || retiredSession)
    {
        SafetyResult result = SafetyResult::Failure(
            "replayed_heartbeat", "Heartbeat sequence or session was already observed.");
        if (heartbeat.signal != InterlockSignal::Safe)
        {
            const SafetyResult trip = TripLocked(
                heartbeat.signal == InterlockSignal::Alarm
                    ? "fire_alarm"
                    : "interlock_fault",
                heartbeat.sourceId);
            result.tripped = true;
            if (!trip.ok) result.message += " " + trip.message;
        }
        return result;
    }

    const bool sessionChanged = m_hasHeartbeat && !sameSession;
    if (sessionChanged)
    {
        m_retiredHeartbeatSessions.push_back(m_heartbeatSession);
        if (m_retiredHeartbeatSessions.size() > MaximumRetiredSessions)
            m_retiredHeartbeatSessions.erase(m_retiredHeartbeatSessions.begin());
        m_safeSince.reset();
        if (m_config.heartbeatRequired && GetMode() == SafetyMode::Ready)
        {
            m_inhibitReason = "heartbeat_session_changed";
            SetModeLocked(SafetyMode::Inhibited);
        }
    }

    m_hasHeartbeat = true;
    m_heartbeatSource = heartbeat.sourceId;
    m_heartbeatSession = heartbeat.sessionId;
    m_heartbeatSequence = heartbeat.sequence;
    m_lastHeartbeat = now;
    const InterlockSignal previousSignal = m_interlockSignal;
    m_interlockSignal = heartbeat.signal;

    if (heartbeat.signal == InterlockSignal::Safe)
    {
        if (previousSignal != InterlockSignal::Safe || sessionChanged || !m_safeSince)
            m_safeSince = now;
        if (m_config.heartbeatRequired && GetMode() == SafetyMode::Inhibited)
            m_inhibitReason = "interlock_stabilizing";
        return SafetyResult::Success(sessionChanged ? "heartbeat_session_changed" : "heartbeat_accepted");
    }

    m_safeSince.reset();
    SafetyResult result = TripLocked(
        heartbeat.signal == InterlockSignal::Alarm
            ? "fire_alarm"
            : "interlock_fault",
        heartbeat.sourceId);
    result.tripped = true;
    return result;
}

SafetyResult SafetyController::Tick(TimePoint now)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized)
        return SafetyResult::Failure("not_initialized", "Safety controller is not initialized.");
    if (!m_config.enabled)
    {
        SetModeLocked(SafetyMode::Ready);
        return SafetyResult::Success("safety_disabled");
    }
    if (GetMode() == SafetyMode::Latched)
    {
        if (!m_persistenceError.empty())
        {
            std::string persistenceError;
            if (!PersistLocked(persistenceError))
            {
                m_persistenceError = persistenceError;
                SafetyResult failure = SafetyResult::Failure(
                    "state_persistence_failed", persistenceError);
                failure.tripped = true;
                return failure;
            }
            m_persistenceError.clear();
        }
        SafetyResult result = SafetyResult::Success("incident_latched");
        result.tripped = true;
        return result;
    }
    if (!m_config.heartbeatRequired)
    {
        if (!m_baselineEstablished)
        {
            m_baselineEstablished = true;
            std::string persistenceError;
            if (!PersistLocked(persistenceError))
            {
                m_baselineEstablished = false;
                m_persistenceError = persistenceError;
                m_inhibitReason = "state_persistence_failed";
                SetModeLocked(SafetyMode::Inhibited);
                return SafetyResult::Failure("state_persistence_failed", persistenceError);
            }
        }
        m_inhibitReason.clear();
        m_persistenceError.clear();
        SetModeLocked(SafetyMode::Ready);
        return SafetyResult::Success("ready_without_interlock");
    }
    if (!m_hasHeartbeat)
    {
        m_inhibitReason = "awaiting_interlock";
        SetModeLocked(SafetyMode::Inhibited);
        return SafetyResult::Success("awaiting_interlock");
    }
    if (!IsHeartbeatFreshLocked(now))
    {
        SafetyResult result = TripLocked("heartbeat_timeout", m_heartbeatSource);
        result.tripped = true;
        return result;
    }
    if (m_interlockSignal != InterlockSignal::Safe)
    {
        SafetyResult result = TripLocked("interlock_not_safe", m_heartbeatSource);
        result.tripped = true;
        return result;
    }
    if (!HasBeenSafeForLocked(now, m_config.healthyBeforeReady))
    {
        m_inhibitReason = "interlock_stabilizing";
        SetModeLocked(SafetyMode::Inhibited);
        return SafetyResult::Success("interlock_stabilizing");
    }

    if (!m_baselineEstablished)
    {
        m_baselineEstablished = true;
        std::string persistenceError;
        if (!PersistLocked(persistenceError))
        {
            m_baselineEstablished = false;
            m_persistenceError = persistenceError;
            m_inhibitReason = "state_persistence_failed";
            SetModeLocked(SafetyMode::Inhibited);
            return SafetyResult::Failure("state_persistence_failed", persistenceError);
        }
    }

    m_inhibitReason.clear();
    m_persistenceError.clear();
    SetModeLocked(SafetyMode::Ready);
    return SafetyResult::Success("ready");
}

SafetyResult SafetyController::Trip(std::string cause, std::string source)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized)
        return SafetyResult::Failure("not_initialized", "Safety controller is not initialized.");
    if (!m_config.enabled)
        return SafetyResult::Failure("safety_disabled", "Safety controller is disabled.");
    if (!IsValidIdentifier(cause, 128) ||
        (!source.empty() && !IsValidIdentifier(source, 128)))
    {
        return SafetyResult::Failure(
            "invalid_incident", "Safety incident cause or source is invalid.");
    }
    return TripLocked(cause, source);
}

SafetyResult SafetyController::Reset(
    const std::string& expectedIncidentId,
    const std::string& operatorId,
    const std::string& reason,
    TimePoint now)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized)
        return SafetyResult::Failure("not_initialized", "Safety controller is not initialized.");
    if (!m_config.enabled)
        return SafetyResult::Failure("safety_disabled", "Safety controller is disabled.");
    if (!m_incident || GetMode() != SafetyMode::Latched)
        return SafetyResult::Failure("not_latched", "No safety incident is latched.");
    if (expectedIncidentId != m_incident->id)
        return SafetyResult::Failure(
            "incident_mismatch", "Reset incident ID does not match the active incident.");
    if (!IsValidIdentifier(operatorId, 128) || !IsBoundedText(reason, 512, false))
        return SafetyResult::Failure(
            "invalid_reset_audit", "Reset operator and reason are required bounded text.");
    if (m_config.heartbeatRequired &&
        (!IsHeartbeatFreshLocked(now) || m_interlockSignal != InterlockSignal::Safe))
        return SafetyResult::Failure(
            "interlock_not_safe", "A fresh SAFE interlock heartbeat is required to reset.");
    if (m_config.heartbeatRequired &&
        !HasBeenSafeForLocked(now, m_config.healthyBeforeReset))
        return SafetyResult::Failure(
            "interlock_not_stable", "The interlock has not remained SAFE long enough to reset.");

    const SafetyIncident previousIncident = *m_incident;
    const std::string previousResetIncidentId = m_lastResetIncidentId;
    const std::string previousResetOperator = m_lastResetOperator;
    const std::string previousResetReason = m_lastResetReason;
    m_incident.reset();
    m_baselineEstablished = true;
    m_lastResetIncidentId = previousIncident.id;
    m_lastResetOperator = operatorId;
    m_lastResetReason = reason;

    std::string persistenceError;
    if (!PersistLocked(persistenceError))
    {
        m_incident = previousIncident;
        m_lastResetIncidentId = previousResetIncidentId;
        m_lastResetOperator = previousResetOperator;
        m_lastResetReason = previousResetReason;
        m_persistenceError = persistenceError;
        m_inhibitReason = "incident_latched";
        SetModeLocked(SafetyMode::Latched);
        return SafetyResult::Failure("state_persistence_failed", persistenceError);
    }

    m_persistenceError.clear();
    m_inhibitReason.clear();
    SetModeLocked(SafetyMode::Ready);
    return SafetyResult::Success("reset");
}

SafetySnapshot SafetyController::GetSnapshot(TimePoint now) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SafetySnapshot snapshot;
    snapshot.mode = GetMode();
    snapshot.enabled = m_config.enabled;
    snapshot.baselineEstablished = m_baselineEstablished;
    snapshot.safeToPlay = snapshot.mode == SafetyMode::Ready;
    snapshot.hasFreshHeartbeat = IsHeartbeatFreshLocked(now);
    snapshot.resetAllowed = IsResetAllowedLocked(now);
    if (m_hasHeartbeat)
    {
        snapshot.heartbeatAge = now >= m_lastHeartbeat
            ? std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastHeartbeat)
            : std::chrono::milliseconds::zero();
    }
    snapshot.heartbeatSource = m_heartbeatSource;
    snapshot.heartbeatSession = m_heartbeatSession;
    snapshot.heartbeatSequence = m_heartbeatSequence;
    snapshot.interlockSignal = m_interlockSignal;
    snapshot.incident = m_incident;
    snapshot.inhibitReason = m_inhibitReason;
    snapshot.persistenceError = m_persistenceError;
    return snapshot;
}

SafetyResult SafetyController::TripLocked(
    const std::string& cause,
    const std::string& source)
{
    const std::string persistedCause = CauseWithSource(cause, source);
    bool changed = false;
    if (!m_incident)
    {
        SafetyIncident incident;
        incident.id = "incident-" + std::to_string(m_generation + 1);
        incident.causes.push_back(persistedCause);
        m_incident = std::move(incident);
        changed = true;
    }
    else if (std::find(
                 m_incident->causes.begin(), m_incident->causes.end(),
                 persistedCause) == m_incident->causes.end() &&
             m_incident->causes.size() < MaximumPersistedCauses)
    {
        m_incident->causes.push_back(persistedCause);
        changed = true;
    }

    m_baselineEstablished = true;
    m_inhibitReason = "incident_latched";
    SetModeLocked(SafetyMode::Latched);
    if (!changed && m_persistenceError.empty())
    {
        SafetyResult result = SafetyResult::Success("already_latched");
        result.tripped = true;
        return result;
    }

    std::string persistenceError;
    if (!PersistLocked(persistenceError))
    {
        m_persistenceError = persistenceError;
        SafetyResult result = SafetyResult::Failure(
            "state_persistence_failed", persistenceError);
        result.tripped = true;
        return result;
    }
    m_persistenceError.clear();
    SafetyResult result = SafetyResult::Success("tripped");
    result.tripped = true;
    return result;
}

bool SafetyController::PersistLocked(std::string& errorMessage)
{
    if (m_statePath.empty())
    {
        errorMessage = "Safety state path is empty.";
        return false;
    }
    if (m_generation == (std::numeric_limits<std::uint64_t>::max)())
    {
        errorMessage = "Safety state generation is exhausted.";
        return false;
    }

    const std::uint64_t nextGeneration = m_generation + 1;
    nlohmann::json incident = nullptr;
    if (m_incident)
    {
        incident = {
            {"id", m_incident->id},
            {"causes", m_incident->causes}
        };
    }
    nlohmann::json lastReset = nullptr;
    if (!m_lastResetIncidentId.empty())
    {
        lastReset = {
            {"incidentId", m_lastResetIncidentId},
            {"operatorId", m_lastResetOperator},
            {"reason", m_lastResetReason}
        };
    }
    const nlohmann::json document = {
        {"schemaVersion", SafetyStateSchemaVersion},
        {"generation", nextGeneration},
        {"baselineEstablished", m_baselineEstablished},
        {"latched", m_incident.has_value()},
        {"incident", std::move(incident)},
        {"lastReset", std::move(lastReset)}
    };
    if (!WriteJsonFileAtomically(m_statePath, document, errorMessage)) return false;
    m_generation = nextGeneration;
    return true;
}

bool SafetyController::IsHeartbeatFreshLocked(TimePoint now) const
{
    return m_hasHeartbeat && now >= m_lastHeartbeat &&
        now - m_lastHeartbeat <= m_config.heartbeatTimeout;
}

bool SafetyController::HasBeenSafeForLocked(
    TimePoint now,
    std::chrono::milliseconds duration) const
{
    return m_safeSince.has_value() && now >= *m_safeSince &&
        now - *m_safeSince >= duration;
}

bool SafetyController::IsResetAllowedLocked(TimePoint now) const
{
    return m_config.enabled && m_incident.has_value() &&
        GetMode() == SafetyMode::Latched &&
        (!m_config.heartbeatRequired ||
         (IsHeartbeatFreshLocked(now) &&
          m_interlockSignal == InterlockSignal::Safe &&
          HasBeenSafeForLocked(now, m_config.healthyBeforeReset)));
}

void SafetyController::SetModeLocked(SafetyMode mode)
{
    m_mode.store(mode, std::memory_order_release);
}

bool SafetyController::IsValidIdentifier(
    const std::string& value,
    std::size_t maximumLength)
{
    return IsBoundedText(value, maximumLength, false);
}

} // namespace TSM
