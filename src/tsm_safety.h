#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace TSM
{

enum class SafetyMode
{
    Inhibited,
    Ready,
    Latched
};

enum class InterlockSignal
{
    Safe,
    Alarm,
    Fault
};

struct SafetyConfig
{
    bool enabled = true;
    bool heartbeatRequired = true;
    std::string expectedHeartbeatSource;
    std::chrono::milliseconds heartbeatTimeout{2000};
    std::chrono::milliseconds healthyBeforeReady{1000};
    std::chrono::milliseconds healthyBeforeReset{1000};
};

struct SafetyHeartbeat
{
    std::string sourceId;
    std::string sessionId;
    std::uint64_t sequence = 0;
    InterlockSignal signal = InterlockSignal::Safe;
};

struct SafetyResult
{
    bool ok = false;
    std::string code;
    std::string message;
    bool tripped = false;

    static SafetyResult Success(std::string code = "ok");
    static SafetyResult Failure(std::string code, std::string message);
};

struct SafetyIncident
{
    std::string id;
    std::vector<std::string> causes;
};

struct SafetySnapshot
{
    SafetyMode mode = SafetyMode::Inhibited;
    bool enabled = true;
    bool baselineEstablished = false;
    bool safeToPlay = false;
    bool hasFreshHeartbeat = false;
    bool resetAllowed = false;
    std::optional<std::chrono::milliseconds> heartbeatAge;
    std::string heartbeatSource;
    std::string heartbeatSession;
    std::uint64_t heartbeatSequence = 0;
    InterlockSignal interlockSignal = InterlockSignal::Fault;
    std::optional<SafetyIncident> incident;
    std::string inhibitReason;
    std::string persistenceError;
};

class SafetyController
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    SafetyController() = default;
    ~SafetyController() = default;

    SafetyController(const SafetyController&) = delete;
    SafetyController& operator=(const SafetyController&) = delete;

    SafetyResult Initialize(
        SafetyConfig config,
        std::filesystem::path statePath,
        TimePoint now);

    SafetyResult ObserveHeartbeat(const SafetyHeartbeat& heartbeat, TimePoint now);
    SafetyResult Tick(TimePoint now);
    SafetyResult Trip(std::string cause, std::string source = {});
    SafetyResult Reset(
        const std::string& expectedIncidentId,
        const std::string& operatorId,
        const std::string& reason,
        TimePoint now);

    SafetySnapshot GetSnapshot(TimePoint now) const;
    SafetyMode GetMode() const { return m_mode.load(std::memory_order_acquire); }
    bool CanPlayNormalAudio() const
    {
        return GetMode() == SafetyMode::Ready;
    }

private:
    SafetyResult TripLocked(const std::string& cause, const std::string& source);
    bool PersistLocked(std::string& errorMessage);
    bool IsHeartbeatFreshLocked(TimePoint now) const;
    bool HasBeenSafeForLocked(TimePoint now, std::chrono::milliseconds duration) const;
    bool IsResetAllowedLocked(TimePoint now) const;
    void SetModeLocked(SafetyMode mode);
    static bool IsValidIdentifier(const std::string& value, std::size_t maximumLength);

    mutable std::mutex m_mutex;
    std::atomic<SafetyMode> m_mode{SafetyMode::Inhibited};
    SafetyConfig m_config;
    std::filesystem::path m_statePath;
    bool m_initialized = false;
    bool m_baselineEstablished = false;
    std::uint64_t m_generation = 0;
    std::optional<SafetyIncident> m_incident;
    std::string m_inhibitReason = "not_initialized";
    std::string m_persistenceError;

    bool m_hasHeartbeat = false;
    std::string m_heartbeatSource;
    std::string m_heartbeatSession;
    std::uint64_t m_heartbeatSequence = 0;
    InterlockSignal m_interlockSignal = InterlockSignal::Fault;
    TimePoint m_lastHeartbeat{};
    std::optional<TimePoint> m_safeSince;
    std::vector<std::string> m_retiredHeartbeatSessions;

    std::string m_lastResetIncidentId;
    std::string m_lastResetOperator;
    std::string m_lastResetReason;
};

const char* SafetyModeToString(SafetyMode mode);
const char* InterlockSignalToString(InterlockSignal signal);

} // namespace TSM
