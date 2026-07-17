#include "pch.h"

#include "tsm_playback_memory_store.h"

#include <winsqlite/winsqlite3.h>

#include <nlohmann/json.hpp>

namespace TSM::Tests
{
namespace
{

namespace Heatmap = ListeningHeatmap;
namespace Memory = PlaybackMemory;
namespace History = TransitionHistory;

std::string TestPathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(value.data()), value.size());
}

bool CreateForgedPlaybackSchema(const std::filesystem::path& databasePath)
{
    sqlite3* database = nullptr;
    const std::string path = TestPathToUtf8(databasePath);
    if (sqlite3_open_v2(
            path.c_str(), &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK)
    {
        if (database) sqlite3_close_v2(database);
        return false;
    }
    constexpr const char* schema = R"sql(
CREATE TABLE heatmaps(normalized_path TEXT, file_size INTEGER, write_time INTEGER,
    state_schema_version INTEGER, track_duration_seconds REAL,
    bucket_duration_seconds REAL, daily_decay_factor REAL,
    reference_epoch_seconds INTEGER, bucket_count INTEGER);
CREATE TABLE heatmap_cells(normalized_path TEXT, cell_index INTEGER, fatigue REAL);
CREATE TABLE transition_history(singleton_id INTEGER,
    payload_schema_version INTEGER, payload_utf8 TEXT);
PRAGMA application_id = 1414745424;
PRAGMA user_version = 1;
)sql";
    char* message = nullptr;
    const bool success = sqlite3_exec(
        database, schema, nullptr, nullptr, &message) == SQLITE_OK;
    if (message) sqlite3_free(message);
    sqlite3_close_v2(database);
    return success;
}

bool ExecutePlaybackSql(
    const std::filesystem::path& databasePath,
    const char* sql)
{
    sqlite3* database = nullptr;
    const std::string path = TestPathToUtf8(databasePath);
    if (sqlite3_open_v2(
            path.c_str(), &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK)
    {
        if (database) sqlite3_close_v2(database);
        return false;
    }
    char* message = nullptr;
    const bool success = sqlite3_exec(
        database, sql, nullptr, nullptr, &message) == SQLITE_OK;
    if (message) sqlite3_free(message);
    sqlite3_close_v2(database);
    return success;
}

class PlaybackMemoryStoreTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto stamp = std::chrono::high_resolution_clock::now()
            .time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() /
            ("tsm_playback_memory_" + std::to_string(stamp));
        ASSERT_TRUE(std::filesystem::create_directories(directory));
        databasePath = directory / Memory::Store::DefaultFileName;
        trackPath = directory / "feature's; DROP TABLE heatmaps;--.wav";
        std::ofstream output(trackPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write("RIFF-playback-memory", 20);
        ASSERT_TRUE(output.good());
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    MusicAnalysis::FileIdentity Identity() const
    {
        MusicAnalysis::FileIdentity identity;
        std::string error;
        EXPECT_TRUE(MusicAnalysis::ReadFileIdentity(trackPath, identity, error))
            << error;
        return identity;
    }

    std::filesystem::path directory;
    std::filesystem::path databasePath;
    std::filesystem::path trackPath;
};

TEST_F(PlaybackMemoryStoreTests, RoundTripsAll1080CellsWithQuotedTrackPath)
{
    constexpr double ThreeHours = 3.0 * 60.0 * 60.0;
    Heatmap::State expected = Heatmap::CreateState(ThreeHours);
    ASSERT_EQ(expected.bucketFatigue.size(), 1080u);
    expected.referenceEpochSeconds = 1'800'000'000;
    for (std::size_t index = 0; index < expected.bucketFatigue.size(); ++index)
        expected.bucketFatigue[index] =
            static_cast<double>(index % 101u) / 100.0;

    Memory::Store store(databasePath);
    std::string error;
    ASSERT_TRUE(store.Initialize(error)) << error;
    const MusicAnalysis::FileIdentity identity = Identity();
    ASSERT_TRUE(store.SaveHeatmap(identity, expected, error)) << error;

    Heatmap::State invalid = expected;
    invalid.bucketFatigue.pop_back();
    EXPECT_FALSE(store.SaveHeatmap(identity, invalid, error));
    EXPECT_FALSE(error.empty());

    Heatmap::State loaded;
    ASSERT_EQ(
        store.LoadHeatmap(identity, loaded, error),
        Memory::StoreLookupStatus::Hit) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(loaded.schemaVersion, expected.schemaVersion);
    EXPECT_DOUBLE_EQ(
        loaded.trackDurationSeconds, expected.trackDurationSeconds);
    EXPECT_DOUBLE_EQ(
        loaded.bucketDurationSeconds, expected.bucketDurationSeconds);
    EXPECT_DOUBLE_EQ(loaded.dailyDecayFactor, expected.dailyDecayFactor);
    EXPECT_EQ(loaded.referenceEpochSeconds, expected.referenceEpochSeconds);
    EXPECT_EQ(loaded.bucketFatigue, expected.bucketFatigue);
}

TEST_F(PlaybackMemoryStoreTests, ChangedFileIdentityInvalidatesHeatmap)
{
    Memory::Store store(databasePath);
    std::string error;
    ASSERT_TRUE(store.Initialize(error)) << error;
    const MusicAnalysis::FileIdentity original = Identity();
    ASSERT_TRUE(store.SaveHeatmap(
        original, Heatmap::CreateState(60.0), error)) << error;

    {
        std::ofstream output(trackPath, std::ios::binary | std::ios::app);
        ASSERT_TRUE(output.is_open());
        output.put('x');
    }
    const MusicAnalysis::FileIdentity changed = Identity();
    ASSERT_GT(changed.size, original.size);

    Heatmap::State untouched = Heatmap::CreateState(10.0);
    untouched.bucketFatigue[0] = 0.75;
    EXPECT_EQ(
        store.LoadHeatmap(changed, untouched, error),
        Memory::StoreLookupStatus::Miss);
    EXPECT_NE(error.find("stale"), std::string::npos);
    ASSERT_EQ(untouched.bucketFatigue.size(), 1u);
    EXPECT_DOUBLE_EQ(untouched.bucketFatigue[0], 0.75);

    const Heatmap::State replacement = Heatmap::CreateState(30.0);
    ASSERT_EQ(replacement.bucketFatigue.size(), 3u);
    ASSERT_TRUE(store.SaveHeatmap(changed, replacement, error)) << error;
    Heatmap::State replaced;
    ASSERT_EQ(
        store.LoadHeatmap(changed, replaced, error),
        Memory::StoreLookupStatus::Hit) << error;
    EXPECT_EQ(replaced.bucketFatigue.size(), 3u);
    EXPECT_EQ(replaced.bucketFatigue, replacement.bucketFatigue);

    const auto previousWriteTime = std::filesystem::last_write_time(trackPath);
    std::filesystem::last_write_time(
        trackPath, previousWriteTime + std::chrono::seconds(2));
    const MusicAnalysis::FileIdentity retimed = Identity();
    ASSERT_EQ(retimed.size, changed.size);
    ASSERT_NE(retimed.writeTime, changed.writeTime);
    EXPECT_EQ(
        store.LoadHeatmap(retimed, untouched, error),
        Memory::StoreLookupStatus::Miss);
    EXPECT_NE(error.find("stale"), std::string::npos);
}

TEST_F(PlaybackMemoryStoreTests, PersistsHeatmapAndHistoryAcrossReopen)
{
    const MusicAnalysis::FileIdentity identity = Identity();
    Heatmap::State expectedHeatmap = Heatmap::CreateState(30.0);
    ASSERT_EQ(expectedHeatmap.bucketFatigue.size(), 3u);
    expectedHeatmap.bucketFatigue = {0.1, 0.5, 0.9};
    expectedHeatmap.referenceEpochSeconds = 1'800'000'000;
    History::History expectedHistory;
    ASSERT_TRUE(expectedHistory.RecordTransition("pre-show", "feature", 500));

    std::string error;
    {
        Memory::Store first(databasePath);
        ASSERT_TRUE(first.Initialize(error)) << error;
        ASSERT_TRUE(first.SaveHeatmap(identity, expectedHeatmap, error)) << error;
        ASSERT_TRUE(first.SaveTransitionHistory(expectedHistory, error)) << error;
    }

    Memory::Store reopened(databasePath);
    ASSERT_TRUE(reopened.Initialize(error)) << error;
    Heatmap::State loadedHeatmap;
    ASSERT_EQ(
        reopened.LoadHeatmap(identity, loadedHeatmap, error),
        Memory::StoreLookupStatus::Hit) << error;
    EXPECT_EQ(loadedHeatmap.bucketFatigue, expectedHeatmap.bucketFatigue);
    EXPECT_EQ(
        loadedHeatmap.referenceEpochSeconds,
        expectedHeatmap.referenceEpochSeconds);

    History::History loadedHistory;
    ASSERT_EQ(
        reopened.LoadTransitionHistory(loadedHistory, error),
        Memory::StoreLookupStatus::Hit) << error;
    EXPECT_EQ(loadedHistory.ExportState(), expectedHistory.ExportState());
}

TEST_F(PlaybackMemoryStoreTests, RejectsForgedVersionOneSchemaBeforeWalMode)
{
    ASSERT_TRUE(CreateForgedPlaybackSchema(databasePath));
    Memory::Store store(databasePath);
    std::string error;
    EXPECT_FALSE(store.Initialize(error));
    EXPECT_FALSE(error.empty());
    std::filesystem::path walPath = databasePath;
    walPath += "-wal";
    EXPECT_FALSE(std::filesystem::exists(walPath));
}

TEST_F(PlaybackMemoryStoreTests, TransitionHistoryKeepsDirectionAndCounts)
{
    History::History expected;
    const std::string injectedId = "A's; DROP TABLE transition_history;--";
    ASSERT_TRUE(expected.RecordTransition(injectedId, "B", 100));
    ASSERT_TRUE(expected.RecordTransition(injectedId, "B", 200));
    ASSERT_TRUE(expected.RecordTransition("B", injectedId, 300));

    Memory::Store store(databasePath);
    std::string error;
    ASSERT_TRUE(store.Initialize(error)) << error;
    ASSERT_TRUE(store.SaveTransitionHistory(expected, error)) << error;

    History::History loaded({0.8, 0.5});
    ASSERT_EQ(
        store.LoadTransitionHistory(loaded, error),
        Memory::StoreLookupStatus::Hit) << error;
    EXPECT_EQ(loaded.ExportState(), expected.ExportState());

    const auto forward = loaded.GetState(injectedId, "B", 300);
    const auto reverse = loaded.GetState("B", injectedId, 300);
    ASSERT_TRUE(forward.has_value());
    ASSERT_TRUE(reverse.has_value());
    EXPECT_EQ(forward->totalCount, 2u);
    EXPECT_EQ(reverse->totalCount, 1u);
    EXPECT_GT(forward->fatigue, reverse->fatigue);
}

TEST_F(PlaybackMemoryStoreTests, ClearOperationsHaveExplicitIndependentScopes)
{
    Memory::Store store(databasePath);
    std::string error;
    ASSERT_TRUE(store.Initialize(error)) << error;
    const MusicAnalysis::FileIdentity identity = Identity();
    const Heatmap::State heatmap = Heatmap::CreateState(120.0);
    History::History history;
    ASSERT_TRUE(history.RecordTransition("pre'show", "feature", 500));
    ASSERT_TRUE(store.SaveHeatmap(identity, heatmap, error)) << error;
    ASSERT_TRUE(store.SaveTransitionHistory(history, error)) << error;

    ASSERT_TRUE(store.ClearHeatmap(identity.normalizedPath, error)) << error;
    Heatmap::State loadedHeatmap;
    EXPECT_EQ(
        store.LoadHeatmap(identity, loadedHeatmap, error),
        Memory::StoreLookupStatus::Miss);
    History::History loadedHistory;
    EXPECT_EQ(
        store.LoadTransitionHistory(loadedHistory, error),
        Memory::StoreLookupStatus::Hit) << error;

    ASSERT_TRUE(store.SaveHeatmap(identity, heatmap, error)) << error;
    ASSERT_TRUE(store.ClearTransitionHistory(error)) << error;
    EXPECT_EQ(
        store.LoadHeatmap(identity, loadedHeatmap, error),
        Memory::StoreLookupStatus::Hit) << error;
    EXPECT_EQ(
        store.LoadTransitionHistory(loadedHistory, error),
        Memory::StoreLookupStatus::Miss);

    ASSERT_TRUE(store.SaveTransitionHistory(history, error)) << error;
    ASSERT_TRUE(store.ClearAll(error)) << error;
    EXPECT_EQ(
        store.LoadHeatmap(identity, loadedHeatmap, error),
        Memory::StoreLookupStatus::Miss);
    EXPECT_EQ(
        store.LoadTransitionHistory(loadedHistory, error),
        Memory::StoreLookupStatus::Miss);
}

TEST_F(PlaybackMemoryStoreTests, TrackResetCommitsHeatmapAndHistoryTogether)
{
    Memory::Store store(databasePath);
    std::string error;
    ASSERT_TRUE(store.Initialize(error)) << error;
    const MusicAnalysis::FileIdentity identity = Identity();
    Heatmap::State heatmap = Heatmap::CreateState(120.0);
    ASSERT_TRUE(Heatmap::RecordCoverage(heatmap, 0.0, 10.0, 1.0, 100));
    History::History history;
    ASSERT_TRUE(history.RecordTransition("track", "next", 100));
    ASSERT_TRUE(store.SaveHeatmap(identity, heatmap, error)) << error;
    ASSERT_TRUE(store.SaveTransitionHistory(history, error)) << error;

    History::History remaining;
    ASSERT_TRUE(store.ClearTrackMemory(
        identity.normalizedPath, remaining, error)) << error;

    Heatmap::State loadedHeatmap;
    EXPECT_EQ(
        store.LoadHeatmap(identity, loadedHeatmap, error),
        Memory::StoreLookupStatus::Miss);
    History::History loadedHistory;
    EXPECT_EQ(
        store.LoadTransitionHistory(loadedHistory, error),
        Memory::StoreLookupStatus::Hit) << error;
    EXPECT_EQ(loadedHistory.Size(), 0u);
}

TEST_F(PlaybackMemoryStoreTests, TrackResetRollsBackIfHistoryWriteFails)
{
    Memory::Store store(databasePath);
    std::string error;
    ASSERT_TRUE(store.Initialize(error)) << error;
    const MusicAnalysis::FileIdentity identity = Identity();
    Heatmap::State heatmap = Heatmap::CreateState(120.0);
    ASSERT_TRUE(Heatmap::RecordCoverage(heatmap, 0.0, 10.0, 1.0, 100));
    History::History history;
    ASSERT_TRUE(history.RecordTransition("track", "next", 100));
    ASSERT_TRUE(store.SaveHeatmap(identity, heatmap, error)) << error;
    ASSERT_TRUE(store.SaveTransitionHistory(history, error)) << error;

    ASSERT_TRUE(ExecutePlaybackSql(databasePath, R"sql(
CREATE TRIGGER fail_transition_history_insert
BEFORE INSERT ON transition_history
BEGIN
    SELECT RAISE(ABORT, 'injected transition write failure');
END;
)sql"));

    History::History remaining;
    EXPECT_FALSE(store.ClearTrackMemory(
        identity.normalizedPath, remaining, error));
    EXPECT_NE(error.find("injected transition write failure"), std::string::npos);

    Heatmap::State loadedHeatmap;
    ASSERT_EQ(
        store.LoadHeatmap(identity, loadedHeatmap, error),
        Memory::StoreLookupStatus::Hit) << error;
    EXPECT_EQ(loadedHeatmap.bucketFatigue, heatmap.bucketFatigue);
    History::History loadedHistory;
    ASSERT_EQ(
        store.LoadTransitionHistory(loadedHistory, error),
        Memory::StoreLookupStatus::Hit) << error;
    EXPECT_EQ(loadedHistory.ExportState(), history.ExportState());
}

TEST_F(PlaybackMemoryStoreTests, CorruptDatabaseIsExplicitlyUnavailableAndNonFatal)
{
    {
        std::ofstream output(databasePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output << "not a SQLite playback memory database";
    }

    Memory::Store store(databasePath);
    std::string error;
    EXPECT_FALSE(store.Initialize(error));
    EXPECT_FALSE(error.empty());

    Heatmap::State untouchedHeatmap = Heatmap::CreateState(10.0);
    untouchedHeatmap.bucketFatigue[0] = 0.65;
    EXPECT_EQ(
        store.LoadHeatmap(Identity(), untouchedHeatmap, error),
        Memory::StoreLookupStatus::Unavailable);
    ASSERT_EQ(untouchedHeatmap.bucketFatigue.size(), 1u);
    EXPECT_DOUBLE_EQ(untouchedHeatmap.bucketFatigue[0], 0.65);

    History::History untouchedHistory;
    ASSERT_TRUE(untouchedHistory.RecordTransition("kept", "pair", 1));
    const nlohmann::json committed = untouchedHistory.ExportState();
    EXPECT_EQ(
        store.LoadTransitionHistory(untouchedHistory, error),
        Memory::StoreLookupStatus::Unavailable);
    EXPECT_EQ(untouchedHistory.ExportState(), committed);
    EXPECT_FALSE(error.empty());
}

} // namespace
} // namespace TSM::Tests
