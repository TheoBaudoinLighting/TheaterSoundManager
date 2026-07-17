#include "tsm_music_analysis_store.h"

#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace TSM::MusicAnalysis
{
namespace
{

constexpr int ApplicationId = 0x54534D41; // "TSMA"
constexpr int BusyTimeoutMilliseconds = 5000;
constexpr std::size_t MaximumPathBytes = 1024U * 1024U;
constexpr std::size_t MaximumFrameCount = 10'000'000U;
constexpr std::size_t MaximumCandidatesPerKind = 1'000'000U;

struct DatabaseCloser
{
    void operator()(sqlite3* database) const noexcept
    {
        if (database) sqlite3_close_v2(database);
    }
};

using DatabaseHandle = std::unique_ptr<sqlite3, DatabaseCloser>;

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(value.data()), value.size());
}

void SetSqliteError(
    std::string& errorMessage,
    std::string_view context,
    sqlite3* database,
    int result)
{
    const int code = database ? sqlite3_extended_errcode(database) : result;
    const char* message = database ? sqlite3_errmsg(database) : sqlite3_errstr(result);
    errorMessage = std::string(context) + " (SQLite " + std::to_string(code) + "): " +
        (message ? message : "unknown database error");
}

bool Execute(
    sqlite3* database,
    const char* sql,
    std::string_view context,
    std::string& errorMessage)
{
    char* sqliteMessage = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &sqliteMessage);
    if (result == SQLITE_OK) return true;

    if (sqliteMessage)
    {
        errorMessage = std::string(context) + " (SQLite " +
            std::to_string(sqlite3_extended_errcode(database)) + "): " + sqliteMessage;
        sqlite3_free(sqliteMessage);
    }
    else
    {
        SetSqliteError(errorMessage, context, database, result);
    }
    return false;
}

class Statement final
{
public:
    Statement() = default;
    ~Statement()
    {
        if (m_statement) sqlite3_finalize(m_statement);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool Prepare(
        sqlite3* database,
        const char* sql,
        std::string_view context,
        std::string& errorMessage)
    {
        m_database = database;
        const int result = sqlite3_prepare_v2(
            database, sql, -1, &m_statement, nullptr);
        if (result == SQLITE_OK) return true;
        SetSqliteError(errorMessage, context, database, result);
        return false;
    }

    bool BindText(
        int index,
        const std::string& value,
        std::string_view context,
        std::string& errorMessage)
    {
        if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        {
            errorMessage = std::string(context) + ": text parameter is too large.";
            return false;
        }
        const int result = sqlite3_bind_text(
            m_statement,
            index,
            value.data(),
            static_cast<int>(value.size()),
            SQLITE_TRANSIENT);
        if (result == SQLITE_OK) return true;
        SetSqliteError(errorMessage, context, m_database, result);
        return false;
    }

    bool BindInt64(
        int index,
        std::int64_t value,
        std::string_view context,
        std::string& errorMessage)
    {
        const int result = sqlite3_bind_int64(
            m_statement, index, static_cast<sqlite3_int64>(value));
        if (result == SQLITE_OK) return true;
        SetSqliteError(errorMessage, context, m_database, result);
        return false;
    }

    bool BindDouble(
        int index,
        double value,
        std::string_view context,
        std::string& errorMessage)
    {
        const int result = sqlite3_bind_double(m_statement, index, value);
        if (result == SQLITE_OK) return true;
        SetSqliteError(errorMessage, context, m_database, result);
        return false;
    }

    bool StepDone(std::string_view context, std::string& errorMessage)
    {
        const int result = sqlite3_step(m_statement);
        if (result == SQLITE_DONE) return true;
        SetSqliteError(errorMessage, context, m_database, result);
        return false;
    }

    bool Reset(std::string_view context, std::string& errorMessage)
    {
        const int resetResult = sqlite3_reset(m_statement);
        if (resetResult != SQLITE_OK)
        {
            SetSqliteError(errorMessage, context, m_database, resetResult);
            return false;
        }
        const int clearResult = sqlite3_clear_bindings(m_statement);
        if (clearResult == SQLITE_OK) return true;
        SetSqliteError(errorMessage, context, m_database, clearResult);
        return false;
    }

    sqlite3_stmt* Get() const { return m_statement; }

private:
    sqlite3* m_database = nullptr;
    sqlite3_stmt* m_statement = nullptr;
};

class Transaction final
{
public:
    Transaction(
        sqlite3* database,
        const char* beginSql,
        std::string& errorMessage)
        : m_database(database),
          m_active(Execute(
              database, beginSql, "Unable to begin music analysis transaction",
              errorMessage))
    {
    }

    ~Transaction()
    {
        if (m_active)
        {
            char* ignored = nullptr;
            sqlite3_exec(m_database, "ROLLBACK", nullptr, nullptr, &ignored);
            if (ignored) sqlite3_free(ignored);
        }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    bool IsActive() const { return m_active; }

    bool Commit(std::string& errorMessage)
    {
        if (!m_active) return false;
        if (!Execute(
                m_database, "COMMIT", "Unable to commit music analysis transaction",
                errorMessage))
            return false;
        m_active = false;
        return true;
    }

private:
    sqlite3* m_database = nullptr;
    bool m_active = false;
};

bool QuerySingleInteger(
    sqlite3* database,
    const char* sql,
    std::int64_t& value,
    std::string_view context,
    std::string& errorMessage)
{
    Statement statement;
    if (!statement.Prepare(database, sql, context, errorMessage)) return false;
    int result = sqlite3_step(statement.Get());
    if (result != SQLITE_ROW ||
        sqlite3_column_type(statement.Get(), 0) != SQLITE_INTEGER)
    {
        if (result == SQLITE_ROW)
            errorMessage = std::string(context) + ": expected one integer value.";
        else
            SetSqliteError(errorMessage, context, database, result);
        return false;
    }
    value = static_cast<std::int64_t>(sqlite3_column_int64(statement.Get(), 0));
    result = sqlite3_step(statement.Get());
    if (result == SQLITE_DONE) return true;
    if (result == SQLITE_ROW)
        errorMessage = std::string(context) + ": returned more than one row.";
    else
        SetSqliteError(errorMessage, context, database, result);
    return false;
}

std::string CompactSql(std::string_view sql)
{
    std::string compact;
    compact.reserve(sql.size());
    for (const unsigned char character : sql)
    {
        if (!std::isspace(character))
            compact.push_back(static_cast<char>(std::tolower(character)));
    }
    return compact;
}

bool RequireSchemaDefinition(
    sqlite3* database,
    const std::string& type,
    const std::string& name,
    std::initializer_list<std::string_view> requiredFragments,
    std::string& errorMessage)
{
    Statement statement;
    if (!statement.Prepare(
            database,
            "SELECT sql FROM sqlite_master WHERE type = ?1 AND name = ?2",
            "Unable to inspect music analysis schema", errorMessage) ||
        !statement.BindText(
            1, type, "Unable to bind schema object type", errorMessage) ||
        !statement.BindText(
            2, name, "Unable to bind schema object name", errorMessage))
        return false;
    int result = sqlite3_step(statement.Get());
    if (result != SQLITE_ROW ||
        sqlite3_column_type(statement.Get(), 0) != SQLITE_TEXT)
    {
        errorMessage = "Music analysis schema object '" + name + "' is missing.";
        return false;
    }
    const auto* raw = reinterpret_cast<const char*>(
        sqlite3_column_text(statement.Get(), 0));
    const std::string definition = CompactSql(raw ? raw : "");
    result = sqlite3_step(statement.Get());
    if (result != SQLITE_DONE)
    {
        errorMessage = "Music analysis schema object '" + name +
            "' is duplicated or unreadable.";
        return false;
    }
    for (const std::string_view fragment : requiredFragments)
    {
        if (definition.find(fragment) == std::string::npos)
        {
            errorMessage = "Music analysis schema object '" + name +
                "' is missing a required constraint.";
            return false;
        }
    }
    return true;
}

bool ValidatePrimaryKey(
    sqlite3* database,
    const char* tableInfoSql,
    std::initializer_list<std::pair<std::string_view, int>> expected,
    std::string_view tableName,
    std::string& errorMessage)
{
    Statement statement;
    if (!statement.Prepare(
            database, tableInfoSql, "Unable to inspect primary key", errorMessage))
        return false;
    std::vector<bool> found(expected.size(), false);
    while (true)
    {
        const int result = sqlite3_step(statement.Get());
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW ||
            sqlite3_column_type(statement.Get(), 1) != SQLITE_TEXT ||
            sqlite3_column_type(statement.Get(), 5) != SQLITE_INTEGER)
        {
            errorMessage = "Primary key metadata for '" +
                std::string(tableName) + "' is malformed.";
            return false;
        }
        const int rank = sqlite3_column_int(statement.Get(), 5);
        if (rank == 0) continue;
        const auto* rawName = reinterpret_cast<const char*>(
            sqlite3_column_text(statement.Get(), 1));
        const std::string_view columnName(rawName ? rawName : "");
        bool matched = false;
        std::size_t index = 0;
        for (const auto& [expectedName, expectedRank] : expected)
        {
            if (columnName == expectedName && rank == expectedRank)
            {
                found[index] = true;
                matched = true;
                break;
            }
            ++index;
        }
        if (!matched)
        {
            errorMessage = "Table '" + std::string(tableName) +
                "' has an unexpected primary key.";
            return false;
        }
    }
    if (std::all_of(found.begin(), found.end(), [](bool value) { return value; }))
        return true;
    errorMessage = "Table '" + std::string(tableName) +
        "' is missing its required primary key.";
    return false;
}

bool ValidateIndexColumns(
    sqlite3* database,
    const char* indexInfoSql,
    std::initializer_list<std::string_view> expected,
    std::string_view indexName,
    std::string& errorMessage)
{
    Statement statement;
    if (!statement.Prepare(
            database, indexInfoSql, "Unable to inspect analysis index", errorMessage))
        return false;
    std::size_t index = 0;
    for (const std::string_view expectedName : expected)
    {
        const int result = sqlite3_step(statement.Get());
        if (result != SQLITE_ROW ||
            sqlite3_column_type(statement.Get(), 2) != SQLITE_TEXT)
        {
            errorMessage = "Analysis index '" + std::string(indexName) +
                "' is incomplete.";
            return false;
        }
        const auto* rawName = reinterpret_cast<const char*>(
            sqlite3_column_text(statement.Get(), 2));
        if (std::string_view(rawName ? rawName : "") != expectedName ||
            sqlite3_column_int(statement.Get(), 0) != static_cast<int>(index))
        {
            errorMessage = "Analysis index '" + std::string(indexName) +
                "' has unexpected columns.";
            return false;
        }
        ++index;
    }
    if (sqlite3_step(statement.Get()) == SQLITE_DONE) return true;
    errorMessage = "Analysis index '" + std::string(indexName) +
        "' has extra columns.";
    return false;
}

bool VerifyIntegrity(sqlite3* database, std::string& errorMessage)
{
    Statement statement;
    if (!statement.Prepare(
            database, "PRAGMA quick_check(1)",
            "Unable to check music analysis database integrity", errorMessage))
        return false;

    int result = sqlite3_step(statement.Get());
    if (result != SQLITE_ROW ||
        sqlite3_column_type(statement.Get(), 0) != SQLITE_TEXT)
    {
        SetSqliteError(
            errorMessage, "Music analysis database integrity check failed",
            database, result);
        return false;
    }
    const auto* text = reinterpret_cast<const char*>(
        sqlite3_column_text(statement.Get(), 0));
    if (!text || std::string_view(text) != "ok")
    {
        errorMessage = "Music analysis database is corrupt: " +
            std::string(text ? text : "integrity check returned no detail");
        return false;
    }
    result = sqlite3_step(statement.Get());
    if (result == SQLITE_DONE) return true;
    errorMessage = "Music analysis database is corrupt: integrity check returned "
        "multiple results.";
    return false;
}

bool VerifyForeignKeys(sqlite3* database, std::string& errorMessage)
{
    Statement statement;
    if (!statement.Prepare(
            database, "PRAGMA foreign_key_check",
            "Unable to check music analysis database references", errorMessage))
        return false;
    const int result = sqlite3_step(statement.Get());
    if (result == SQLITE_DONE) return true;
    if (result == SQLITE_ROW)
        errorMessage =
            "Music analysis database is corrupt: a cached row has a broken reference.";
    else
        SetSqliteError(
            errorMessage, "Music analysis database reference check failed",
            database, result);
    return false;
}

bool ValidateSchema(sqlite3* database, std::string& errorMessage)
{
    constexpr const char* queries[] = {
        "SELECT normalized_path, file_size, write_time, duration_seconds "
        "FROM tracks LIMIT 0",
        "SELECT normalized_path, frame_index, time_seconds, energy, rms, "
        "loudness_lufs, frame_true_peak_db, spectral_centroid_hz, spectral_flux, "
        "onset_strength, silence_probability, energy_slope FROM frames LIMIT 0",
        "SELECT normalized_path, role, candidate_index, kind, frame_index, "
        "time_seconds, score, profile_energy, profile_loudness_lufs, "
        "profile_spectral_centroid_hz, profile_spectral_flux, "
        "profile_onset_strength FROM candidates LIMIT 0"
    };
    for (const char* query : queries)
    {
        Statement statement;
        if (!statement.Prepare(
                database, query, "Music analysis database schema is invalid",
                errorMessage))
            return false;
    }
    return RequireSchemaDefinition(
               database, "table", "tracks",
               {"primarykey", "check(file_size>=0)",
                "check(duration_seconds>=0)", "withoutrowid"},
               errorMessage) &&
        RequireSchemaDefinition(
               database, "table", "frames",
               {"check(frame_index>=0)",
                "primarykey(normalized_path,frame_index)",
                "foreignkey(normalized_path)referencestracks(normalized_path)"
                "ondeletecascade",
                "withoutrowid"},
               errorMessage) &&
        RequireSchemaDefinition(
               database, "table", "candidates",
               {"check(rolein(0,1))", "check(candidate_index>=0)",
                "check(kindin(0,1))", "check(frame_index>=0)",
                "primarykey(normalized_path,role,candidate_index)",
                "foreignkey(normalized_path)referencestracks(normalized_path)"
                "ondeletecascade",
                "foreignkey(normalized_path,frame_index)referencesframes"
                "(normalized_path,frame_index)ondeletecascade",
                "withoutrowid"},
               errorMessage) &&
        RequireSchemaDefinition(
               database, "index", "candidates_by_track_frame",
               {"oncandidates(normalized_path,frame_index)"}, errorMessage) &&
        ValidatePrimaryKey(
               database, "PRAGMA table_info(tracks)",
               {{"normalized_path", 1}}, "tracks", errorMessage) &&
        ValidatePrimaryKey(
               database, "PRAGMA table_info(frames)",
               {{"normalized_path", 1}, {"frame_index", 2}},
               "frames", errorMessage) &&
        ValidatePrimaryKey(
               database, "PRAGMA table_info(candidates)",
               {{"normalized_path", 1}, {"role", 2}, {"candidate_index", 3}},
               "candidates", errorMessage) &&
        ValidateIndexColumns(
               database, "PRAGMA index_info(candidates_by_track_frame)",
               {"normalized_path", "frame_index"},
               "candidates_by_track_frame", errorMessage);
}

bool EnsureSchema(sqlite3* database, std::string& errorMessage)
{
    Transaction transaction(database, "BEGIN IMMEDIATE", errorMessage);
    if (!transaction.IsActive()) return false;

    std::int64_t version = 0;
    std::int64_t applicationId = 0;
    if (!QuerySingleInteger(
            database, "PRAGMA user_version", version,
            "Unable to read music analysis schema version", errorMessage) ||
        !QuerySingleInteger(
            database, "PRAGMA application_id", applicationId,
            "Unable to identify music analysis database", errorMessage))
        return false;

    if (version == 0)
    {
        std::int64_t userTableCount = 0;
        if (applicationId != 0)
        {
            errorMessage =
                "Refusing to initialize a database owned by another application as "
                "the music analysis cache.";
            return false;
        }
        if (!QuerySingleInteger(
                database,
                "SELECT count(*) FROM sqlite_master WHERE type = 'table' "
                "AND name NOT LIKE 'sqlite_%'",
                userTableCount,
                "Unable to inspect unversioned music analysis database",
                errorMessage))
            return false;
        if (userTableCount != 0)
        {
            errorMessage =
                "Refusing to initialize an unversioned, non-empty database as the "
                "music analysis cache.";
            return false;
        }

        constexpr const char* schema = R"sql(
CREATE TABLE tracks (
    normalized_path TEXT PRIMARY KEY NOT NULL,
    file_size INTEGER NOT NULL CHECK(file_size >= 0),
    write_time INTEGER NOT NULL,
    duration_seconds REAL NOT NULL CHECK(duration_seconds >= 0)
) WITHOUT ROWID;
CREATE TABLE frames (
    normalized_path TEXT NOT NULL,
    frame_index INTEGER NOT NULL CHECK(frame_index >= 0),
    time_seconds REAL NOT NULL,
    energy REAL NOT NULL,
    rms REAL NOT NULL,
    loudness_lufs REAL NOT NULL,
    frame_true_peak_db REAL NOT NULL,
    spectral_centroid_hz REAL NOT NULL,
    spectral_flux REAL NOT NULL,
    onset_strength REAL NOT NULL,
    silence_probability REAL NOT NULL,
    energy_slope REAL NOT NULL,
    PRIMARY KEY(normalized_path, frame_index),
    FOREIGN KEY(normalized_path) REFERENCES tracks(normalized_path) ON DELETE CASCADE
) WITHOUT ROWID;
CREATE TABLE candidates (
    normalized_path TEXT NOT NULL,
    role INTEGER NOT NULL CHECK(role IN (0, 1)),
    candidate_index INTEGER NOT NULL CHECK(candidate_index >= 0),
    kind INTEGER NOT NULL CHECK(kind IN (0, 1)),
    frame_index INTEGER NOT NULL CHECK(frame_index >= 0),
    time_seconds REAL NOT NULL,
    score REAL NOT NULL,
    profile_energy REAL NOT NULL,
    profile_loudness_lufs REAL NOT NULL,
    profile_spectral_centroid_hz REAL NOT NULL,
    profile_spectral_flux REAL NOT NULL,
    profile_onset_strength REAL NOT NULL,
    PRIMARY KEY(normalized_path, role, candidate_index),
    FOREIGN KEY(normalized_path) REFERENCES tracks(normalized_path) ON DELETE CASCADE,
    FOREIGN KEY(normalized_path, frame_index)
        REFERENCES frames(normalized_path, frame_index) ON DELETE CASCADE
) WITHOUT ROWID;
CREATE INDEX candidates_by_track_frame
    ON candidates(normalized_path, frame_index);
PRAGMA application_id = 1414745409;
PRAGMA user_version = 1;
)sql";
        if (!Execute(
                database, schema, "Unable to create music analysis database schema",
                errorMessage))
            return false;
    }
    else if (version != AnalysisStore::CurrentSchemaVersion ||
             applicationId != ApplicationId)
    {
        errorMessage = "Unsupported music analysis database schema (version " +
            std::to_string(version) + ", application id " +
            std::to_string(applicationId) + ").";
        return false;
    }

    return transaction.Commit(errorMessage);
}

bool IsFinite(float value)
{
    return std::isfinite(value);
}

bool IsUnitValue(float value)
{
    return IsFinite(value) && value >= 0.0f && value <= 1.0f;
}

bool ValidateProfile(const Profile& profile)
{
    return IsUnitValue(profile.energy) && IsFinite(profile.loudnessLufs) &&
        IsFinite(profile.spectralCentroidHz) && profile.spectralCentroidHz >= 0.0f &&
        IsUnitValue(profile.spectralFlux) && IsUnitValue(profile.onsetStrength);
}

bool ValidateAnalysis(const TrackAnalysis& analysis, std::string& reason)
{
    if (!IsFinite(analysis.durationSeconds) || analysis.durationSeconds < 0.0f)
    {
        reason = "track duration must be finite and non-negative";
        return false;
    }
    if (analysis.frames.size() > MaximumFrameCount ||
        analysis.entryCandidates.size() > MaximumCandidatesPerKind ||
        analysis.exitCandidates.size() > MaximumCandidatesPerKind)
    {
        reason = "analysis exceeds the supported cache row limit";
        return false;
    }

    float previousTime = -1.0f;
    for (const Frame& frame : analysis.frames)
    {
        if (!IsFinite(frame.timeSeconds) || frame.timeSeconds < 0.0f ||
            frame.timeSeconds > analysis.durationSeconds ||
            frame.timeSeconds < previousTime || !IsUnitValue(frame.energy) ||
            !IsUnitValue(frame.rms) ||
            !IsFinite(frame.loudnessLufs) || !IsFinite(frame.truePeakDb) ||
            !IsFinite(frame.spectralCentroidHz) || frame.spectralCentroidHz < 0.0f ||
            !IsUnitValue(frame.spectralFlux) || !IsUnitValue(frame.onsetStrength) ||
            !IsUnitValue(frame.silenceProbability) || !IsFinite(frame.energySlope) ||
            frame.energySlope < -1.0f || frame.energySlope > 1.0f)
        {
            reason = "analysis contains an invalid or unordered frame";
            return false;
        }
        previousTime = frame.timeSeconds;
    }

    const auto validateCandidates = [&analysis, &reason](
                                        const std::vector<Candidate>& candidates,
                                        CandidateKind expectedKind) {
        for (const Candidate& candidate : candidates)
        {
            if (candidate.kind != expectedKind ||
                candidate.frameIndex >= analysis.frames.size() ||
                !IsFinite(candidate.timeSeconds) || candidate.timeSeconds < 0.0f ||
                candidate.timeSeconds > analysis.durationSeconds ||
                !IsFinite(candidate.score) || !ValidateProfile(candidate.profile))
            {
                reason = "analysis contains an invalid candidate";
                return false;
            }
        }
        return true;
    };
    return validateCandidates(analysis.entryCandidates, CandidateKind::Entry) &&
        validateCandidates(analysis.exitCandidates, CandidateKind::Exit);
}

bool ValidateIdentity(const FileIdentity& identity, std::string& errorMessage)
{
    if (identity.normalizedPath.empty() ||
        identity.normalizedPath.size() > MaximumPathBytes ||
        identity.normalizedPath.find('\0') != std::string::npos)
    {
        errorMessage = "Music analysis file identity has an invalid normalized path.";
        return false;
    }
    if (identity.size > static_cast<std::uint64_t>(
                            (std::numeric_limits<std::int64_t>::max)()))
    {
        errorMessage = "Music analysis file is too large for the cache metadata.";
        return false;
    }
    return true;
}

bool ReadFloatColumn(sqlite3_stmt* statement, int column, float& value)
{
    const int type = sqlite3_column_type(statement, column);
    if (type != SQLITE_FLOAT && type != SQLITE_INTEGER) return false;
    const double parsed = sqlite3_column_double(statement, column);
    if (!std::isfinite(parsed) ||
        parsed < static_cast<double>((std::numeric_limits<float>::lowest)()) ||
        parsed > static_cast<double>((std::numeric_limits<float>::max)()))
        return false;
    value = static_cast<float>(parsed);
    return std::isfinite(value);
}

bool ReadIndexColumn(sqlite3_stmt* statement, int column, std::size_t& value)
{
    if (sqlite3_column_type(statement, column) != SQLITE_INTEGER) return false;
    const sqlite3_int64 parsed = sqlite3_column_int64(statement, column);
    if (parsed < 0 || static_cast<std::uint64_t>(parsed) >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
        return false;
    value = static_cast<std::size_t>(parsed);
    return true;
}

bool LoadFrames(
    sqlite3* database,
    const std::string& normalizedPath,
    TrackAnalysis& analysis,
    std::string& errorMessage)
{
    Statement statement;
    if (!statement.Prepare(
            database,
            "SELECT frame_index, time_seconds, energy, rms, loudness_lufs, "
            "frame_true_peak_db, spectral_centroid_hz, spectral_flux, onset_strength, "
            "silence_probability, energy_slope FROM frames "
            "WHERE normalized_path = ?1 ORDER BY frame_index",
            "Unable to load music analysis frames", errorMessage) ||
        !statement.BindText(
            1, normalizedPath, "Unable to bind music analysis path", errorMessage))
        return false;

    while (true)
    {
        const int result = sqlite3_step(statement.Get());
        if (result == SQLITE_DONE) return true;
        if (result != SQLITE_ROW)
        {
            SetSqliteError(
                errorMessage, "Unable to read music analysis frames", database, result);
            return false;
        }
        if (analysis.frames.size() >= MaximumFrameCount)
        {
            errorMessage = "Music analysis cache contains too many frames.";
            return false;
        }

        std::size_t frameIndex = 0;
        Frame frame;
        if (!ReadIndexColumn(statement.Get(), 0, frameIndex) ||
            frameIndex != analysis.frames.size() ||
            !ReadFloatColumn(statement.Get(), 1, frame.timeSeconds) ||
            !ReadFloatColumn(statement.Get(), 2, frame.energy) ||
            !ReadFloatColumn(statement.Get(), 3, frame.rms) ||
            !ReadFloatColumn(statement.Get(), 4, frame.loudnessLufs) ||
            !ReadFloatColumn(statement.Get(), 5, frame.truePeakDb) ||
            !ReadFloatColumn(statement.Get(), 6, frame.spectralCentroidHz) ||
            !ReadFloatColumn(statement.Get(), 7, frame.spectralFlux) ||
            !ReadFloatColumn(statement.Get(), 8, frame.onsetStrength) ||
            !ReadFloatColumn(statement.Get(), 9, frame.silenceProbability) ||
            !ReadFloatColumn(statement.Get(), 10, frame.energySlope))
        {
            errorMessage = "Music analysis cache contains a malformed frame row.";
            return false;
        }
        analysis.frames.push_back(frame);
    }
}

bool LoadCandidates(
    sqlite3* database,
    const std::string& normalizedPath,
    int role,
    CandidateKind expectedKind,
    std::vector<Candidate>& candidates,
    std::string& errorMessage)
{
    Statement statement;
    if (!statement.Prepare(
            database,
            "SELECT candidate_index, kind, frame_index, time_seconds, score, "
            "profile_energy, profile_loudness_lufs, profile_spectral_centroid_hz, "
            "profile_spectral_flux, profile_onset_strength FROM candidates "
            "WHERE normalized_path = ?1 AND role = ?2 ORDER BY candidate_index",
            "Unable to load music analysis candidates", errorMessage) ||
        !statement.BindText(
            1, normalizedPath, "Unable to bind music analysis path", errorMessage) ||
        !statement.BindInt64(
            2, role, "Unable to bind candidate role", errorMessage))
        return false;

    while (true)
    {
        const int result = sqlite3_step(statement.Get());
        if (result == SQLITE_DONE) return true;
        if (result != SQLITE_ROW)
        {
            SetSqliteError(
                errorMessage, "Unable to read music analysis candidates",
                database, result);
            return false;
        }
        if (candidates.size() >= MaximumCandidatesPerKind)
        {
            errorMessage = "Music analysis cache contains too many candidates.";
            return false;
        }

        std::size_t candidateIndex = 0;
        std::size_t frameIndex = 0;
        if (!ReadIndexColumn(statement.Get(), 0, candidateIndex) ||
            candidateIndex != candidates.size() ||
            sqlite3_column_type(statement.Get(), 1) != SQLITE_INTEGER ||
            !ReadIndexColumn(statement.Get(), 2, frameIndex))
        {
            errorMessage = "Music analysis cache contains a malformed candidate row.";
            return false;
        }
        const int kind = sqlite3_column_int(statement.Get(), 1);
        if (kind != static_cast<int>(expectedKind))
        {
            errorMessage = "Music analysis cache candidate kind does not match its role.";
            return false;
        }

        Candidate candidate;
        candidate.kind = expectedKind;
        candidate.frameIndex = frameIndex;
        if (!ReadFloatColumn(statement.Get(), 3, candidate.timeSeconds) ||
            !ReadFloatColumn(statement.Get(), 4, candidate.score) ||
            !ReadFloatColumn(statement.Get(), 5, candidate.profile.energy) ||
            !ReadFloatColumn(statement.Get(), 6, candidate.profile.loudnessLufs) ||
            !ReadFloatColumn(
                statement.Get(), 7, candidate.profile.spectralCentroidHz) ||
            !ReadFloatColumn(statement.Get(), 8, candidate.profile.spectralFlux) ||
            !ReadFloatColumn(statement.Get(), 9, candidate.profile.onsetStrength))
        {
            errorMessage = "Music analysis cache contains malformed candidate values.";
            return false;
        }
        candidates.push_back(candidate);
    }
}

bool BindFrame(
    Statement& statement,
    const std::string& path,
    std::size_t index,
    const Frame& frame,
    std::string& errorMessage)
{
    constexpr std::string_view context = "Unable to bind music analysis frame";
    return statement.BindText(1, path, context, errorMessage) &&
        statement.BindInt64(2, static_cast<std::int64_t>(index), context, errorMessage) &&
        statement.BindDouble(3, frame.timeSeconds, context, errorMessage) &&
        statement.BindDouble(4, frame.energy, context, errorMessage) &&
        statement.BindDouble(5, frame.rms, context, errorMessage) &&
        statement.BindDouble(6, frame.loudnessLufs, context, errorMessage) &&
        statement.BindDouble(7, frame.truePeakDb, context, errorMessage) &&
        statement.BindDouble(8, frame.spectralCentroidHz, context, errorMessage) &&
        statement.BindDouble(9, frame.spectralFlux, context, errorMessage) &&
        statement.BindDouble(10, frame.onsetStrength, context, errorMessage) &&
        statement.BindDouble(11, frame.silenceProbability, context, errorMessage) &&
        statement.BindDouble(12, frame.energySlope, context, errorMessage);
}

bool BindCandidate(
    Statement& statement,
    const std::string& path,
    int role,
    std::size_t index,
    const Candidate& candidate,
    std::string& errorMessage)
{
    constexpr std::string_view context = "Unable to bind music analysis candidate";
    return statement.BindText(1, path, context, errorMessage) &&
        statement.BindInt64(2, role, context, errorMessage) &&
        statement.BindInt64(3, static_cast<std::int64_t>(index), context, errorMessage) &&
        statement.BindInt64(
            4, static_cast<int>(candidate.kind), context, errorMessage) &&
        statement.BindInt64(
            5, static_cast<std::int64_t>(candidate.frameIndex), context, errorMessage) &&
        statement.BindDouble(6, candidate.timeSeconds, context, errorMessage) &&
        statement.BindDouble(7, candidate.score, context, errorMessage) &&
        statement.BindDouble(8, candidate.profile.energy, context, errorMessage) &&
        statement.BindDouble(9, candidate.profile.loudnessLufs, context, errorMessage) &&
        statement.BindDouble(
            10, candidate.profile.spectralCentroidHz, context, errorMessage) &&
        statement.BindDouble(11, candidate.profile.spectralFlux, context, errorMessage) &&
        statement.BindDouble(12, candidate.profile.onsetStrength, context, errorMessage);
}

} // namespace

struct AnalysisStore::Impl
{
    explicit Impl(std::filesystem::path path) : databasePath(std::move(path)) {}

    ~Impl()
    {
        if (database) sqlite3_close_v2(database);
    }

    std::filesystem::path databasePath;
    sqlite3* database = nullptr;
    std::mutex mutex;
};

bool ReadFileIdentity(
    const std::filesystem::path& path,
    FileIdentity& identity,
    std::string& errorMessage) noexcept
{
    errorMessage.clear();
    try
    {
        if (path.empty())
        {
            errorMessage = "Cannot identify an empty music file path.";
            return false;
        }

        std::error_code error;
        const std::filesystem::path canonical = std::filesystem::canonical(path, error);
        if (error)
        {
            errorMessage = "Unable to normalize music file path '" + PathToUtf8(path) +
                "': " + error.message();
            return false;
        }
        if (!std::filesystem::is_regular_file(canonical, error) || error)
        {
            errorMessage = "Music analysis source is not a regular file: '" +
                PathToUtf8(canonical) + "'.";
            return false;
        }

        const std::uintmax_t size = std::filesystem::file_size(canonical, error);
        if (error || !std::in_range<std::uint64_t>(size))
        {
            errorMessage = "Unable to read music file size for '" +
                PathToUtf8(canonical) + "': " + error.message();
            return false;
        }
        const auto writeTime = std::filesystem::last_write_time(canonical, error);
        if (error)
        {
            errorMessage = "Unable to read music file write time for '" +
                PathToUtf8(canonical) + "': " + error.message();
            return false;
        }
        const auto ticks = writeTime.time_since_epoch().count();
        if (!std::in_range<std::int64_t>(ticks))
        {
            errorMessage = "Music file write time cannot be represented by the cache.";
            return false;
        }

        FileIdentity resolved;
        resolved.normalizedPath = PathToUtf8(canonical.lexically_normal());
        resolved.size = static_cast<std::uint64_t>(size);
        resolved.writeTime = static_cast<std::int64_t>(ticks);
        if (!ValidateIdentity(resolved, errorMessage)) return false;
        identity = std::move(resolved);
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage =
            "Unable to identify music file for analysis cache: " +
            std::string(exception.what());
        return false;
    }
    catch (...)
    {
        errorMessage = "Unable to identify music file for analysis cache: unknown error.";
        return false;
    }
}

AnalysisStore::AnalysisStore(std::filesystem::path databasePath)
    : m_impl(std::make_unique<Impl>(std::move(databasePath)))
{
}

AnalysisStore::~AnalysisStore() = default;

bool AnalysisStore::Initialize(std::string& errorMessage) noexcept
{
    errorMessage.clear();
    try
    {
        std::lock_guard lock(m_impl->mutex);
        if (m_impl->database) return true;
        if (m_impl->databasePath.empty())
        {
            errorMessage = "Music analysis database path is empty.";
            return false;
        }

        const std::filesystem::path parent = m_impl->databasePath.parent_path();
        if (!parent.empty())
        {
            std::error_code directoryError;
            std::filesystem::create_directories(parent, directoryError);
            if (directoryError)
            {
                errorMessage = "Unable to create music analysis database directory: " +
                    directoryError.message();
                return false;
            }
        }

        const std::string databasePath = PathToUtf8(
            std::filesystem::absolute(m_impl->databasePath).lexically_normal());
        sqlite3* database = nullptr;
        const int openResult = sqlite3_open_v2(
            databasePath.c_str(), &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr);
        DatabaseHandle databaseHandle(database);
        if (openResult != SQLITE_OK)
        {
            SetSqliteError(
                errorMessage, "Unable to open music analysis database", database,
                openResult);
            return false;
        }

        const int extendedResult = sqlite3_extended_result_codes(database, 1);
        if (extendedResult != SQLITE_OK)
        {
            SetSqliteError(
                errorMessage,
                "Unable to enable detailed music analysis database errors",
                database, extendedResult);
            return false;
        }
        const int timeoutResult = sqlite3_busy_timeout(
            database, BusyTimeoutMilliseconds);
        if (timeoutResult != SQLITE_OK)
        {
            SetSqliteError(
                errorMessage, "Unable to configure music analysis database timeout",
                database, timeoutResult);
            return false;
        }
        if (!Execute(
                database, "PRAGMA foreign_keys = ON",
                "Unable to enable music analysis database references", errorMessage) ||
            !EnsureSchema(database, errorMessage) ||
            !ValidateSchema(database, errorMessage) ||
            !VerifyIntegrity(database, errorMessage) ||
            !VerifyForeignKeys(database, errorMessage) ||
            !Execute(
                database, "PRAGMA journal_mode = WAL",
                "Unable to enable music analysis write-ahead logging", errorMessage) ||
            !Execute(
                database, "PRAGMA synchronous = FULL",
                "Unable to configure durable music analysis writes", errorMessage))
        {
            return false;
        }

        int defensiveEnabled = 0;
        const int defensiveResult = sqlite3_db_config(
            database, SQLITE_DBCONFIG_DEFENSIVE, 1, &defensiveEnabled);
        if (defensiveResult != SQLITE_OK || defensiveEnabled != 1)
        {
            SetSqliteError(
                errorMessage, "Unable to harden music analysis database connection",
                database, defensiveResult);
            return false;
        }

        m_impl->database = databaseHandle.release();
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Unable to initialize music analysis database: " +
            std::string(exception.what());
        return false;
    }
    catch (...)
    {
        errorMessage = "Unable to initialize music analysis database: unknown error.";
        return false;
    }
}

StoreLookupStatus AnalysisStore::Load(
    const FileIdentity& identity,
    TrackAnalysis& analysis,
    std::string& errorMessage) noexcept
{
    errorMessage.clear();
    try
    {
        if (!ValidateIdentity(identity, errorMessage))
            return StoreLookupStatus::Unavailable;

        std::lock_guard lock(m_impl->mutex);
        sqlite3* const database = m_impl->database;
        if (!database)
        {
            errorMessage =
                "Music analysis database is unavailable because Initialize has not "
                "completed successfully.";
            return StoreLookupStatus::Unavailable;
        }

        Transaction transaction(database, "BEGIN", errorMessage);
        if (!transaction.IsActive()) return StoreLookupStatus::Unavailable;

        Statement track;
        if (!track.Prepare(
                database,
                "SELECT file_size, write_time, duration_seconds FROM tracks "
                "WHERE normalized_path = ?1",
                "Unable to load music analysis track", errorMessage) ||
            !track.BindText(
                1, identity.normalizedPath, "Unable to bind music analysis path",
                errorMessage))
            return StoreLookupStatus::Unavailable;

        int result = sqlite3_step(track.Get());
        if (result == SQLITE_DONE)
        {
            if (!transaction.Commit(errorMessage))
                return StoreLookupStatus::Unavailable;
            return StoreLookupStatus::Miss;
        }
        if (result != SQLITE_ROW ||
            sqlite3_column_type(track.Get(), 0) != SQLITE_INTEGER ||
            sqlite3_column_type(track.Get(), 1) != SQLITE_INTEGER)
        {
            if (result == SQLITE_ROW)
                errorMessage = "Music analysis cache contains malformed track metadata.";
            else
                SetSqliteError(
                    errorMessage, "Unable to read music analysis track",
                    database, result);
            return StoreLookupStatus::Unavailable;
        }

        const sqlite3_int64 cachedSize = sqlite3_column_int64(track.Get(), 0);
        const sqlite3_int64 cachedWriteTime = sqlite3_column_int64(track.Get(), 1);
        TrackAnalysis loaded;
        if (cachedSize < 0 ||
            !ReadFloatColumn(track.Get(), 2, loaded.durationSeconds))
        {
            errorMessage = "Music analysis cache contains invalid track metadata.";
            return StoreLookupStatus::Unavailable;
        }
        result = sqlite3_step(track.Get());
        if (result != SQLITE_DONE)
        {
            if (result == SQLITE_ROW)
                errorMessage = "Music analysis cache contains duplicate track rows.";
            else
                SetSqliteError(
                    errorMessage, "Unable to finish reading music analysis track",
                    database, result);
            return StoreLookupStatus::Unavailable;
        }

        if (static_cast<std::uint64_t>(cachedSize) != identity.size ||
            static_cast<std::int64_t>(cachedWriteTime) != identity.writeTime)
        {
            if (!transaction.Commit(errorMessage))
                return StoreLookupStatus::Unavailable;
            errorMessage =
                "Music analysis cache entry is stale because the source file changed.";
            return StoreLookupStatus::Miss;
        }

        if (!LoadFrames(database, identity.normalizedPath, loaded, errorMessage) ||
            !LoadCandidates(
                database, identity.normalizedPath, 0, CandidateKind::Entry,
                loaded.entryCandidates, errorMessage) ||
            !LoadCandidates(
                database, identity.normalizedPath, 1, CandidateKind::Exit,
                loaded.exitCandidates, errorMessage))
            return StoreLookupStatus::Unavailable;

        std::string validationError;
        if (!ValidateAnalysis(loaded, validationError))
        {
            errorMessage = "Music analysis cache entry is corrupt: " + validationError + ".";
            return StoreLookupStatus::Unavailable;
        }
        if (!transaction.Commit(errorMessage))
            return StoreLookupStatus::Unavailable;

        analysis = std::move(loaded);
        errorMessage.clear();
        return StoreLookupStatus::Hit;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Unable to load music analysis cache: " +
            std::string(exception.what());
        return StoreLookupStatus::Unavailable;
    }
    catch (...)
    {
        errorMessage = "Unable to load music analysis cache: unknown error.";
        return StoreLookupStatus::Unavailable;
    }
}

bool AnalysisStore::Save(
    const FileIdentity& identity,
    const TrackAnalysis& analysis,
    std::string& errorMessage) noexcept
{
    errorMessage.clear();
    try
    {
        if (!ValidateIdentity(identity, errorMessage)) return false;
        std::string validationError;
        if (!ValidateAnalysis(analysis, validationError))
        {
            errorMessage = "Refusing to cache invalid music analysis: " +
                validationError + ".";
            return false;
        }

        std::lock_guard lock(m_impl->mutex);
        sqlite3* const database = m_impl->database;
        if (!database)
        {
            errorMessage =
                "Music analysis database is unavailable because Initialize has not "
                "completed successfully.";
            return false;
        }

        Transaction transaction(database, "BEGIN IMMEDIATE", errorMessage);
        if (!transaction.IsActive()) return false;

        Statement upsertTrack;
        if (!upsertTrack.Prepare(
                database,
                "INSERT OR REPLACE INTO tracks(normalized_path, file_size, write_time, "
                "duration_seconds) VALUES(?1, ?2, ?3, ?4)",
                "Unable to prepare music analysis track write", errorMessage) ||
            !upsertTrack.BindText(
                1, identity.normalizedPath, "Unable to bind music analysis path",
                errorMessage) ||
            !upsertTrack.BindInt64(
                2, static_cast<std::int64_t>(identity.size),
                "Unable to bind music file size", errorMessage) ||
            !upsertTrack.BindInt64(
                3, identity.writeTime, "Unable to bind music file write time",
                errorMessage) ||
            !upsertTrack.BindDouble(
                4, analysis.durationSeconds, "Unable to bind music duration",
                errorMessage) ||
            !upsertTrack.StepDone(
                "Unable to save music analysis track", errorMessage))
            return false;

        Statement deleteFrames;
        Statement deleteCandidates;
        if (!deleteFrames.Prepare(
                database, "DELETE FROM frames WHERE normalized_path = ?1",
                "Unable to prepare stale frame removal", errorMessage) ||
            !deleteFrames.BindText(
                1, identity.normalizedPath, "Unable to bind music analysis path",
                errorMessage) ||
            !deleteFrames.StepDone(
                "Unable to remove stale music analysis frames", errorMessage) ||
            !deleteCandidates.Prepare(
                database, "DELETE FROM candidates WHERE normalized_path = ?1",
                "Unable to prepare stale candidate removal", errorMessage) ||
            !deleteCandidates.BindText(
                1, identity.normalizedPath, "Unable to bind music analysis path",
                errorMessage) ||
            !deleteCandidates.StepDone(
                "Unable to remove stale music analysis candidates", errorMessage))
            return false;

        Statement insertFrame;
        if (!insertFrame.Prepare(
                database,
                "INSERT INTO frames(normalized_path, frame_index, time_seconds, "
                "energy, rms, loudness_lufs, frame_true_peak_db, "
                "spectral_centroid_hz, spectral_flux, onset_strength, "
                "silence_probability, energy_slope) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)",
                "Unable to prepare music analysis frame write", errorMessage))
            return false;
        for (std::size_t index = 0; index < analysis.frames.size(); ++index)
        {
            if (index != 0 && !insertFrame.Reset(
                                  "Unable to reset music analysis frame write",
                                  errorMessage))
                return false;
            if (!BindFrame(
                    insertFrame, identity.normalizedPath, index,
                    analysis.frames[index], errorMessage) ||
                !insertFrame.StepDone(
                    "Unable to save music analysis frame", errorMessage))
                return false;
        }

        Statement insertCandidate;
        if (!insertCandidate.Prepare(
                database,
                "INSERT INTO candidates(normalized_path, role, candidate_index, "
                "kind, frame_index, time_seconds, score, profile_energy, "
                "profile_loudness_lufs, profile_spectral_centroid_hz, "
                "profile_spectral_flux, profile_onset_strength) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)",
                "Unable to prepare music analysis candidate write", errorMessage))
            return false;

        bool firstCandidate = true;
        const auto saveCandidates = [&](const std::vector<Candidate>& candidates, int role) {
            for (std::size_t index = 0; index < candidates.size(); ++index)
            {
                if (!firstCandidate && !insertCandidate.Reset(
                                           "Unable to reset candidate write",
                                           errorMessage))
                    return false;
                firstCandidate = false;
                if (!BindCandidate(
                        insertCandidate, identity.normalizedPath, role, index,
                        candidates[index], errorMessage) ||
                    !insertCandidate.StepDone(
                        "Unable to save music analysis candidate", errorMessage))
                    return false;
            }
            return true;
        };
        if (!saveCandidates(analysis.entryCandidates, 0) ||
            !saveCandidates(analysis.exitCandidates, 1))
            return false;

        if (!transaction.Commit(errorMessage)) return false;
        errorMessage.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Unable to save music analysis cache: " +
            std::string(exception.what());
        return false;
    }
    catch (...)
    {
        errorMessage = "Unable to save music analysis cache: unknown error.";
        return false;
    }
}

bool AnalysisStore::Clear(std::string& errorMessage) noexcept
{
    errorMessage.clear();
    try
    {
        std::lock_guard lock(m_impl->mutex);
        sqlite3* const database = m_impl->database;
        if (!database)
        {
            errorMessage =
                "Music analysis database is unavailable because Initialize has not "
                "completed successfully.";
            return false;
        }
        Transaction transaction(database, "BEGIN IMMEDIATE", errorMessage);
        if (!transaction.IsActive()) return false;
        if (!Execute(
                database, "DELETE FROM tracks",
                "Unable to clear music analysis cache", errorMessage))
            return false;
        if (!transaction.Commit(errorMessage)) return false;
        errorMessage.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Unable to clear music analysis cache: " +
            std::string(exception.what());
        return false;
    }
    catch (...)
    {
        errorMessage = "Unable to clear music analysis cache: unknown error.";
        return false;
    }
}

} // namespace TSM::MusicAnalysis
