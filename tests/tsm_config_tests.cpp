#include "pch.h"
#include "tsm_config.h"

#include <filesystem>
#include <nlohmann/json.hpp>

namespace
{

class TemporaryConfigFile
{
public:
    TemporaryConfigFile(const char* name, const nlohmann::json& document)
        : m_path(std::filesystem::temp_directory_path() / name)
    {
        std::ofstream output(m_path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) throw std::runtime_error("Unable to create temporary config.");
        output << document.dump(2);
        if (!output.good()) throw std::runtime_error("Unable to write temporary config.");
    }

    ~TemporaryConfigFile()
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    const std::filesystem::path& Path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

bool HasIssuePath(
    const std::vector<TSM::ConfigValidationIssue>& issues,
    const std::string& pathFragment)
{
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue)
    {
        return issue.path.find(pathFragment) != std::string::npos;
    });
}

bool HasIssueMessage(
    const std::vector<TSM::ConfigValidationIssue>& issues,
    const std::string& messageFragment)
{
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue)
    {
        return issue.message.find(messageFragment) != std::string::npos;
    });
}

std::filesystem::path FindRepositoryFile(const std::filesystem::path& relative)
{
    std::filesystem::path directory = std::filesystem::current_path();
    for (int depth = 0; depth < 8; ++depth)
    {
        const std::filesystem::path candidate = directory / relative;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error)
            return candidate;
        if (!directory.has_parent_path() || directory.parent_path() == directory) break;
        directory = directory.parent_path();
    }
    return {};
}

nlohmann::json MinimalCinemaDocument()
{
    return {
        {"playlists", nlohmann::json::array({{
            {"name", "seasonal"},
            {"tracks", nlohmann::json::array({{
                {"id", "track"}, {"path", "track.mp3"}
            }})}
        }})},
        {"announcements", nlohmann::json::array({{
            {"id", "evacuation"}, {"path", "evacuation.mp3"}
        }})},
        {"cinema", {
            {"enabled", true},
            {"schedules", nlohmann::json::array()},
            {"safety", {
                {"enabled", true},
                {"evacuationAnnouncementId", "evacuation"}
            }}
        }}
    };
}

} // namespace

namespace TSM::Tests
{

TEST(ConfigTests, LoadsPlaylistLoudnessAndSchedule)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "tsm_config_test.json";
    {
        std::ofstream output(path);
        output << R"({
            "loudnessTargetLufs": -18.0,
            "playlists": [{
                "name": "test",
                "options": {"segmentDuration": 42.0},
                "tracks": [{"id": "track", "path": "track.mp3"}]
            }],
            "announcements": [{
                "id": "announcement", "path": "announcement.mp3",
                "hour": 9, "minute": 30
            }]
        })";
    }

    AppConfig config;
    std::string error;
    ASSERT_TRUE(LoadAppConfig(path.string(), config, error)) << error;
    EXPECT_FLOAT_EQ(config.loudnessTargetLufs, -18.0f);
    ASSERT_EQ(config.playlists.size(), 1u);
    EXPECT_EQ(config.playlists[0].name, "test");
    EXPECT_FLOAT_EQ(config.playlists[0].options.segmentDuration, 42.0f);
    ASSERT_EQ(config.playlists[0].tracks.size(), 1u);
    ASSERT_EQ(config.announcements.size(), 1u);
    EXPECT_EQ(config.announcements[0].hour, 9);
    EXPECT_EQ(config.announcements[0].minute, 30);

    std::error_code removeError;
    std::filesystem::remove(path, removeError);
}

TEST(ConfigTests, LoadsCinemaCalendarResumeAndSafetyConfiguration)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "tsm_cinema_config_test.json";
    {
        std::ofstream output(path);
        output << R"({
            "playlists": [{
                "name": "seasonal",
                "tracks": [{"id": "track", "path": "track.mp3"}]
            }],
            "announcements": [{
                "id": "evacuation", "path": "evacuation.mp3"
            }],
            "cinema": {
                "enabled": true,
                "schedules": [
                    {
                        "id": "halloween",
                        "playlist": "seasonal",
                        "priority": 50,
                        "period": {"type": "annual-date", "date": "10-31"},
                        "window": {"start": "18:00", "end": "02:00"},
                        "weekdays": [1, 2, 3, 4, 5, 6, 7]
                    },
                    {
                        "id": "easter-week",
                        "playlist": "seasonal",
                        "period": {
                            "type": "easter-range",
                            "startOffsetDays": -6,
                            "endOffsetDays": 1
                        }
                    }
                ],
                "resume": {
                    "enabled": true,
                    "automatic": true,
                    "maxAgeMinutes": 180,
                    "checkpointIntervalSeconds": 2
                },
                "safety": {
                    "enabled": true,
                    "evacuationAnnouncementId": "evacuation",
                    "interlock": {
                        "enabled": true,
                        "expectedHeartbeatSource": "fire-panel",
                        "heartbeatTimeoutMilliseconds": 2500,
                        "startupGraceMilliseconds": 8000,
                        "safeStableMilliseconds": 500
                    }
                }
            }
        })";
    }

    AppConfig config;
    std::vector<ConfigValidationIssue> issues;
    std::string error;
    ASSERT_TRUE(LoadValidatedAppConfig(path.string(), config, issues, error)) << error;
    ASSERT_TRUE(issues.empty());
    ASSERT_TRUE(config.cinema.enabled);
    ASSERT_EQ(config.cinema.schedules.size(), 2u);
    EXPECT_EQ(config.cinema.schedules[0].id, "halloween");
    EXPECT_EQ(config.cinema.schedules[0].period.kind, CinemaPeriodKind::AnnualDate);
    EXPECT_EQ(config.cinema.schedules[0].windowStartMinute, 18 * 60);
    EXPECT_EQ(config.cinema.schedules[0].windowEndMinute, 2 * 60);
    EXPECT_EQ(config.cinema.schedules[1].period.kind, CinemaPeriodKind::EasterRange);
    EXPECT_EQ(config.cinema.schedules[1].period.startOffsetDays, -6);
    EXPECT_TRUE(config.cinema.resume.automatic);
    EXPECT_EQ(config.cinema.resume.maxAgeMinutes, 180);
    EXPECT_TRUE(config.cinema.safety.enabled);
    EXPECT_EQ(config.cinema.safety.evacuationAnnouncementId, "evacuation");
    EXPECT_EQ(
        config.cinema.safety.interlock.heartbeatTimeoutMilliseconds, 2500);
    EXPECT_EQ(
        config.cinema.safety.interlock.expectedHeartbeatSource, "fire-panel");

    std::error_code removeError;
    std::filesystem::remove(path, removeError);
}

TEST(ConfigTests, RejectsInvalidCinemaCalendarAndSafetyReferences)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "tsm_invalid_cinema_config_test.json";
    {
        std::ofstream output(path);
        output << R"({
            "playlists": [{"name": "known", "tracks": []}],
            "announcements": [],
            "cinema": {
                "enabled": true,
                "schedules": [
                    {
                        "id": "bad",
                        "playlist": "missing",
                        "period": {"type": "date-range", "start": "2026-12-31", "end": "2026-01-01"},
                        "window": {"start": "25:00", "end": "02:00"}
                    },
                    {
                        "id": "bad",
                        "playlist": "known",
                        "period": {"type": "easter-range", "startOffsetDays": 10, "endOffsetDays": -10}
                    }
                ],
                "resume": {"maxAgeMinutes": 0},
                "safety": {
                    "enabled": true,
                    "evacuationAnnouncementId": "missing",
                    "interlock": {"enabled": true, "heartbeatTimeoutMilliseconds": 10}
                }
            }
        })";
    }

    std::vector<ConfigValidationIssue> issues;
    std::string error;
    EXPECT_FALSE(ValidateAppConfig(path.string(), issues, error));
    EXPECT_TRUE(error.empty());
    const auto hasIssue = [&](const std::string& pathFragment)
    {
        return std::any_of(issues.begin(), issues.end(), [&](const auto& issue)
        {
            return issue.path.find(pathFragment) != std::string::npos;
        });
    };
    EXPECT_TRUE(hasIssue(".playlist"));
    EXPECT_TRUE(hasIssue(".window.start"));
    EXPECT_TRUE(hasIssue(".resume.maxAgeMinutes"));
    EXPECT_TRUE(hasIssue(".evacuationAnnouncementId"));
    EXPECT_TRUE(hasIssue(".heartbeatTimeoutMilliseconds"));
    EXPECT_TRUE(hasIssue(".expectedHeartbeatSource"));

    std::error_code removeError;
    std::filesystem::remove(path, removeError);
}

TEST(ConfigTests, ShippedCinemaExampleValidatesAndParses)
{
    const std::filesystem::path path =
        FindRepositoryFile("config/tsm_config.cinema.example.json");
    ASSERT_FALSE(path.empty());

    AppConfig config;
    std::vector<ConfigValidationIssue> issues;
    std::string error;
    ASSERT_TRUE(LoadValidatedAppConfig(path.string(), config, issues, error)) << error;
    EXPECT_TRUE(issues.empty());
    EXPECT_TRUE(config.cinema.enabled);
    ASSERT_EQ(config.cinema.schedules.size(), 3u);
    EXPECT_EQ(config.cinema.schedules[0].id, "halloween");
    EXPECT_EQ(config.cinema.schedules[0].period.kind, CinemaPeriodKind::AnnualDate);
    EXPECT_EQ(config.cinema.schedules[1].period.kind, CinemaPeriodKind::AnnualRange);
    EXPECT_EQ(config.cinema.schedules[2].period.kind, CinemaPeriodKind::EasterRange);
    EXPECT_TRUE(config.cinema.safety.enabled);
    EXPECT_TRUE(config.cinema.safety.interlock.enabled);
    EXPECT_EQ(config.cinema.safety.evacuationAnnouncementId, "evacuation_fr");
}

TEST(ConfigTests, CinemaValidationHandlesWrongJsonTypesWithoutThrowing)
{
    nlohmann::json document = MinimalCinemaDocument();
    document["cinema"] = {
        {"enabled", "yes"},
        {"schedules", nlohmann::json::array({
            {
                {"id", 42},
                {"playlist", nlohmann::json::array()},
                {"enabled", 1},
                {"priority", 1.5},
                {"period", nlohmann::json::array()},
                {"window", nlohmann::json::array()},
                {"weekdays", "monday"}
            },
            {
                {"id", "bad-period-type"},
                {"playlist", "seasonal"},
                {"period", {{"type", false}}}
            },
            {
                {"id", "bad-period-fields"},
                {"playlist", "seasonal"},
                {"period", {
                    {"type", "easter-range"},
                    {"startOffsetDays", "-1"},
                    {"endOffsetDays", nlohmann::json::array()}
                }},
                {"window", {{"start", 900}, {"end", nullptr}}},
                {"weekdays", nlohmann::json::array({1, "2", 2.0, 8})}
            }
        })},
        {"resume", {
            {"enabled", 1},
            {"automatic", "yes"},
            {"maxAgeMinutes", 1.5},
            {"checkpointIntervalSeconds", nullptr}
        }},
        {"safety", {
            {"enabled", "yes"},
            {"evacuationAnnouncementId", 42},
            {"interlock", {
                {"enabled", nlohmann::json::array()},
                {"heartbeatTimeoutMilliseconds", "fast"},
                {"startupGraceMilliseconds", 1.5},
                {"safeStableMilliseconds", nullptr}
            }}
        }}
    };
    const TemporaryConfigFile file("tsm_cinema_wrong_types.json", document);

    std::vector<ConfigValidationIssue> issues;
    std::string error;
    bool valid = true;
    EXPECT_NO_THROW(valid = ValidateAppConfig(file.Path().string(), issues, error));
    EXPECT_FALSE(valid);
    EXPECT_TRUE(error.empty());
    EXPECT_GE(issues.size(), 15u);
    EXPECT_TRUE(HasIssuePath(issues, "$.cinema.enabled"));
    EXPECT_TRUE(HasIssuePath(issues, ".period"));
    EXPECT_TRUE(HasIssuePath(issues, ".window"));
    EXPECT_TRUE(HasIssuePath(issues, ".weekdays"));
    EXPECT_TRUE(HasIssuePath(issues, "$.cinema.resume"));
    EXPECT_TRUE(HasIssuePath(issues, "$.cinema.safety"));

    AppConfig validatedConfig;
    validatedConfig.cinema.enabled = true;
    bool loaded = true;
    EXPECT_NO_THROW(loaded = LoadValidatedAppConfig(
        file.Path().string(), validatedConfig, issues, error));
    EXPECT_FALSE(loaded);
    EXPECT_TRUE(validatedConfig.cinema.enabled);

    AppConfig unvalidatedConfig;
    unvalidatedConfig.loudnessTargetLufs = -22.0f;
    unvalidatedConfig.cinema.enabled = true;
    EXPECT_NO_THROW(loaded = LoadAppConfig(
        file.Path().string(), unvalidatedConfig, error));
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(error.empty());
    EXPECT_FLOAT_EQ(unvalidatedConfig.loudnessTargetLufs, -22.0f);
    EXPECT_TRUE(unvalidatedConfig.cinema.enabled);
}

TEST(ConfigTests, ParsesEveryCinemaPeriodAndAcceptedBoundary)
{
    nlohmann::json document = MinimalCinemaDocument();
    document["cinema"]["schedules"] = nlohmann::json::array({
        {
            {"id", "always"}, {"playlist", "seasonal"}, {"priority", -1000},
            {"weekdays", nlohmann::json::array({1, 7})},
            {"window", {{"start", "00:00"}, {"end", "00:00"}}},
            {"period", {{"type", "always"}}}
        },
        {
            {"id", "date"}, {"playlist", "seasonal"}, {"priority", 1000},
            {"period", {{"type", "date"}, {"date", "9999-12-31"}}}
        },
        {
            {"id", "date-range"}, {"playlist", "seasonal"},
            {"period", {
                {"type", "date-range"},
                {"start", "2026-01-01"}, {"end", "2026-01-01"}
            }}
        },
        {
            {"id", "annual-date"}, {"playlist", "seasonal"},
            {"period", {{"type", "annual-date"}, {"date", "02-29"}}}
        },
        {
            {"id", "annual-range"}, {"playlist", "seasonal"},
            {"period", {
                {"type", "annual-range"}, {"start", "12-31"}, {"end", "01-01"}
            }}
        },
        {
            {"id", "easter-range"}, {"playlist", "seasonal"},
            {"period", {
                {"type", "easter-range"},
                {"startOffsetDays", -366}, {"endOffsetDays", 366}
            }}
        }
    });
    document["cinema"]["resume"] = {
        {"enabled", true}, {"automatic", true},
        {"maxAgeMinutes", 10080}, {"checkpointIntervalSeconds", 60}
    };
    document["cinema"]["safety"]["interlock"] = {
        {"enabled", true},
        {"expectedHeartbeatSource", "fire-panel"},
        {"heartbeatTimeoutMilliseconds", 100},
        {"startupGraceMilliseconds", 300000},
        {"safeStableMilliseconds", 60000}
    };
    const TemporaryConfigFile file("tsm_cinema_period_boundaries.json", document);

    AppConfig config;
    std::vector<ConfigValidationIssue> issues;
    std::string error;
    ASSERT_TRUE(LoadValidatedAppConfig(file.Path().string(), config, issues, error))
        << error;
    EXPECT_TRUE(issues.empty());
    ASSERT_EQ(config.cinema.schedules.size(), 6u);
    EXPECT_EQ(config.cinema.schedules[0].period.kind, CinemaPeriodKind::Always);
    EXPECT_EQ(config.cinema.schedules[1].period.start.year, 9999);
    EXPECT_EQ(config.cinema.schedules[2].period.start.day, 1);
    EXPECT_EQ(config.cinema.schedules[3].period.start.month, 2);
    EXPECT_EQ(config.cinema.schedules[4].period.end.month, 1);
    EXPECT_EQ(config.cinema.schedules[5].period.startOffsetDays, -366);
    EXPECT_EQ(config.cinema.schedules[5].period.endOffsetDays, 366);
    EXPECT_EQ(config.cinema.resume.maxAgeMinutes, 10080);
    EXPECT_EQ(config.cinema.resume.checkpointIntervalSeconds, 60);
    EXPECT_EQ(
        config.cinema.safety.interlock.heartbeatTimeoutMilliseconds, 100);
    EXPECT_EQ(config.cinema.safety.interlock.startupGraceMilliseconds, 300000);
    EXPECT_EQ(config.cinema.safety.interlock.safeStableMilliseconds, 60000);
}

TEST(ConfigTests, RejectsMalformedDatesRangesOffsetsAndNumericBounds)
{
    nlohmann::json document = MinimalCinemaDocument();
    document["cinema"]["schedules"] = nlohmann::json::array({
        {
            {"id", "bad-date"}, {"playlist", "seasonal"},
            {"priority", -1001},
            {"period", {{"type", "date"}, {"date", "2026-02-29"}}}
        },
        {
            {"id", "bad-date-range"}, {"playlist", "seasonal"},
            {"period", {
                {"type", "date-range"},
                {"start", "2026-12-31"}, {"end", "2026-01-01"}
            }}
        },
        {
            {"id", "bad-annual"}, {"playlist", "seasonal"},
            {"period", {{"type", "annual-date"}, {"date", "02-30"}}}
        },
        {
            {"id", "bad-format"}, {"playlist", "seasonal"},
            {"period", {
                {"type", "annual-range"}, {"start", "1-01"}, {"end", "13-01"}
            }}
        },
        {
            {"id", "bad-easter-order"}, {"playlist", "seasonal"},
            {"period", {
                {"type", "easter-range"},
                {"startOffsetDays", 10}, {"endOffsetDays", -10}
            }}
        },
        {
            {"id", "bad-easter-bounds"}, {"playlist", "seasonal"},
            {"period", {
                {"type", "easter-range"},
                {"startOffsetDays", -367}, {"endOffsetDays", 367}
            }}
        },
        {
            {"id", "bad-priority-low"}, {"playlist", "seasonal"},
            {"priority", -1001}
        },
        {
            {"id", "bad-priority-high"}, {"playlist", "seasonal"},
            {"priority", 1001}
        }
    });
    document["cinema"]["resume"] = {
        {"maxAgeMinutes", 10081}, {"checkpointIntervalSeconds", 0}
    };
    document["cinema"]["safety"]["interlock"] = {
        {"enabled", true},
        {"expectedHeartbeatSource", "fire-panel"},
        {"heartbeatTimeoutMilliseconds", 60001},
        {"startupGraceMilliseconds", -1},
        {"safeStableMilliseconds", 60001}
    };
    const TemporaryConfigFile file("tsm_cinema_invalid_boundaries.json", document);

    std::vector<ConfigValidationIssue> issues;
    std::string error;
    EXPECT_FALSE(ValidateAppConfig(file.Path().string(), issues, error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(HasIssuePath(issues, ".priority"));
    EXPECT_TRUE(HasIssuePath(issues, ".period.date"));
    EXPECT_TRUE(HasIssuePath(issues, ".period.start"));
    EXPECT_TRUE(HasIssuePath(issues, ".period.end"));
    EXPECT_TRUE(HasIssueMessage(issues, "end on or after"));
    EXPECT_TRUE(HasIssueMessage(issues, "ordered range"));
    EXPECT_TRUE(HasIssuePath(issues, ".resume.maxAgeMinutes"));
    EXPECT_TRUE(HasIssuePath(issues, ".resume.checkpointIntervalSeconds"));
    EXPECT_TRUE(HasIssuePath(issues, ".heartbeatTimeoutMilliseconds"));
    EXPECT_TRUE(HasIssuePath(issues, ".startupGraceMilliseconds"));
    EXPECT_TRUE(HasIssuePath(issues, ".safeStableMilliseconds"));
}

TEST(ConfigTests, RejectsEmptyPlaylistAndMissingOrEmptyEvacuationReference)
{
    nlohmann::json document = MinimalCinemaDocument();
    document["playlists"][0]["tracks"] = nlohmann::json::array();
    document["cinema"]["schedules"] = nlohmann::json::array({{
        {"id", "empty"}, {"playlist", "seasonal"}
    }});
    document["cinema"]["safety"].erase("evacuationAnnouncementId");
    TemporaryConfigFile missingFile("tsm_cinema_missing_safety_ref.json", document);

    std::vector<ConfigValidationIssue> issues;
    std::string error;
    EXPECT_FALSE(ValidateAppConfig(missingFile.Path().string(), issues, error));
    EXPECT_TRUE(HasIssueMessage(issues, "at least one configured track"));
    EXPECT_TRUE(HasIssuePath(issues, ".evacuationAnnouncementId"));
    EXPECT_TRUE(HasIssueMessage(issues, "non-empty evacuation announcement"));

    document["cinema"]["safety"]["evacuationAnnouncementId"] = "";
    TemporaryConfigFile emptyFile("tsm_cinema_empty_safety_ref.json", document);
    issues.clear();
    EXPECT_FALSE(ValidateAppConfig(emptyFile.Path().string(), issues, error));
    EXPECT_TRUE(HasIssueMessage(issues, "non-empty evacuation announcement"));
}

TEST(ConfigTests, RejectsEvacuationPresetUsedAsDailyAnnouncement)
{
    nlohmann::json document = MinimalCinemaDocument();
    document["announcements"][0]["hour"] = 9;
    document["announcements"][0]["minute"] = 0;
    const TemporaryConfigFile file("tsm_cinema_scheduled_evacuation.json", document);

    std::vector<ConfigValidationIssue> issues;
    std::string error;
    EXPECT_FALSE(ValidateAppConfig(file.Path().string(), issues, error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(HasIssueMessage(
        issues, "cannot also be a daily announcement"));
}

TEST(ConfigTests, EnforcesMaximumCinemaScheduleCount)
{
    nlohmann::json document = MinimalCinemaDocument();
    auto& schedules = document["cinema"]["schedules"];
    for (int index = 0; index < 1024; ++index)
    {
        schedules.push_back({
            {"id", "schedule-" + std::to_string(index)},
            {"playlist", "seasonal"}
        });
    }
    const TemporaryConfigFile accepted("tsm_cinema_1024_schedules.json", document);
    std::vector<ConfigValidationIssue> issues;
    std::string error;
    EXPECT_TRUE(ValidateAppConfig(accepted.Path().string(), issues, error)) << error;
    EXPECT_TRUE(issues.empty());

    schedules.push_back({{"id", "schedule-1024"}, {"playlist", "seasonal"}});
    const TemporaryConfigFile rejected("tsm_cinema_1025_schedules.json", document);
    EXPECT_FALSE(ValidateAppConfig(rejected.Path().string(), issues, error));
    EXPECT_TRUE(HasIssuePath(issues, "$.cinema.schedules"));
    EXPECT_TRUE(HasIssueMessage(issues, "At most 1024"));
}

} // namespace TSM::Tests
