#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace TSM
{

enum class CinemaPeriodKind
{
    Always,
    AbsoluteDate,
    AbsoluteRange,
    AnnualDate,
    AnnualRange,
    EasterRange
};

struct CinemaDate
{
    int year = 1970;
    int month = 1;
    int day = 1;
};

struct CinemaCivilTime : CinemaDate
{
    int isoWeekday = 4; // Monday=1, Sunday=7.
    int minuteOfDay = 0;
};

struct CinemaPeriod
{
    CinemaPeriodKind kind = CinemaPeriodKind::Always;
    CinemaDate start;
    CinemaDate end;
    int startOffsetDays = 0;
    int endOffsetDays = 0;
};

struct CinemaSchedule
{
    std::string id;
    std::string playlist;
    bool enabled = true;
    int priority = 0;
    CinemaPeriod period;
    int windowStartMinute = 0;
    int windowEndMinute = 0; // Equal endpoints mean a full local day.
    std::vector<int> isoWeekdays;
};

struct CinemaScheduleResolution
{
    const CinemaSchedule* selected = nullptr;
    std::vector<const CinemaSchedule*> matches;
};

const char* CinemaPeriodKindName(CinemaPeriodKind kind);
bool TryParseCinemaPeriodKind(const std::string& value, CinemaPeriodKind& kind);
bool IsValidCinemaDate(const CinemaDate& date);
bool ValidateCinemaSchedule(const CinemaSchedule& schedule, std::string& errorMessage);
bool IsCinemaScheduleActive(const CinemaSchedule& schedule, const CinemaCivilTime& localTime);
CinemaScheduleResolution ResolveCinemaSchedule(
    const std::vector<CinemaSchedule>& schedules,
    const CinemaCivilTime& localTime);
CinemaCivilTime GetLocalCinemaTime(std::chrono::system_clock::time_point timePoint);
CinemaDate GregorianEasterDate(int year);

} // namespace TSM
