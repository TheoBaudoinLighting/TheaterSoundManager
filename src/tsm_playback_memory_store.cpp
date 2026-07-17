#include "tsm_playback_memory_store.h"

#include <winsqlite/winsqlite3.h>

#include <nlohmann/json.hpp>

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

namespace TSM::PlaybackMemory
{
namespace
{

constexpr int ApplicationId = 0x54534D50; // "TSMP"
constexpr int BusyTimeoutMilliseconds = 5000;
constexpr std::size_t MaximumPathBytes = 1024U * 1024U;
constexpr std::size_t MaximumHeatmapCells = 1'000'000U;
constexpr std::size_t MaximumHistoryBytes = 16U * 1024U * 1024U;

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
    ~Statement()
    {
        if (m_statement) sqlite3_finalize(m_statement);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement() = default;

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
            m_statement, index, value.data(), static_cast<int>(value.size()),
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

    sqlite3_stmt* Get() const noexcept { return m_statement; }

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
              database, beginSql,
              "Unable to begin playback memory transaction", errorMessage))
    {
    }

    ~Transaction()
    {
        if (!m_active) return;
        char* ignored = nullptr;
        sqlite3_exec(m_database, "ROLLBACK", nullptr, nullptr, &ignored);
        if (ignored) sqlite3_free(ignored);
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    bool IsActive() const noexcept { return m_active; }

    bool Commit(std::string& errorMessage)
    {
        if (!m_active) return false;
        if (!Execute(
                m_database, "COMMIT",
                "Unable to commit playback memory transaction", errorMessage))
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
            "Unable to inspect playback memory schema", errorMessage) ||
        !statement.BindText(
            1, type, "Unable to bind schema object type", errorMessage) ||
        !statement.BindText(
            2, name, "Unable to bind schema object name", errorMessage))
        return false;
    int result = sqlite3_step(statement.Get());
    if (result != SQLITE_ROW ||
        sqlite3_column_type(statement.Get(), 0) != SQLITE_TEXT)
    {
        errorMessage = "Playback memory schema object '" + name + "' is missing.";
        return false;
    }
    const auto* raw = reinterpret_cast<const char*>(
        sqlite3_column_text(statement.Get(), 0));
    const std::string definition = CompactSql(raw ? raw : "");
    result = sqlite3_step(statement.Get());
    if (result != SQLITE_DONE)
    {
        errorMessage = "Playback memory schema object '" + name +
            "' is duplicated or unreadable.";
        return false;
    }
    for (const std::string_view fragment : requiredFragments)
    {
        if (definition.find(fragment) == std::string::npos)
        {
            errorMessage = "Playback memory schema object '" + name +
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

bool VerifyIntegrity(sqlite3* database, std::string& errorMessage)
{
    Statement statement;
    if (!statement.Prepare(
            database, "PRAGMA quick_check(1)",
            "Unable to check playback memory database integrity", errorMessage))
        return false;
    int result = sqlite3_step(statement.Get());
    if (result != SQLITE_ROW ||
        sqlite3_column_type(statement.Get(), 0) != SQLITE_TEXT)
    {
        SetSqliteError(
            errorMessage, "Playback memory integrity check failed", database, result);
        return false;
    }
    const auto* text = reinterpret_cast<const char*>(
        sqlite3_column_text(statement.Get(), 0));
    if (!text || std::string_view(text) != "ok")
    {
        errorMessage = "Playback memory database is corrupt: " +
            std::string(text ? text : "integrity check returned no detail");
        return false;
    }
    result = sqlite3_step(statement.Get());
    if (result == SQLITE_DONE) return true;
    errorMessage =
        "Playback memory database is corrupt: integrity check returned multiple rows.";
    return false;
}

bool VerifyForeignKeys(sqlite3* database, std::string& errorMessage)
{
    Statement statement;
    if (!statement.Prepare(
            database, "PRAGMA foreign_key_check",
            "Unable to check playback memory references", errorMessage))
        return false;
    const int result = sqlite3_step(statement.Get());
    if (result == SQLITE_DONE) return true;
    if (result == SQLITE_ROW)
        errorMessage =
            "Playback memory database is corrupt: a row has a broken reference.";
    else
        SetSqliteError(
            errorMessage, "Playback memory reference check failed", database, result);
    return false;
}

bool ValidateSchema(sqlite3* database, std::string& errorMessage)
{
    constexpr const char* queries[] = {
        "SELECT normalized_path, file_size, write_time, state_schema_version, "
        "track_duration_seconds, bucket_duration_seconds, daily_decay_factor, "
        "reference_epoch_seconds, bucket_count FROM heatmaps LIMIT 0",
        "SELECT normalized_path, cell_index, fatigue FROM heatmap_cells LIMIT 0",
        "SELECT singleton_id, payload_schema_version, payload_utf8 "
        "FROM transition_history LIMIT 0"
    };
    for (const char* query : queries)
    {
        Statement statement;
        if (!statement.Prepare(
                database, query, "Playback memory database schema is invalid",
                errorMessage))
            return false;
    }
    return RequireSchemaDefinition(
               database, "table", "heatmaps",
               {"primarykey", "check(file_size>=0)",
                "check(reference_epoch_seconds>=0)",
                "check(bucket_count>=0andbucket_count<=1000000)",
                "withoutrowid"},
               errorMessage) &&
        RequireSchemaDefinition(
               database, "table", "heatmap_cells",
               {"check(cell_index>=0)",
                "check(fatigue>=0andfatigue<=1)",
                "primarykey(normalized_path,cell_index)",
                "foreignkey(normalized_path)referencesheatmaps(normalized_path)"
                "ondeletecascade",
                "withoutrowid"},
               errorMessage) &&
        RequireSchemaDefinition(
               database, "table", "transition_history",
               {"primarykey", "check(singleton_id=1)",
                "check(length(cast(payload_utf8asblob))<=16777216)"},
               errorMessage) &&
        ValidatePrimaryKey(
               database, "PRAGMA table_info(heatmaps)",
               {{"normalized_path", 1}}, "heatmaps", errorMessage) &&
        ValidatePrimaryKey(
               database, "PRAGMA table_info(heatmap_cells)",
               {{"normalized_path", 1}, {"cell_index", 2}},
               "heatmap_cells", errorMessage) &&
        ValidatePrimaryKey(
               database, "PRAGMA table_info(transition_history)",
               {{"singleton_id", 1}}, "transition_history", errorMessage);
}

bool EnsureSchema(sqlite3* database, std::string& errorMessage)
{
    Transaction transaction(database, "BEGIN IMMEDIATE", errorMessage);
    if (!transaction.IsActive()) return false;

    std::int64_t version = 0;
    std::int64_t applicationId = 0;
    if (!QuerySingleInteger(
            database, "PRAGMA user_version", version,
            "Unable to read playback memory schema version", errorMessage) ||
        !QuerySingleInteger(
            database, "PRAGMA application_id", applicationId,
            "Unable to identify playback memory database", errorMessage))
        return false;

    if (version == 0)
    {
        if (applicationId != 0)
        {
            errorMessage =
                "Refusing to use a database owned by another application for "
                "playback memory.";
            return false;
        }
        std::int64_t userTableCount = 0;
        if (!QuerySingleInteger(
                database,
                "SELECT count(*) FROM sqlite_master WHERE type = 'table' "
                "AND name NOT LIKE 'sqlite_%'",
                userTableCount,
                "Unable to inspect unversioned playback memory database",
                errorMessage))
            return false;
        if (userTableCount != 0)
        {
            errorMessage =
                "Refusing to initialize an unversioned, non-empty database as "
                "playback memory.";
            return false;
        }

        constexpr const char* schema = R"sql(
CREATE TABLE heatmaps (
    normalized_path TEXT PRIMARY KEY NOT NULL,
    file_size INTEGER NOT NULL CHECK(file_size >= 0),
    write_time INTEGER NOT NULL,
    state_schema_version INTEGER NOT NULL,
    track_duration_seconds REAL NOT NULL,
    bucket_duration_seconds REAL NOT NULL,
    daily_decay_factor REAL NOT NULL,
    reference_epoch_seconds INTEGER NOT NULL CHECK(reference_epoch_seconds >= 0),
    bucket_count INTEGER NOT NULL CHECK(bucket_count >= 0 AND bucket_count <= 1000000)
) WITHOUT ROWID;
CREATE TABLE heatmap_cells (
    normalized_path TEXT NOT NULL,
    cell_index INTEGER NOT NULL CHECK(cell_index >= 0),
    fatigue REAL NOT NULL CHECK(fatigue >= 0 AND fatigue <= 1),
    PRIMARY KEY(normalized_path, cell_index),
    FOREIGN KEY(normalized_path) REFERENCES heatmaps(normalized_path) ON DELETE CASCADE
) WITHOUT ROWID;
CREATE TABLE transition_history (
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id = 1),
    payload_schema_version INTEGER NOT NULL,
    payload_utf8 TEXT NOT NULL CHECK(length(CAST(payload_utf8 AS BLOB)) <= 16777216)
);
PRAGMA application_id = 1414745424;
PRAGMA user_version = 1;
)sql";
        if (!Execute(
                database, schema, "Unable to create playback memory schema",
                errorMessage))
            return false;
    }
    else if (version != Store::CurrentSchemaVersion ||
             applicationId != ApplicationId)
    {
        errorMessage = "Unsupported playback memory schema (version " +
            std::to_string(version) + ", application id " +
            std::to_string(applicationId) + ").";
        return false;
    }
    return transaction.Commit(errorMessage);
}

bool ValidateIdentity(
    const MusicAnalysis::FileIdentity& identity,
    std::string& errorMessage)
{
    if (identity.normalizedPath.empty() ||
        identity.normalizedPath.size() > MaximumPathBytes ||
        identity.normalizedPath.find('\0') != std::string::npos)
    {
        errorMessage = "Playback memory file identity has an invalid normalized path.";
        return false;
    }
    if (identity.size > static_cast<std::uint64_t>(
                            (std::numeric_limits<std::int64_t>::max)()))
    {
        errorMessage = "Playback memory source file is too large.";
        return false;
    }
    return true;
}

bool ReadDoubleColumn(sqlite3_stmt* statement, int column, double& value)
{
    const int type = sqlite3_column_type(statement, column);
    if (type != SQLITE_FLOAT && type != SQLITE_INTEGER) return false;
    value = sqlite3_column_double(statement, column);
    return std::isfinite(value);
}

bool ReadNonNegativeSize(
    sqlite3_stmt* statement,
    int column,
    std::size_t maximum,
    std::size_t& value)
{
    if (sqlite3_column_type(statement, column) != SQLITE_INTEGER) return false;
    const sqlite3_int64 parsed = sqlite3_column_int64(statement, column);
    if (parsed < 0 || static_cast<std::uint64_t>(parsed) > maximum) return false;
    value = static_cast<std::size_t>(parsed);
    return true;
}

bool CommitMiss(
    Transaction& transaction,
    std::string& errorMessage)
{
    return transaction.Commit(errorMessage);
}

bool SerializeTransitionHistory(
    const TransitionHistory::History& history,
    std::string& payload,
    std::string& errorMessage)
{
    const nlohmann::json document = history.ExportState();
    TransitionHistory::History validated;
    std::string importError;
    if (!validated.ImportState(document, importError))
    {
        errorMessage = "Refusing to persist invalid transition history: " +
            importError;
        return false;
    }
    payload = document.dump();
    if (payload.empty() || payload.size() > MaximumHistoryBytes)
    {
        errorMessage =
            "Transition history payload exceeds the durable size limit.";
        return false;
    }
    return true;
}

bool WriteTransitionHistory(
    sqlite3* database,
    const std::string& payload,
    std::string& errorMessage)
{
    Statement statement;
    return statement.Prepare(
               database,
               "INSERT OR REPLACE INTO transition_history(singleton_id, "
               "payload_schema_version, payload_utf8) VALUES(1, ?1, ?2)",
               "Unable to prepare transition history write", errorMessage) &&
        statement.BindInt64(
               1, TransitionHistory::History::CurrentSchemaVersion,
               "Unable to bind transition history schema", errorMessage) &&
        statement.BindText(
               2, payload, "Unable to bind transition history payload",
               errorMessage) &&
        statement.StepDone("Unable to save transition history", errorMessage);
}

} // namespace

struct Store::Impl
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

Store::Store(std::filesystem::path databasePath)
    : m_impl(std::make_unique<Impl>(std::move(databasePath)))
{
}

Store::~Store() = default;

bool Store::Initialize(std::string& errorMessage) noexcept
{
    errorMessage.clear();
    try
    {
        std::lock_guard lock(m_impl->mutex);
        if (m_impl->database) return true;
        if (m_impl->databasePath.empty())
        {
            errorMessage = "Playback memory database path is empty.";
            return false;
        }

        const std::filesystem::path parent = m_impl->databasePath.parent_path();
        if (!parent.empty())
        {
            std::error_code directoryError;
            std::filesystem::create_directories(parent, directoryError);
            if (directoryError)
            {
                errorMessage = "Unable to create playback memory directory: " +
                    directoryError.message();
                return false;
            }
        }

        const std::string path = PathToUtf8(
            std::filesystem::absolute(m_impl->databasePath).lexically_normal());
        sqlite3* rawDatabase = nullptr;
        const int openResult = sqlite3_open_v2(
            path.c_str(), &rawDatabase,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr);
        DatabaseHandle database(rawDatabase);
        if (openResult != SQLITE_OK)
        {
            SetSqliteError(
                errorMessage, "Unable to open playback memory database",
                rawDatabase, openResult);
            return false;
        }
        const int extendedResult = sqlite3_extended_result_codes(rawDatabase, 1);
        if (extendedResult != SQLITE_OK)
        {
            SetSqliteError(
                errorMessage, "Unable to enable detailed playback memory errors",
                rawDatabase, extendedResult);
            return false;
        }
        const int timeoutResult = sqlite3_busy_timeout(
            rawDatabase, BusyTimeoutMilliseconds);
        if (timeoutResult != SQLITE_OK)
        {
            SetSqliteError(
                errorMessage, "Unable to configure playback memory timeout",
                rawDatabase, timeoutResult);
            return false;
        }
        if (!Execute(
                rawDatabase, "PRAGMA foreign_keys = ON",
                "Unable to enable playback memory references", errorMessage) ||
            !EnsureSchema(rawDatabase, errorMessage) ||
            !ValidateSchema(rawDatabase, errorMessage) ||
            !VerifyIntegrity(rawDatabase, errorMessage) ||
            !VerifyForeignKeys(rawDatabase, errorMessage) ||
            !Execute(
                rawDatabase, "PRAGMA journal_mode = WAL",
                "Unable to enable playback memory write-ahead logging", errorMessage) ||
            !Execute(
                rawDatabase, "PRAGMA synchronous = FULL",
                "Unable to configure durable playback memory writes", errorMessage))
            return false;

        int defensiveEnabled = 0;
        const int defensiveResult = sqlite3_db_config(
            rawDatabase, SQLITE_DBCONFIG_DEFENSIVE, 1, &defensiveEnabled);
        if (defensiveResult != SQLITE_OK || defensiveEnabled != 1)
        {
            SetSqliteError(
                errorMessage, "Unable to harden playback memory connection",
                rawDatabase, defensiveResult);
            return false;
        }

        m_impl->database = database.release();
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Unable to initialize playback memory database: " +
            std::string(exception.what());
        return false;
    }
    catch (...)
    {
        errorMessage = "Unable to initialize playback memory database: unknown error.";
        return false;
    }
}

StoreLookupStatus Store::LoadHeatmap(
    const MusicAnalysis::FileIdentity& identity,
    ListeningHeatmap::State& state,
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
                "Playback memory is unavailable because Initialize has not completed.";
            return StoreLookupStatus::Unavailable;
        }

        Transaction transaction(database, "BEGIN", errorMessage);
        if (!transaction.IsActive()) return StoreLookupStatus::Unavailable;
        Statement parent;
        if (!parent.Prepare(
                database,
                "SELECT file_size, write_time, state_schema_version, "
                "track_duration_seconds, bucket_duration_seconds, "
                "daily_decay_factor, reference_epoch_seconds, bucket_count "
                "FROM heatmaps WHERE normalized_path = ?1",
                "Unable to load listening heatmap", errorMessage) ||
            !parent.BindText(
                1, identity.normalizedPath, "Unable to bind heatmap path",
                errorMessage))
            return StoreLookupStatus::Unavailable;

        int result = sqlite3_step(parent.Get());
        if (result == SQLITE_DONE)
        {
            if (!CommitMiss(transaction, errorMessage))
                return StoreLookupStatus::Unavailable;
            return StoreLookupStatus::Miss;
        }
        if (result != SQLITE_ROW ||
            sqlite3_column_type(parent.Get(), 0) != SQLITE_INTEGER ||
            sqlite3_column_type(parent.Get(), 1) != SQLITE_INTEGER ||
            sqlite3_column_type(parent.Get(), 2) != SQLITE_INTEGER ||
            sqlite3_column_type(parent.Get(), 6) != SQLITE_INTEGER)
        {
            if (result == SQLITE_ROW)
                errorMessage = "Listening heatmap metadata is malformed.";
            else
                SetSqliteError(
                    errorMessage, "Unable to read listening heatmap",
                    database, result);
            return StoreLookupStatus::Unavailable;
        }

        const sqlite3_int64 fileSize = sqlite3_column_int64(parent.Get(), 0);
        const sqlite3_int64 writeTime = sqlite3_column_int64(parent.Get(), 1);
        const sqlite3_int64 stateVersion = sqlite3_column_int64(parent.Get(), 2);
        const sqlite3_int64 referenceEpoch = sqlite3_column_int64(parent.Get(), 6);
        std::size_t bucketCount = 0;
        ListeningHeatmap::State loaded;
        if (fileSize < 0 || referenceEpoch < 0 ||
            stateVersion != static_cast<sqlite3_int64>(
                                ListeningHeatmap::StateSchemaVersion) ||
            !ReadDoubleColumn(parent.Get(), 3, loaded.trackDurationSeconds) ||
            !ReadDoubleColumn(parent.Get(), 4, loaded.bucketDurationSeconds) ||
            !ReadDoubleColumn(parent.Get(), 5, loaded.dailyDecayFactor) ||
            !ReadNonNegativeSize(
                parent.Get(), 7, MaximumHeatmapCells, bucketCount))
        {
            errorMessage = "Listening heatmap metadata contains invalid values.";
            return StoreLookupStatus::Unavailable;
        }
        loaded.schemaVersion = static_cast<std::uint32_t>(stateVersion);
        loaded.referenceEpochSeconds = static_cast<std::int64_t>(referenceEpoch);
        result = sqlite3_step(parent.Get());
        if (result != SQLITE_DONE)
        {
            if (result == SQLITE_ROW)
                errorMessage = "Listening heatmap contains duplicate parent rows.";
            else
                SetSqliteError(
                    errorMessage, "Unable to finish reading listening heatmap",
                    database, result);
            return StoreLookupStatus::Unavailable;
        }

        if (static_cast<std::uint64_t>(fileSize) != identity.size ||
            static_cast<std::int64_t>(writeTime) != identity.writeTime)
        {
            if (!transaction.Commit(errorMessage))
                return StoreLookupStatus::Unavailable;
            errorMessage =
                "Listening heatmap is stale because the source file changed.";
            return StoreLookupStatus::Miss;
        }

        Statement cells;
        if (!cells.Prepare(
                database,
                "SELECT cell_index, fatigue FROM heatmap_cells "
                "WHERE normalized_path = ?1 ORDER BY cell_index",
                "Unable to load heatmap cells", errorMessage) ||
            !cells.BindText(
                1, identity.normalizedPath, "Unable to bind heatmap path",
                errorMessage))
            return StoreLookupStatus::Unavailable;
        loaded.bucketFatigue.reserve(bucketCount);
        while (true)
        {
            result = sqlite3_step(cells.Get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW)
            {
                SetSqliteError(
                    errorMessage, "Unable to read heatmap cells", database, result);
                return StoreLookupStatus::Unavailable;
            }
            std::size_t index = 0;
            double fatigue = 0.0;
            if (!ReadNonNegativeSize(
                    cells.Get(), 0, MaximumHeatmapCells, index) ||
                index != loaded.bucketFatigue.size() ||
                !ReadDoubleColumn(cells.Get(), 1, fatigue) ||
                fatigue < 0.0 || fatigue > 1.0)
            {
                errorMessage = "Listening heatmap contains a malformed cell.";
                return StoreLookupStatus::Unavailable;
            }
            loaded.bucketFatigue.push_back(fatigue);
        }
        if (loaded.bucketFatigue.size() != bucketCount)
        {
            errorMessage = "Listening heatmap cell count does not match its metadata.";
            return StoreLookupStatus::Unavailable;
        }

        ListeningHeatmap::State imported;
        if (!ListeningHeatmap::ImportState(loaded, imported))
        {
            errorMessage = "Listening heatmap failed strict state validation.";
            return StoreLookupStatus::Unavailable;
        }
        if (!transaction.Commit(errorMessage))
            return StoreLookupStatus::Unavailable;
        state = std::move(imported);
        errorMessage.clear();
        return StoreLookupStatus::Hit;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Unable to load listening heatmap: " +
            std::string(exception.what());
        return StoreLookupStatus::Unavailable;
    }
    catch (...)
    {
        errorMessage = "Unable to load listening heatmap: unknown error.";
        return StoreLookupStatus::Unavailable;
    }
}

bool Store::SaveHeatmap(
    const MusicAnalysis::FileIdentity& identity,
    const ListeningHeatmap::State& state,
    std::string& errorMessage) noexcept
{
    errorMessage.clear();
    try
    {
        if (!ValidateIdentity(identity, errorMessage)) return false;
        ListeningHeatmap::State validated;
        if (!ListeningHeatmap::ImportState(state, validated) ||
            validated.bucketFatigue.size() > MaximumHeatmapCells)
        {
            errorMessage = "Refusing to persist an invalid listening heatmap state.";
            return false;
        }
        validated = ListeningHeatmap::ExportState(validated);

        std::lock_guard lock(m_impl->mutex);
        sqlite3* const database = m_impl->database;
        if (!database)
        {
            errorMessage =
                "Playback memory is unavailable because Initialize has not completed.";
            return false;
        }
        Transaction transaction(database, "BEGIN IMMEDIATE", errorMessage);
        if (!transaction.IsActive()) return false;

        Statement parent;
        constexpr std::string_view bindContext = "Unable to bind listening heatmap";
        if (!parent.Prepare(
                database,
                "INSERT OR REPLACE INTO heatmaps(normalized_path, file_size, write_time, "
                "state_schema_version, track_duration_seconds, "
                "bucket_duration_seconds, daily_decay_factor, "
                "reference_epoch_seconds, bucket_count) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
                "Unable to prepare listening heatmap write", errorMessage) ||
            !parent.BindText(1, identity.normalizedPath, bindContext, errorMessage) ||
            !parent.BindInt64(
                2, static_cast<std::int64_t>(identity.size),
                bindContext, errorMessage) ||
            !parent.BindInt64(3, identity.writeTime, bindContext, errorMessage) ||
            !parent.BindInt64(
                4, validated.schemaVersion, bindContext, errorMessage) ||
            !parent.BindDouble(
                5, validated.trackDurationSeconds, bindContext, errorMessage) ||
            !parent.BindDouble(
                6, validated.bucketDurationSeconds, bindContext, errorMessage) ||
            !parent.BindDouble(
                7, validated.dailyDecayFactor, bindContext, errorMessage) ||
            !parent.BindInt64(
                8, validated.referenceEpochSeconds, bindContext, errorMessage) ||
            !parent.BindInt64(
                9, static_cast<std::int64_t>(validated.bucketFatigue.size()),
                bindContext, errorMessage) ||
            !parent.StepDone("Unable to save listening heatmap", errorMessage))
            return false;

        Statement deleteCells;
        if (!deleteCells.Prepare(
                database, "DELETE FROM heatmap_cells WHERE normalized_path = ?1",
                "Unable to prepare old heatmap cell removal", errorMessage) ||
            !deleteCells.BindText(
                1, identity.normalizedPath, bindContext, errorMessage) ||
            !deleteCells.StepDone(
                "Unable to remove old heatmap cells", errorMessage))
            return false;

        Statement insertCell;
        if (!insertCell.Prepare(
                database,
                "INSERT INTO heatmap_cells(normalized_path, cell_index, fatigue) "
                "VALUES(?1, ?2, ?3)",
                "Unable to prepare heatmap cell write", errorMessage))
            return false;
        for (std::size_t index = 0; index < validated.bucketFatigue.size(); ++index)
        {
            if (index != 0 && !insertCell.Reset(
                                  "Unable to reset heatmap cell write", errorMessage))
                return false;
            if (!insertCell.BindText(
                    1, identity.normalizedPath, bindContext, errorMessage) ||
                !insertCell.BindInt64(
                    2, static_cast<std::int64_t>(index),
                    bindContext, errorMessage) ||
                !insertCell.BindDouble(
                    3, validated.bucketFatigue[index],
                    bindContext, errorMessage) ||
                !insertCell.StepDone("Unable to save heatmap cell", errorMessage))
                return false;
        }
        if (!transaction.Commit(errorMessage)) return false;
        errorMessage.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Unable to save listening heatmap: " +
            std::string(exception.what());
        return false;
    }
    catch (...)
    {
        errorMessage = "Unable to save listening heatmap: unknown error.";
        return false;
    }
}

StoreLookupStatus Store::LoadTransitionHistory(
    TransitionHistory::History& history,
    std::string& errorMessage) noexcept
{
    errorMessage.clear();
    try
    {
        std::lock_guard lock(m_impl->mutex);
        sqlite3* const database = m_impl->database;
        if (!database)
        {
            errorMessage =
                "Playback memory is unavailable because Initialize has not completed.";
            return StoreLookupStatus::Unavailable;
        }
        Transaction transaction(database, "BEGIN", errorMessage);
        if (!transaction.IsActive()) return StoreLookupStatus::Unavailable;

        Statement statement;
        if (!statement.Prepare(
                database,
                "SELECT payload_schema_version, payload_utf8 "
                "FROM transition_history WHERE singleton_id = 1",
                "Unable to load transition history", errorMessage))
            return StoreLookupStatus::Unavailable;
        int result = sqlite3_step(statement.Get());
        if (result == SQLITE_DONE)
        {
            if (!transaction.Commit(errorMessage))
                return StoreLookupStatus::Unavailable;
            return StoreLookupStatus::Miss;
        }
        if (result != SQLITE_ROW ||
            sqlite3_column_type(statement.Get(), 0) != SQLITE_INTEGER ||
            sqlite3_column_type(statement.Get(), 1) != SQLITE_TEXT)
        {
            if (result == SQLITE_ROW)
                errorMessage = "Transition history cache row is malformed.";
            else
                SetSqliteError(
                    errorMessage, "Unable to read transition history",
                    database, result);
            return StoreLookupStatus::Unavailable;
        }
        if (sqlite3_column_int64(statement.Get(), 0) !=
            TransitionHistory::History::CurrentSchemaVersion)
        {
            errorMessage = "Transition history payload schema is unsupported.";
            return StoreLookupStatus::Unavailable;
        }
        const int byteCount = sqlite3_column_bytes(statement.Get(), 1);
        const auto* bytes = reinterpret_cast<const char*>(
            sqlite3_column_text(statement.Get(), 1));
        if (!bytes || byteCount <= 0 ||
            static_cast<std::size_t>(byteCount) > MaximumHistoryBytes)
        {
            errorMessage = "Transition history payload size is invalid.";
            return StoreLookupStatus::Unavailable;
        }
        const std::string payload(bytes, static_cast<std::size_t>(byteCount));
        result = sqlite3_step(statement.Get());
        if (result != SQLITE_DONE)
        {
            if (result == SQLITE_ROW)
                errorMessage = "Transition history contains duplicate singleton rows.";
            else
                SetSqliteError(
                    errorMessage, "Unable to finish reading transition history",
                    database, result);
            return StoreLookupStatus::Unavailable;
        }

        const nlohmann::json document = nlohmann::json::parse(
            payload, nullptr, false, false);
        if (document.is_discarded())
        {
            errorMessage = "Transition history payload is not valid UTF-8 JSON.";
            return StoreLookupStatus::Unavailable;
        }
        TransitionHistory::History loaded;
        std::string importError;
        if (!loaded.ImportState(document, importError))
        {
            errorMessage = "Transition history payload is invalid: " + importError;
            return StoreLookupStatus::Unavailable;
        }
        if (!transaction.Commit(errorMessage))
            return StoreLookupStatus::Unavailable;
        history = std::move(loaded);
        errorMessage.clear();
        return StoreLookupStatus::Hit;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Unable to load transition history: " +
            std::string(exception.what());
        return StoreLookupStatus::Unavailable;
    }
    catch (...)
    {
        errorMessage = "Unable to load transition history: unknown error.";
        return StoreLookupStatus::Unavailable;
    }
}

bool Store::SaveTransitionHistory(
    const TransitionHistory::History& history,
    std::string& errorMessage) noexcept
{
    errorMessage.clear();
    try
    {
        std::string payload;
        if (!SerializeTransitionHistory(history, payload, errorMessage))
            return false;

        std::lock_guard lock(m_impl->mutex);
        sqlite3* const database = m_impl->database;
        if (!database)
        {
            errorMessage =
                "Playback memory is unavailable because Initialize has not completed.";
            return false;
        }
        Transaction transaction(database, "BEGIN IMMEDIATE", errorMessage);
        if (!transaction.IsActive()) return false;
        if (!WriteTransitionHistory(database, payload, errorMessage))
            return false;
        if (!transaction.Commit(errorMessage)) return false;
        errorMessage.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Unable to save transition history: " +
            std::string(exception.what());
        return false;
    }
    catch (...)
    {
        errorMessage = "Unable to save transition history: unknown error.";
        return false;
    }
}

bool Store::ClearHeatmap(
    const std::string& normalizedTrackPath,
    std::string& errorMessage) noexcept
{
    errorMessage.clear();
    try
    {
        MusicAnalysis::FileIdentity identity;
        identity.normalizedPath = normalizedTrackPath;
        if (!ValidateIdentity(identity, errorMessage)) return false;
        std::lock_guard lock(m_impl->mutex);
        sqlite3* const database = m_impl->database;
        if (!database)
        {
            errorMessage =
                "Playback memory is unavailable because Initialize has not completed.";
            return false;
        }
        Transaction transaction(database, "BEGIN IMMEDIATE", errorMessage);
        if (!transaction.IsActive()) return false;
        Statement statement;
        if (!statement.Prepare(
                database, "DELETE FROM heatmaps WHERE normalized_path = ?1",
                "Unable to prepare heatmap clear", errorMessage) ||
            !statement.BindText(
                1, normalizedTrackPath, "Unable to bind heatmap path", errorMessage) ||
            !statement.StepDone("Unable to clear listening heatmap", errorMessage))
            return false;
        if (!transaction.Commit(errorMessage)) return false;
        errorMessage.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Unable to clear listening heatmap: " +
            std::string(exception.what());
        return false;
    }
    catch (...)
    {
        errorMessage = "Unable to clear listening heatmap: unknown error.";
        return false;
    }
}

bool Store::ClearTrackMemory(
    const std::string& normalizedTrackPath,
    const TransitionHistory::History& remainingHistory,
    std::string& errorMessage) noexcept
{
    errorMessage.clear();
    try
    {
        MusicAnalysis::FileIdentity identity;
        identity.normalizedPath = normalizedTrackPath;
        if (!ValidateIdentity(identity, errorMessage)) return false;

        std::string payload;
        if (!SerializeTransitionHistory(
                remainingHistory, payload, errorMessage))
            return false;

        std::lock_guard lock(m_impl->mutex);
        sqlite3* const database = m_impl->database;
        if (!database)
        {
            errorMessage =
                "Playback memory is unavailable because Initialize has not completed.";
            return false;
        }
        Transaction transaction(database, "BEGIN IMMEDIATE", errorMessage);
        if (!transaction.IsActive()) return false;
        Statement statement;
        if (!statement.Prepare(
                database, "DELETE FROM heatmaps WHERE normalized_path = ?1",
                "Unable to prepare atomic track-memory clear", errorMessage) ||
            !statement.BindText(
                1, normalizedTrackPath, "Unable to bind heatmap path",
                errorMessage) ||
            !statement.StepDone(
                "Unable to clear listening heatmap", errorMessage) ||
            !WriteTransitionHistory(database, payload, errorMessage))
            return false;
        if (!transaction.Commit(errorMessage)) return false;
        errorMessage.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Unable to clear track playback memory atomically: " +
            std::string(exception.what());
        return false;
    }
    catch (...)
    {
        errorMessage =
            "Unable to clear track playback memory atomically: unknown error.";
        return false;
    }
}

bool Store::ClearTransitionHistory(std::string& errorMessage) noexcept
{
    errorMessage.clear();
    try
    {
        std::lock_guard lock(m_impl->mutex);
        sqlite3* const database = m_impl->database;
        if (!database)
        {
            errorMessage =
                "Playback memory is unavailable because Initialize has not completed.";
            return false;
        }
        Transaction transaction(database, "BEGIN IMMEDIATE", errorMessage);
        if (!transaction.IsActive()) return false;
        if (!Execute(
                database, "DELETE FROM transition_history",
                "Unable to clear transition history", errorMessage))
            return false;
        if (!transaction.Commit(errorMessage)) return false;
        errorMessage.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Unable to clear transition history: " +
            std::string(exception.what());
        return false;
    }
    catch (...)
    {
        errorMessage = "Unable to clear transition history: unknown error.";
        return false;
    }
}

bool Store::ClearAll(std::string& errorMessage) noexcept
{
    errorMessage.clear();
    try
    {
        std::lock_guard lock(m_impl->mutex);
        sqlite3* const database = m_impl->database;
        if (!database)
        {
            errorMessage =
                "Playback memory is unavailable because Initialize has not completed.";
            return false;
        }
        Transaction transaction(database, "BEGIN IMMEDIATE", errorMessage);
        if (!transaction.IsActive()) return false;
        if (!Execute(
                database,
                "DELETE FROM heatmaps; DELETE FROM transition_history;",
                "Unable to clear playback memory", errorMessage))
            return false;
        if (!transaction.Commit(errorMessage)) return false;
        errorMessage.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Unable to clear playback memory: " +
            std::string(exception.what());
        return false;
    }
    catch (...)
    {
        errorMessage = "Unable to clear playback memory: unknown error.";
        return false;
    }
}

} // namespace TSM::PlaybackMemory
