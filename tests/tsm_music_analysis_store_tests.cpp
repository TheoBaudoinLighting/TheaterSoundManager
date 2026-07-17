#include "pch.h"

#include "tsm_music_analysis_store.h"

#include <winsqlite/winsqlite3.h>

#include <limits>

namespace TSM::Tests
{
namespace
{

using MusicAnalysis::AnalysisStore;
using MusicAnalysis::Candidate;
using MusicAnalysis::CandidateKind;
using MusicAnalysis::FileIdentity;
using MusicAnalysis::Frame;
using MusicAnalysis::Profile;
using MusicAnalysis::StoreLookupStatus;
using MusicAnalysis::TrackAnalysis;

std::string TestPathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(value.data()), value.size());
}

TrackAnalysis SampleAnalysis()
{
    TrackAnalysis analysis;
    analysis.durationSeconds = 10.0f;
    Frame opening;
    opening.timeSeconds = 0.0f;
    opening.energy = 0.15f;
    opening.rms = 0.12f;
    opening.loudnessLufs = -32.0f;
    opening.truePeakDb = -8.0f;
    opening.spectralCentroidHz = 400.0f;
    opening.spectralFlux = 0.10f;
    opening.onsetStrength = 0.20f;
    opening.silenceProbability = 0.80f;
    opening.energySlope = 0.10f;

    Frame middle;
    middle.timeSeconds = 5.0f;
    middle.energy = 0.55f;
    middle.rms = 0.52f;
    middle.loudnessLufs = -20.0f;
    middle.truePeakDb = -1.5f;
    middle.spectralCentroidHz = 900.0f;
    middle.spectralFlux = 0.60f;
    middle.onsetStrength = 0.75f;
    middle.silenceProbability = 0.10f;
    middle.energySlope = 0.25f;

    Frame ending;
    ending.timeSeconds = 10.0f;
    ending.energy = 0.25f;
    ending.rms = 0.22f;
    ending.loudnessLufs = -28.0f;
    ending.truePeakDb = -4.0f;
    ending.spectralCentroidHz = 500.0f;
    ending.spectralFlux = 0.20f;
    ending.onsetStrength = 0.30f;
    ending.silenceProbability = 0.65f;
    ending.energySlope = -0.30f;
    analysis.frames = {opening, middle, ending};

    Candidate entry;
    entry.kind = CandidateKind::Entry;
    entry.frameIndex = 0;
    entry.timeSeconds = 0.0f;
    entry.score = 0.91f;
    entry.profile = Profile{0.20f, -30.0f, 450.0f, 0.15f, 0.25f};
    analysis.entryCandidates.push_back(entry);

    Candidate exit;
    exit.kind = CandidateKind::Exit;
    exit.frameIndex = 2;
    exit.timeSeconds = 10.0f;
    exit.score = 0.87f;
    exit.profile = Profile{0.30f, -27.0f, 520.0f, 0.22f, 0.35f};
    analysis.exitCandidates.push_back(exit);
    return analysis;
}

void ExpectProfileEqual(const Profile& actual, const Profile& expected)
{
    EXPECT_FLOAT_EQ(actual.energy, expected.energy);
    EXPECT_FLOAT_EQ(actual.loudnessLufs, expected.loudnessLufs);
    EXPECT_FLOAT_EQ(actual.spectralCentroidHz, expected.spectralCentroidHz);
    EXPECT_FLOAT_EQ(actual.spectralFlux, expected.spectralFlux);
    EXPECT_FLOAT_EQ(actual.onsetStrength, expected.onsetStrength);
}

void ExpectAnalysisEqual(const TrackAnalysis& actual, const TrackAnalysis& expected)
{
    EXPECT_FLOAT_EQ(actual.durationSeconds, expected.durationSeconds);
    ASSERT_EQ(actual.frames.size(), expected.frames.size());
    for (std::size_t index = 0; index < expected.frames.size(); ++index)
    {
        const Frame& actualFrame = actual.frames[index];
        const Frame& expectedFrame = expected.frames[index];
        EXPECT_FLOAT_EQ(actualFrame.timeSeconds, expectedFrame.timeSeconds);
        EXPECT_FLOAT_EQ(actualFrame.energy, expectedFrame.energy);
        EXPECT_FLOAT_EQ(actualFrame.rms, expectedFrame.rms);
        EXPECT_FLOAT_EQ(actualFrame.loudnessLufs, expectedFrame.loudnessLufs);
        EXPECT_FLOAT_EQ(actualFrame.truePeakDb, expectedFrame.truePeakDb);
        EXPECT_FLOAT_EQ(
            actualFrame.spectralCentroidHz, expectedFrame.spectralCentroidHz);
        EXPECT_FLOAT_EQ(actualFrame.spectralFlux, expectedFrame.spectralFlux);
        EXPECT_FLOAT_EQ(actualFrame.onsetStrength, expectedFrame.onsetStrength);
        EXPECT_FLOAT_EQ(
            actualFrame.silenceProbability, expectedFrame.silenceProbability);
        EXPECT_FLOAT_EQ(actualFrame.energySlope, expectedFrame.energySlope);
    }

    const auto expectCandidates = [](const std::vector<Candidate>& actualCandidates,
                                     const std::vector<Candidate>& expectedCandidates) {
        ASSERT_EQ(actualCandidates.size(), expectedCandidates.size());
        for (std::size_t index = 0; index < expectedCandidates.size(); ++index)
        {
            const Candidate& actualCandidate = actualCandidates[index];
            const Candidate& expectedCandidate = expectedCandidates[index];
            EXPECT_EQ(actualCandidate.kind, expectedCandidate.kind);
            EXPECT_EQ(actualCandidate.frameIndex, expectedCandidate.frameIndex);
            EXPECT_FLOAT_EQ(
                actualCandidate.timeSeconds, expectedCandidate.timeSeconds);
            EXPECT_FLOAT_EQ(actualCandidate.score, expectedCandidate.score);
            ExpectProfileEqual(actualCandidate.profile, expectedCandidate.profile);
        }
    };
    expectCandidates(actual.entryCandidates, expected.entryCandidates);
    expectCandidates(actual.exitCandidates, expected.exitCandidates);
}

int ReadIntegerPragma(
    const std::filesystem::path& databasePath,
    const char* pragma)
{
    sqlite3* database = nullptr;
    const std::string path = TestPathToUtf8(databasePath);
    if (sqlite3_open_v2(
            path.c_str(), &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK)
    {
        if (database) sqlite3_close_v2(database);
        return -1;
    }

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, pragma, -1, &statement, nullptr) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW ||
        sqlite3_column_type(statement, 0) != SQLITE_INTEGER)
    {
        if (statement) sqlite3_finalize(statement);
        sqlite3_close_v2(database);
        return -1;
    }
    const int result = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close_v2(database);
    return result;
}

bool CreateForgedAnalysisSchema(const std::filesystem::path& databasePath)
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
CREATE TABLE tracks(normalized_path TEXT, file_size INTEGER, write_time INTEGER,
    duration_seconds REAL);
CREATE TABLE frames(normalized_path TEXT, frame_index INTEGER, time_seconds REAL,
    energy REAL, rms REAL, loudness_lufs REAL, frame_true_peak_db REAL,
    spectral_centroid_hz REAL, spectral_flux REAL, onset_strength REAL,
    silence_probability REAL, energy_slope REAL);
CREATE TABLE candidates(normalized_path TEXT, role INTEGER, candidate_index INTEGER,
    kind INTEGER, frame_index INTEGER, time_seconds REAL, score REAL,
    profile_energy REAL, profile_loudness_lufs REAL,
    profile_spectral_centroid_hz REAL, profile_spectral_flux REAL,
    profile_onset_strength REAL);
CREATE INDEX candidates_by_track_frame
    ON candidates(normalized_path, frame_index);
PRAGMA application_id = 1414745409;
PRAGMA user_version = 1;
)sql";
    char* message = nullptr;
    const bool success = sqlite3_exec(
        database, schema, nullptr, nullptr, &message) == SQLITE_OK;
    if (message) sqlite3_free(message);
    sqlite3_close_v2(database);
    return success;
}

class MusicAnalysisStoreTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto stamp = std::chrono::high_resolution_clock::now()
            .time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() /
            ("tsm_music_analysis_store_" + std::to_string(stamp));
        ASSERT_TRUE(std::filesystem::create_directories(directory));
        databasePath = directory / "analysis.sqlite3";
        audioPath = directory / "cinema's; DROP TABLE tracks;--.wav";
        std::ofstream output(audioPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write("RIFF-analysis-fixture", 21);
        ASSERT_TRUE(output.good());
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    FileIdentity Identity() const
    {
        FileIdentity identity;
        std::string error;
        EXPECT_TRUE(MusicAnalysis::ReadFileIdentity(
            directory / "." / audioPath.filename(), identity, error)) << error;
        return identity;
    }

    std::filesystem::path directory;
    std::filesystem::path databasePath;
    std::filesystem::path audioPath;
};

TEST_F(MusicAnalysisStoreTests, InitializesVersionedSchemaAndRoundTripsBoundData)
{
    const FileIdentity identity = Identity();
    AnalysisStore store(databasePath);
    TrackAnalysis untouched;
    untouched.durationSeconds = 77.0f;
    std::string error;

    EXPECT_EQ(
        store.Load(identity, untouched, error), StoreLookupStatus::Unavailable);
    EXPECT_FALSE(error.empty());
    EXPECT_FLOAT_EQ(untouched.durationSeconds, 77.0f);

    ASSERT_TRUE(store.Initialize(error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(databasePath));
    EXPECT_EQ(
        ReadIntegerPragma(databasePath, "PRAGMA user_version"),
        AnalysisStore::CurrentSchemaVersion);
    EXPECT_EQ(
        ReadIntegerPragma(databasePath, "PRAGMA application_id"),
        0x54534D41);

    EXPECT_EQ(store.Load(identity, untouched, error), StoreLookupStatus::Miss);
    EXPECT_TRUE(error.empty());
    EXPECT_FLOAT_EQ(untouched.durationSeconds, 77.0f);

    const TrackAnalysis expected = SampleAnalysis();
    ASSERT_TRUE(store.Save(identity, expected, error)) << error;
    EXPECT_TRUE(error.empty());

    TrackAnalysis loaded;
    ASSERT_EQ(store.Load(identity, loaded, error), StoreLookupStatus::Hit) << error;
    EXPECT_TRUE(error.empty());
    ExpectAnalysisEqual(loaded, expected);
}

TEST_F(MusicAnalysisStoreTests, ReplacementCascadesRowsAndSurvivesReopen)
{
    const FileIdentity identity = Identity();
    const TrackAnalysis original = SampleAnalysis();
    std::string error;
    {
        AnalysisStore first(databasePath);
        ASSERT_TRUE(first.Initialize(error)) << error;
        ASSERT_TRUE(first.Save(identity, original, error)) << error;
    }

    TrackAnalysis replacement = original;
    replacement.durationSeconds = 1.0f;
    replacement.frames.resize(1);
    replacement.entryCandidates.clear();
    replacement.exitCandidates.clear();
    {
        AnalysisStore reopened(databasePath);
        ASSERT_TRUE(reopened.Initialize(error)) << error;
        TrackAnalysis persisted;
        ASSERT_EQ(
            reopened.Load(identity, persisted, error), StoreLookupStatus::Hit) << error;
        ExpectAnalysisEqual(persisted, original);
        ASSERT_TRUE(reopened.Save(identity, replacement, error)) << error;
    }

    AnalysisStore finalOpen(databasePath);
    ASSERT_TRUE(finalOpen.Initialize(error)) << error;
    TrackAnalysis loaded;
    ASSERT_EQ(
        finalOpen.Load(identity, loaded, error), StoreLookupStatus::Hit) << error;
    ExpectAnalysisEqual(loaded, replacement);
}

TEST_F(MusicAnalysisStoreTests, RejectsForgedVersionOneSchemaBeforeWalMode)
{
    ASSERT_TRUE(CreateForgedAnalysisSchema(databasePath));
    AnalysisStore store(databasePath);
    std::string error;
    EXPECT_FALSE(store.Initialize(error));
    EXPECT_FALSE(error.empty());
    std::filesystem::path walPath = databasePath;
    walPath += "-wal";
    EXPECT_FALSE(std::filesystem::exists(walPath));
}

TEST_F(MusicAnalysisStoreTests, InvalidatesEntriesWhenSizeOrWriteTimeChanges)
{
    AnalysisStore store(databasePath);
    std::string error;
    ASSERT_TRUE(store.Initialize(error)) << error;
    const FileIdentity original = Identity();
    ASSERT_TRUE(store.Save(original, SampleAnalysis(), error)) << error;

    {
        std::ofstream output(audioPath, std::ios::binary | std::ios::app);
        ASSERT_TRUE(output.is_open());
        output.put('x');
    }
    const FileIdentity resized = Identity();
    ASSERT_GT(resized.size, original.size);
    TrackAnalysis untouched;
    untouched.durationSeconds = 88.0f;
    EXPECT_EQ(store.Load(resized, untouched, error), StoreLookupStatus::Miss);
    EXPECT_NE(error.find("stale"), std::string::npos);
    EXPECT_FLOAT_EQ(untouched.durationSeconds, 88.0f);

    ASSERT_TRUE(store.Save(resized, SampleAnalysis(), error)) << error;
    const auto previousWriteTime = std::filesystem::last_write_time(audioPath);
    std::filesystem::last_write_time(
        audioPath, previousWriteTime + std::chrono::seconds(2));
    const FileIdentity retimed = Identity();
    ASSERT_EQ(retimed.size, resized.size);
    ASSERT_NE(retimed.writeTime, resized.writeTime);
    EXPECT_EQ(store.Load(retimed, untouched, error), StoreLookupStatus::Miss);
    EXPECT_NE(error.find("stale"), std::string::npos);
}

TEST_F(MusicAnalysisStoreTests, RejectedAnalysisCannotReplaceCommittedEntry)
{
    AnalysisStore store(databasePath);
    std::string error;
    ASSERT_TRUE(store.Initialize(error)) << error;
    const FileIdentity identity = Identity();
    const TrackAnalysis committed = SampleAnalysis();
    ASSERT_TRUE(store.Save(identity, committed, error)) << error;

    TrackAnalysis invalid = committed;
    invalid.frames[1].rms =
        (std::numeric_limits<float>::quiet_NaN)();
    EXPECT_FALSE(store.Save(identity, invalid, error));
    EXPECT_NE(error.find("invalid"), std::string::npos);

    TrackAnalysis loaded;
    ASSERT_EQ(store.Load(identity, loaded, error), StoreLookupStatus::Hit) << error;
    ExpectAnalysisEqual(loaded, committed);
}

TEST_F(MusicAnalysisStoreTests, ClearAtomicallyRemovesCachedAnalysis)
{
    AnalysisStore store(databasePath);
    std::string error;
    ASSERT_TRUE(store.Initialize(error)) << error;
    const FileIdentity identity = Identity();
    ASSERT_TRUE(store.Save(identity, SampleAnalysis(), error)) << error;

    ASSERT_TRUE(store.Clear(error)) << error;
    TrackAnalysis loaded;
    EXPECT_EQ(store.Load(identity, loaded, error), StoreLookupStatus::Miss);
    EXPECT_TRUE(error.empty());
}

TEST_F(MusicAnalysisStoreTests, CorruptDatabaseIsAnExplicitNonFatalUnavailableCache)
{
    {
        std::ofstream output(databasePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output << "this is not a SQLite database";
    }

    AnalysisStore store(databasePath);
    std::string error;
    EXPECT_FALSE(store.Initialize(error));
    EXPECT_FALSE(error.empty());

    TrackAnalysis untouched;
    untouched.durationSeconds = 99.0f;
    EXPECT_EQ(
        store.Load(Identity(), untouched, error),
        StoreLookupStatus::Unavailable);
    EXPECT_FALSE(error.empty());
    EXPECT_FLOAT_EQ(untouched.durationSeconds, 99.0f);
}

} // namespace
} // namespace TSM::Tests
