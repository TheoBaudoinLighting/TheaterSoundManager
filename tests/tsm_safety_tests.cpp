#include "pch.h"

#include "tsm_atomic_file.h"
#include "tsm_safety.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace TSM::Tests
{
namespace
{

using namespace std::chrono_literals;

class SafetyControllerTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<unsigned long long> sequence{0};
        const auto timestamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() /
            ("tsm_safety_tests_" + std::to_string(timestamp) + "_" +
             std::to_string(++sequence));
        std::filesystem::create_directories(directory);
        statePath = directory / "safety_state.json";

        config.enabled = true;
        config.expectedHeartbeatSource = "fire-panel-a";
        config.heartbeatTimeout = 100ms;
        config.healthyBeforeReady = 10ms;
        config.healthyBeforeReset = 20ms;
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    SafetyHeartbeat Heartbeat(
        std::uint64_t sequence,
        InterlockSignal signal = InterlockSignal::Safe,
        std::string session = "panel-session-1") const
    {
        return {"fire-panel-a", std::move(session), sequence, signal};
    }

    void EstablishReady(SafetyController& controller)
    {
        ASSERT_TRUE(controller.Initialize(config, statePath, epoch).ok);
        ASSERT_EQ(controller.GetMode(), SafetyMode::Inhibited);
        ASSERT_TRUE(controller.ObserveHeartbeat(Heartbeat(1), epoch).ok);
        ASSERT_TRUE(controller.Tick(epoch + 10ms).ok);
        ASSERT_EQ(controller.GetMode(), SafetyMode::Ready);
        ASSERT_TRUE(std::filesystem::is_regular_file(statePath));
    }

    static bool ContainsCause(const SafetyIncident& incident, const std::string& cause)
    {
        return std::find(incident.causes.begin(), incident.causes.end(), cause) !=
            incident.causes.end();
    }

    SafetyConfig config;
    SafetyController::TimePoint epoch{};
    std::filesystem::path directory;
    std::filesystem::path statePath;
};

TEST_F(SafetyControllerTests, MissingStateStartsInhibitedUntilSafeHeartbeatIsStable)
{
    SafetyController controller;
    const SafetyResult initialized = controller.Initialize(config, statePath, epoch);

    ASSERT_TRUE(initialized.ok);
    EXPECT_EQ(initialized.code, "state_missing");
    EXPECT_EQ(controller.GetMode(), SafetyMode::Inhibited);
    EXPECT_FALSE(controller.CanPlayNormalAudio());

    ASSERT_TRUE(controller.ObserveHeartbeat(Heartbeat(1), epoch).ok);
    EXPECT_EQ(controller.GetMode(), SafetyMode::Inhibited);
    EXPECT_EQ(controller.Tick(epoch + 9ms).code, "interlock_stabilizing");
    EXPECT_EQ(controller.GetMode(), SafetyMode::Inhibited);

    EXPECT_EQ(controller.Tick(epoch + 10ms).code, "ready");
    EXPECT_EQ(controller.GetMode(), SafetyMode::Ready);
    EXPECT_TRUE(controller.CanPlayNormalAudio());
    EXPECT_TRUE(controller.GetSnapshot(epoch + 10ms).baselineEstablished);
}

TEST_F(SafetyControllerTests, DisabledSafetyAllowsFirstLaunchWithoutState)
{
    config.enabled = false;
    SafetyController controller;
    const SafetyResult initialized = controller.Initialize(config, statePath, epoch);

    ASSERT_TRUE(initialized.ok);
    EXPECT_EQ(initialized.code, "safety_disabled");
    EXPECT_EQ(controller.GetMode(), SafetyMode::Ready);
    EXPECT_FALSE(std::filesystem::exists(statePath));
}

TEST_F(SafetyControllerTests, EnabledSafetyWithoutRequiredHeartbeatCreatesBaselineAndRuns)
{
    config.heartbeatRequired = false;
    SafetyController firstRun;
    const SafetyResult initialized = firstRun.Initialize(config, statePath, epoch);

    ASSERT_TRUE(initialized.ok) << initialized.message;
    EXPECT_EQ(initialized.code, "baseline_created");
    EXPECT_EQ(firstRun.GetMode(), SafetyMode::Ready);
    EXPECT_TRUE(firstRun.GetSnapshot(epoch).baselineEstablished);
    EXPECT_TRUE(std::filesystem::is_regular_file(statePath));

    SafetyController secondRun;
    const SafetyResult restarted = secondRun.Initialize(config, statePath, epoch + 1s);
    ASSERT_TRUE(restarted.ok) << restarted.message;
    EXPECT_EQ(restarted.code, "ready_without_interlock");
    EXPECT_EQ(secondRun.GetMode(), SafetyMode::Ready);
}

TEST_F(SafetyControllerTests, ManualIncidentCanBeResetWithoutHeartbeatWhenNotRequired)
{
    config.heartbeatRequired = false;
    SafetyController controller;
    ASSERT_TRUE(controller.Initialize(config, statePath, epoch).ok);

    const SafetyResult trip = controller.Trip("manual_emergency", "operator-console");
    ASSERT_TRUE(trip.tripped);
    ASSERT_EQ(controller.GetMode(), SafetyMode::Latched);
    const std::string incidentId = controller.GetSnapshot(epoch).incident->id;
    EXPECT_TRUE(controller.GetSnapshot(epoch).resetAllowed);

    const SafetyResult reset = controller.Reset(
        incidentId, "operator-a", "Area inspected", epoch);
    ASSERT_TRUE(reset.ok) << reset.message;
    EXPECT_EQ(controller.GetMode(), SafetyMode::Ready);
    EXPECT_FALSE(controller.GetSnapshot(epoch).incident.has_value());
}

TEST_F(SafetyControllerTests, CorruptStateFailsClosedAndIsRewrittenAsLatched)
{
    {
        std::ofstream output(statePath, std::ios::binary | std::ios::trunc);
        output << "{not-json";
    }

    SafetyController controller;
    const SafetyResult initialized = controller.Initialize(config, statePath, epoch);

    EXPECT_FALSE(initialized.ok);
    EXPECT_EQ(initialized.code, "state_corrupt");
    EXPECT_TRUE(initialized.tripped);
    EXPECT_EQ(controller.GetMode(), SafetyMode::Latched);
    const SafetySnapshot snapshot = controller.GetSnapshot(epoch);
    ASSERT_TRUE(snapshot.incident.has_value());
    EXPECT_TRUE(ContainsCause(*snapshot.incident, "state_corrupt"));

    const JsonFileReadResult persisted = ReadJsonFileStrict(statePath);
    ASSERT_TRUE(persisted.IsSuccess()) << persisted.error;
    EXPECT_TRUE(persisted.document.at("latched").get<bool>());
}

TEST_F(SafetyControllerTests, HeartbeatTimeoutLatchesAndPersistsIncident)
{
    SafetyController controller;
    EstablishReady(controller);

    const SafetyResult tick = controller.Tick(epoch + 101ms);
    EXPECT_TRUE(tick.tripped);
    EXPECT_EQ(controller.GetMode(), SafetyMode::Latched);
    const SafetySnapshot snapshot = controller.GetSnapshot(epoch + 101ms);
    ASSERT_TRUE(snapshot.incident.has_value());
    EXPECT_TRUE(ContainsCause(*snapshot.incident, "heartbeat_timeout@fire-panel-a"));
    EXPECT_FALSE(snapshot.safeToPlay);
}

TEST_F(SafetyControllerTests, ReplayedHeartbeatCannotExtendFreshness)
{
    SafetyController controller;
    ASSERT_TRUE(controller.Initialize(config, statePath, epoch).ok);
    ASSERT_TRUE(controller.ObserveHeartbeat(Heartbeat(10), epoch).ok);
    ASSERT_TRUE(controller.Tick(epoch + 10ms).ok);
    ASSERT_EQ(controller.GetMode(), SafetyMode::Ready);

    const SafetyResult replay = controller.ObserveHeartbeat(Heartbeat(10), epoch + 90ms);
    EXPECT_FALSE(replay.ok);
    EXPECT_EQ(replay.code, "replayed_heartbeat");
    EXPECT_EQ(controller.GetSnapshot(epoch + 90ms).heartbeatSequence, 10u);

    const SafetyResult timedOut = controller.Tick(epoch + 101ms);
    EXPECT_TRUE(timedOut.tripped);
    EXPECT_EQ(controller.GetMode(), SafetyMode::Latched);
}

TEST_F(SafetyControllerTests, RetiredHeartbeatSessionCannotBeReplayed)
{
    SafetyController controller;
    EstablishReady(controller);

    const SafetyResult changed = controller.ObserveHeartbeat(
        Heartbeat(1, InterlockSignal::Safe, "panel-session-2"), epoch + 20ms);
    ASSERT_TRUE(changed.ok);
    EXPECT_EQ(changed.code, "heartbeat_session_changed");
    EXPECT_EQ(controller.GetMode(), SafetyMode::Inhibited);

    const SafetyResult retired = controller.ObserveHeartbeat(
        Heartbeat(2, InterlockSignal::Safe, "panel-session-1"), epoch + 21ms);
    EXPECT_FALSE(retired.ok);
    EXPECT_EQ(retired.code, "replayed_heartbeat");
    EXPECT_EQ(controller.GetSnapshot(epoch + 21ms).heartbeatSession, "panel-session-2");
}

TEST_F(SafetyControllerTests, SafeSignalAfterAlarmNeverClearsLatch)
{
    SafetyController controller;
    EstablishReady(controller);

    const SafetyResult alarm = controller.ObserveHeartbeat(
        Heartbeat(2, InterlockSignal::Alarm), epoch + 11ms);
    ASSERT_TRUE(alarm.tripped);
    ASSERT_EQ(controller.GetMode(), SafetyMode::Latched);
    const std::string incidentId = controller.GetSnapshot(epoch + 11ms).incident->id;

    ASSERT_TRUE(controller.ObserveHeartbeat(Heartbeat(3), epoch + 12ms).ok);
    EXPECT_EQ(controller.GetMode(), SafetyMode::Latched);
    EXPECT_EQ(controller.Tick(epoch + 50ms).code, "incident_latched");
    EXPECT_EQ(controller.GetSnapshot(epoch + 50ms).incident->id, incidentId);
}

TEST_F(SafetyControllerTests, ResetRequiresMatchingIncidentAndFreshStableSafeInterlock)
{
    SafetyController controller;
    EstablishReady(controller);
    ASSERT_TRUE(controller.ObserveHeartbeat(
        Heartbeat(2, InterlockSignal::Alarm), epoch + 11ms).tripped);
    const std::string incidentId = controller.GetSnapshot(epoch + 11ms).incident->id;
    ASSERT_TRUE(controller.ObserveHeartbeat(Heartbeat(3), epoch + 12ms).ok);

    SafetyResult reset = controller.Reset(
        "wrong-incident", "operator-a", "Fire panel inspected", epoch + 40ms);
    EXPECT_FALSE(reset.ok);
    EXPECT_EQ(reset.code, "incident_mismatch");

    reset = controller.Reset(
        incidentId, "operator-a", "Fire panel inspected", epoch + 31ms);
    EXPECT_FALSE(reset.ok);
    EXPECT_EQ(reset.code, "interlock_not_stable");
    EXPECT_EQ(controller.GetMode(), SafetyMode::Latched);

    reset = controller.Reset(
        incidentId, "operator-a", "Fire panel inspected", epoch + 32ms);
    ASSERT_TRUE(reset.ok) << reset.message;
    EXPECT_EQ(reset.code, "reset");
    EXPECT_EQ(controller.GetMode(), SafetyMode::Ready);
    EXPECT_FALSE(controller.GetSnapshot(epoch + 32ms).incident.has_value());
}

TEST_F(SafetyControllerTests, LatchedIncidentSurvivesRebootWithSameCasIdentifier)
{
    std::string incidentId;
    {
        SafetyController firstRun;
        EstablishReady(firstRun);
        const SafetyResult trip = firstRun.Trip("manual_emergency", "operator-console");
        ASSERT_TRUE(trip.tripped);
        incidentId = firstRun.GetSnapshot(epoch + 10ms).incident->id;
    }

    SafetyController secondRun;
    const SafetyResult initialized = secondRun.Initialize(config, statePath, epoch + 1s);
    ASSERT_TRUE(initialized.ok);
    EXPECT_EQ(initialized.code, "incident_restored");
    EXPECT_TRUE(initialized.tripped);
    EXPECT_EQ(secondRun.GetMode(), SafetyMode::Latched);
    ASSERT_TRUE(secondRun.GetSnapshot(epoch + 1s).incident.has_value());
    EXPECT_EQ(secondRun.GetSnapshot(epoch + 1s).incident->id, incidentId);

    ASSERT_TRUE(secondRun.ObserveHeartbeat(
        Heartbeat(1, InterlockSignal::Safe, "panel-session-after-reboot"),
        epoch + 1s).ok);
    EXPECT_EQ(secondRun.Tick(epoch + 1050ms).code, "incident_latched");
    EXPECT_EQ(secondRun.GetMode(), SafetyMode::Latched);
}

TEST_F(SafetyControllerTests, StrictStateRejectsUnknownFields)
{
    nlohmann::json document = {
        {"schemaVersion", 1},
        {"generation", 1},
        {"baselineEstablished", true},
        {"latched", false},
        {"incident", nullptr},
        {"lastReset", nullptr},
        {"unexpected", true}
    };
    std::string writeError;
    ASSERT_TRUE(WriteJsonFileAtomically(statePath, document, writeError)) << writeError;

    SafetyController controller;
    const SafetyResult initialized = controller.Initialize(config, statePath, epoch);
    EXPECT_FALSE(initialized.ok);
    EXPECT_EQ(initialized.code, "state_corrupt");
    EXPECT_EQ(controller.GetMode(), SafetyMode::Latched);
}

} // namespace
} // namespace TSM::Tests
