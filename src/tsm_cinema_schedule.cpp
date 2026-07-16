#include "tsm_cinema_schedule.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <tuple>
#include <utility>

namespace TSM
{
namespace
{

using namespace std::chrono;

year_month_day ToYearMonthDay(const CinemaDate& date)
{
    return year{date.year} / month{static_cast<unsigned int>(date.month)} /
        day{static_cast<unsigned int>(date.day)};
}

CinemaDate FromYearMonthDay(const year_month_day& date)
{
    return {
        static_cast<int>(date.year()),
        static_cast<int>(static_cast<unsigned int>(date.month())),
        static_cast<int>(static_cast<unsigned int>(date.day()))
    };
}

CinemaCivilTime PreviousDay(const CinemaCivilTime& value)
{
    const sys_days previous = sys_days{ToYearMonthDay(value)} - days{1};
    const year_month_day date{previous};
    const weekday weekdayValue{previous};
    const CinemaDate civilDate = FromYearMonthDay(date);
    return {
        civilDate.year,
        civilDate.month,
        civilDate.day,
        static_cast<int>(weekdayValue.iso_encoding()),
        value.minuteOfDay
    };
}

bool DateLess(const CinemaDate& left, const CinemaDate& right)
{
    return std::tie(left.year, left.month, left.day) <
        std::tie(right.year, right.month, right.day);
}

bool DateInClosedRange(
    const CinemaDate& value, const CinemaDate& start, const CinemaDate& end)
{
    return !DateLess(value, start) && !DateLess(end, value);
}

bool DatesEqual(const CinemaDate& left, const CinemaDate& right)
{
    return !DateLess(left, right) && !DateLess(right, left);
}

int AnnualKey(const CinemaDate& date)
{
    return date.month * 100 + date.day;
}

bool IsValidAnnualDate(const CinemaDate& date)
{
    if (date.month < 1 || date.month > 12 || date.day < 1 || date.day > 31)
        return false;
    return (year{2000} / month{static_cast<unsigned int>(date.month)} /
            day{static_cast<unsigned int>(date.day)}).ok();
}

bool MatchesPeriod(const CinemaPeriod& period, const CinemaCivilTime& anchor)
{
    const CinemaDate anchorDate{anchor.year, anchor.month, anchor.day};
    switch (period.kind)
    {
        case CinemaPeriodKind::Always:
            return true;
        case CinemaPeriodKind::AbsoluteDate:
            return DatesEqual(anchorDate, period.start);
        case CinemaPeriodKind::AbsoluteRange:
            return DateInClosedRange(anchorDate, period.start, period.end);
        case CinemaPeriodKind::AnnualDate:
            return anchor.month == period.start.month && anchor.day == period.start.day;
        case CinemaPeriodKind::AnnualRange:
        {
            const int value = AnnualKey(anchorDate);
            const int start = AnnualKey(period.start);
            const int end = AnnualKey(period.end);
            return start <= end
                ? value >= start && value <= end
                : value >= start || value <= end;
        }
        case CinemaPeriodKind::EasterRange:
        {
            const sys_days anchorDays{ToYearMonthDay(anchorDate)};
            // Offsets may cross a civil-year boundary. Compare the anchor with
            // Easter occurrences in the adjacent years as well as its own.
            for (const int easterYear : {anchor.year - 1, anchor.year, anchor.year + 1})
            {
                if (easterYear < 1 || easterYear > 9999) continue;
                const sys_days easterDays{
                    ToYearMonthDay(GregorianEasterDate(easterYear))};
                const auto offset = (anchorDays - easterDays).count();
                if (offset >= period.startOffsetDays &&
                    offset <= period.endOffsetDays)
                    return true;
            }
            return false;
        }
    }
    return false;
}

bool SelectWindowAnchor(
    const CinemaSchedule& schedule,
    const CinemaCivilTime& current,
    CinemaCivilTime& anchor)
{
    const int start = schedule.windowStartMinute;
    const int end = schedule.windowEndMinute;
    if (start == end)
    {
        anchor = current;
        return true;
    }
    if (start < end)
    {
        if (current.minuteOfDay < start || current.minuteOfDay >= end) return false;
        anchor = current;
        return true;
    }

    if (current.minuteOfDay >= start)
    {
        anchor = current;
        return true;
    }
    if (current.minuteOfDay < end)
    {
        anchor = PreviousDay(current);
        return true;
    }
    return false;
}

} // namespace

const char* CinemaPeriodKindName(CinemaPeriodKind kind)
{
    switch (kind)
    {
        case CinemaPeriodKind::Always: return "always";
        case CinemaPeriodKind::AbsoluteDate: return "date";
        case CinemaPeriodKind::AbsoluteRange: return "date-range";
        case CinemaPeriodKind::AnnualDate: return "annual-date";
        case CinemaPeriodKind::AnnualRange: return "annual-range";
        case CinemaPeriodKind::EasterRange: return "easter-range";
    }
    return "unknown";
}

bool TryParseCinemaPeriodKind(const std::string& value, CinemaPeriodKind& kind)
{
    static constexpr std::array<std::pair<const char*, CinemaPeriodKind>, 6> values = {{
        {"always", CinemaPeriodKind::Always},
        {"date", CinemaPeriodKind::AbsoluteDate},
        {"date-range", CinemaPeriodKind::AbsoluteRange},
        {"annual-date", CinemaPeriodKind::AnnualDate},
        {"annual-range", CinemaPeriodKind::AnnualRange},
        {"easter-range", CinemaPeriodKind::EasterRange}
    }};
    const auto found = std::find_if(values.begin(), values.end(), [&](const auto& entry)
    {
        return value == entry.first;
    });
    if (found == values.end()) return false;
    kind = found->second;
    return true;
}

bool IsValidCinemaDate(const CinemaDate& date)
{
    return date.year >= 1 && date.year <= 9999 &&
        date.month >= 1 && date.month <= 12 && date.day >= 1 && date.day <= 31 &&
        ToYearMonthDay(date).ok();
}

bool ValidateCinemaSchedule(const CinemaSchedule& schedule, std::string& errorMessage)
{
    errorMessage.clear();
    if (schedule.id.empty())
        errorMessage = "Schedule ID must not be empty.";
    else if (schedule.playlist.empty())
        errorMessage = "Schedule playlist must not be empty.";
    else if (schedule.priority < -1000 || schedule.priority > 1000)
        errorMessage = "Schedule priority must be between -1000 and 1000.";
    else if (schedule.windowStartMinute < 0 || schedule.windowStartMinute >= 1440 ||
             schedule.windowEndMinute < 0 || schedule.windowEndMinute >= 1440)
        errorMessage = "Schedule window endpoints must be local minutes from 0 to 1439.";

    if (!errorMessage.empty()) return false;

    std::array<bool, 8> weekdays{};
    for (const int weekday : schedule.isoWeekdays)
    {
        if (weekday < 1 || weekday > 7)
        {
            errorMessage = "ISO weekdays must be integers from 1 (Monday) to 7 (Sunday).";
            return false;
        }
        if (weekdays[static_cast<std::size_t>(weekday)])
        {
            errorMessage = "ISO weekdays must not contain duplicates.";
            return false;
        }
        weekdays[static_cast<std::size_t>(weekday)] = true;
    }

    const CinemaPeriod& period = schedule.period;
    switch (period.kind)
    {
        case CinemaPeriodKind::Always:
            return true;
        case CinemaPeriodKind::AbsoluteDate:
            if (IsValidCinemaDate(period.start)) return true;
            errorMessage = "Absolute date is invalid.";
            return false;
        case CinemaPeriodKind::AbsoluteRange:
            if (!IsValidCinemaDate(period.start) || !IsValidCinemaDate(period.end))
                errorMessage = "Absolute date range contains an invalid date.";
            else if (DateLess(period.end, period.start))
                errorMessage = "Absolute date range must end on or after it starts.";
            else
                return true;
            return false;
        case CinemaPeriodKind::AnnualDate:
            if (IsValidAnnualDate(period.start)) return true;
            errorMessage = "Annual date is invalid.";
            return false;
        case CinemaPeriodKind::AnnualRange:
        {
            if (IsValidAnnualDate(period.start) && IsValidAnnualDate(period.end))
                return true;
            errorMessage = "Annual date range contains an invalid month or day.";
            return false;
        }
        case CinemaPeriodKind::EasterRange:
            if (period.startOffsetDays < -366 || period.endOffsetDays > 366 ||
                period.startOffsetDays > period.endOffsetDays)
            {
                errorMessage =
                    "Easter offsets must form an ordered range between -366 and 366 days.";
                return false;
            }
            return true;
    }
    errorMessage = "Schedule period type is invalid.";
    return false;
}

bool IsCinemaScheduleActive(
    const CinemaSchedule& schedule, const CinemaCivilTime& localTime)
{
    std::string validationError;
    if (!schedule.enabled || !ValidateCinemaSchedule(schedule, validationError) ||
        !IsValidCinemaDate(localTime) ||
        localTime.isoWeekday < 1 || localTime.isoWeekday > 7 ||
        localTime.minuteOfDay < 0 || localTime.minuteOfDay >= 1440)
        return false;

    const sys_days localDays{ToYearMonthDay(localTime)};
    if (static_cast<int>(weekday{localDays}.iso_encoding()) != localTime.isoWeekday)
        return false;

    CinemaCivilTime anchor;
    if (!SelectWindowAnchor(schedule, localTime, anchor)) return false;
    if (!IsValidCinemaDate(anchor)) return false;
    if (!schedule.isoWeekdays.empty() &&
        std::find(schedule.isoWeekdays.begin(), schedule.isoWeekdays.end(),
                  anchor.isoWeekday) == schedule.isoWeekdays.end())
        return false;
    return MatchesPeriod(schedule.period, anchor);
}

CinemaScheduleResolution ResolveCinemaSchedule(
    const std::vector<CinemaSchedule>& schedules,
    const CinemaCivilTime& localTime)
{
    CinemaScheduleResolution result;
    for (const CinemaSchedule& schedule : schedules)
    {
        if (IsCinemaScheduleActive(schedule, localTime))
            result.matches.push_back(&schedule);
    }
    std::stable_sort(
        result.matches.begin(), result.matches.end(), [](const auto* left, const auto* right)
    {
        if (left->priority != right->priority) return left->priority > right->priority;
        return left->id < right->id;
    });
    if (!result.matches.empty()) result.selected = result.matches.front();
    return result;
}

CinemaCivilTime GetLocalCinemaTime(system_clock::time_point timePoint)
{
    const std::time_t value = system_clock::to_time_t(timePoint);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &value);
#else
    localtime_r(&value, &local);
#endif
    return {
        local.tm_year + 1900,
        local.tm_mon + 1,
        local.tm_mday,
        local.tm_wday == 0 ? 7 : local.tm_wday,
        local.tm_hour * 60 + local.tm_min
    };
}

CinemaDate GregorianEasterDate(int yearValue)
{
    // Meeus/Jones/Butcher algorithm for the Gregorian calendar.
    const int a = yearValue % 19;
    const int b = yearValue / 100;
    const int c = yearValue % 100;
    const int d = b / 4;
    const int e = b % 4;
    const int f = (b + 8) / 25;
    const int g = (b - f + 1) / 3;
    const int h = (19 * a + b - d - g + 15) % 30;
    const int i = c / 4;
    const int k = c % 4;
    const int l = (32 + 2 * e + 2 * i - h - k) % 7;
    const int m = (a + 11 * h + 22 * l) / 451;
    const int monthValue = (h + l - 7 * m + 114) / 31;
    const int dayValue = ((h + l - 7 * m + 114) % 31) + 1;
    return {yearValue, monthValue, dayValue};
}

} // namespace TSM
