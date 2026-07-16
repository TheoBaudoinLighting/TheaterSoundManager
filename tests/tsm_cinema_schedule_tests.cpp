#include "pch.h"

#include "tsm_cinema_schedule.h"

#include <array>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace TSM::Tests
{
namespace
{

CinemaCivilTime CivilTime(
    int yearValue, unsigned int monthValue, unsigned int dayValue,
    int hour = 0, int minute = 0)
{
    const std::chrono::year_month_day date =
        std::chrono::year{yearValue} / std::chrono::month{monthValue} /
        std::chrono::day{dayValue};
    const std::chrono::sys_days days{date};

    CinemaCivilTime result;
    result.year = yearValue;
    result.month = static_cast<int>(monthValue);
    result.day = static_cast<int>(dayValue);
    result.isoWeekday = static_cast<int>(std::chrono::weekday{days}.iso_encoding());
    result.minuteOfDay = hour * 60 + minute;
    return result;
}

CinemaSchedule BaseSchedule(
    std::string id = "schedule", std::string playlist = "playlist")
{
    CinemaSchedule result;
    result.id = std::move(id);
    result.playlist = std::move(playlist);
    return result;
}

CinemaSchedule AnnualDateSchedule(int month, int day)
{
    CinemaSchedule result = BaseSchedule();
    result.period.kind = CinemaPeriodKind::AnnualDate;
    result.period.start.month = month;
    result.period.start.day = day;
    return result;
}

void ExpectInvalid(CinemaSchedule schedule)
{
    std::string error;
    EXPECT_FALSE(ValidateCinemaSchedule(schedule, error));
    EXPECT_FALSE(error.empty());
}

} // namespace

TEST(CinemaScheduleTests, PeriodKindNamesAndParsingRoundTrip)
{
    const std::array<std::pair<CinemaPeriodKind, const char*>, 6> values = {{
        {CinemaPeriodKind::Always, "always"},
        {CinemaPeriodKind::AbsoluteDate, "date"},
        {CinemaPeriodKind::AbsoluteRange, "date-range"},
        {CinemaPeriodKind::AnnualDate, "annual-date"},
        {CinemaPeriodKind::AnnualRange, "annual-range"},
        {CinemaPeriodKind::EasterRange, "easter-range"}
    }};

    for (const auto& [expectedKind, name] : values)
    {
        EXPECT_STREQ(CinemaPeriodKindName(expectedKind), name);
        CinemaPeriodKind parsed = CinemaPeriodKind::Always;
        EXPECT_TRUE(TryParseCinemaPeriodKind(name, parsed));
        EXPECT_EQ(parsed, expectedKind);
    }

    CinemaPeriodKind unchanged = CinemaPeriodKind::AnnualRange;
    EXPECT_FALSE(TryParseCinemaPeriodKind("ANNUAL-RANGE", unchanged));
    EXPECT_EQ(unchanged, CinemaPeriodKind::AnnualRange);
    EXPECT_STREQ(
        CinemaPeriodKindName(static_cast<CinemaPeriodKind>(999)), "unknown");
}

TEST(CinemaScheduleTests, DateValidationUsesTheProlepticGregorianCalendar)
{
    EXPECT_TRUE(IsValidCinemaDate({2024, 2, 29}));
    EXPECT_TRUE(IsValidCinemaDate({9999, 12, 31}));
    EXPECT_FALSE(IsValidCinemaDate({2023, 2, 29}));
    EXPECT_FALSE(IsValidCinemaDate({2026, 4, 31}));
    EXPECT_FALSE(IsValidCinemaDate({2026, 0, 1}));
    EXPECT_FALSE(IsValidCinemaDate({2026, -255, 1}));
    EXPECT_FALSE(IsValidCinemaDate({2026, 1, -255}));
    EXPECT_FALSE(IsValidCinemaDate({0, 1, 1}));
    EXPECT_FALSE(IsValidCinemaDate({10000, 1, 1}));
}

TEST(CinemaScheduleTests, ValidatesEveryScheduleInvariant)
{
    CinemaSchedule valid = BaseSchedule();
    valid.priority = 1000;
    valid.windowStartMinute = 23 * 60;
    valid.windowEndMinute = 2 * 60;
    valid.isoWeekdays = {1, 3, 7};
    valid.period.kind = CinemaPeriodKind::AbsoluteRange;
    valid.period.start = {2026, 1, 1};
    valid.period.end = {2026, 12, 31};
    std::string error;
    EXPECT_TRUE(ValidateCinemaSchedule(valid, error)) << error;
    EXPECT_TRUE(error.empty());

    CinemaSchedule invalid = valid;
    invalid.id.clear();
    ExpectInvalid(invalid);

    invalid = valid;
    invalid.playlist.clear();
    ExpectInvalid(invalid);

    invalid = valid;
    invalid.priority = 1001;
    ExpectInvalid(invalid);
    invalid.priority = -1001;
    ExpectInvalid(invalid);

    invalid = valid;
    invalid.windowStartMinute = -1;
    ExpectInvalid(invalid);
    invalid.windowStartMinute = 1440;
    ExpectInvalid(invalid);
    invalid = valid;
    invalid.windowEndMinute = 1440;
    ExpectInvalid(invalid);

    invalid = valid;
    invalid.isoWeekdays = {0};
    ExpectInvalid(invalid);
    invalid.isoWeekdays = {8};
    ExpectInvalid(invalid);
    invalid.isoWeekdays = {1, 1};
    ExpectInvalid(invalid);

    invalid = BaseSchedule();
    invalid.period.kind = CinemaPeriodKind::AbsoluteDate;
    invalid.period.start = {2026, 2, 29};
    ExpectInvalid(invalid);

    invalid.period.kind = CinemaPeriodKind::AbsoluteRange;
    invalid.period.start = {2026, 12, 31};
    invalid.period.end = {2026, 1, 1};
    ExpectInvalid(invalid);

    invalid.period.kind = CinemaPeriodKind::AnnualDate;
    invalid.period.start = {1970, 4, 31};
    ExpectInvalid(invalid);
    invalid.period.start = {1970, -255, 1};
    ExpectInvalid(invalid);

    invalid.period.kind = CinemaPeriodKind::AnnualRange;
    invalid.period.start = {1970, 12, 1};
    invalid.period.end = {1970, 13, 1};
    ExpectInvalid(invalid);

    invalid.period.kind = CinemaPeriodKind::EasterRange;
    invalid.period.startOffsetDays = 2;
    invalid.period.endOffsetDays = 1;
    ExpectInvalid(invalid);
    invalid.period.startOffsetDays = -367;
    invalid.period.endOffsetDays = 0;
    ExpectInvalid(invalid);
    invalid.period.startOffsetDays = 0;
    invalid.period.endOffsetDays = 367;
    ExpectInvalid(invalid);

    invalid.period.kind = static_cast<CinemaPeriodKind>(999);
    ExpectInvalid(invalid);
}

TEST(CinemaScheduleTests, HalloweenAnnualDateMatchesOnlyOctoberThirtyFirst)
{
    CinemaSchedule halloween = AnnualDateSchedule(10, 31);
    EXPECT_TRUE(IsCinemaScheduleActive(halloween, CivilTime(2026, 10, 31, 12, 0)));
    EXPECT_TRUE(IsCinemaScheduleActive(halloween, CivilTime(2031, 10, 31, 23, 59)));
    EXPECT_FALSE(IsCinemaScheduleActive(halloween, CivilTime(2026, 10, 30, 23, 59)));
    EXPECT_FALSE(IsCinemaScheduleActive(halloween, CivilTime(2026, 11, 1, 0, 0)));
}

TEST(CinemaScheduleTests, ChristmasDecemberRangeIncludesBothBoundaries)
{
    CinemaSchedule christmas = BaseSchedule();
    christmas.period.kind = CinemaPeriodKind::AnnualRange;
    christmas.period.start = {1970, 12, 1};
    christmas.period.end = {1970, 12, 31};

    EXPECT_FALSE(IsCinemaScheduleActive(christmas, CivilTime(2026, 11, 30, 23, 59)));
    EXPECT_TRUE(IsCinemaScheduleActive(christmas, CivilTime(2026, 12, 1, 0, 0)));
    EXPECT_TRUE(IsCinemaScheduleActive(christmas, CivilTime(2026, 12, 31, 23, 59)));
    EXPECT_FALSE(IsCinemaScheduleActive(christmas, CivilTime(2027, 1, 1, 0, 0)));
}

TEST(CinemaScheduleTests, AnnualRangeCanWrapAcrossNewYear)
{
    CinemaSchedule holidays = BaseSchedule();
    holidays.period.kind = CinemaPeriodKind::AnnualRange;
    holidays.period.start = {1970, 12, 1};
    holidays.period.end = {1970, 1, 6};

    EXPECT_TRUE(IsCinemaScheduleActive(holidays, CivilTime(2026, 12, 1)));
    EXPECT_TRUE(IsCinemaScheduleActive(holidays, CivilTime(2027, 1, 6, 23, 59)));
    EXPECT_FALSE(IsCinemaScheduleActive(holidays, CivilTime(2026, 11, 30, 23, 59)));
    EXPECT_FALSE(IsCinemaScheduleActive(holidays, CivilTime(2027, 1, 7)));
}

TEST(CinemaScheduleTests, AnnualLeapDayOnlyMatchesInLeapYears)
{
    const CinemaSchedule leapDay = AnnualDateSchedule(2, 29);
    EXPECT_TRUE(IsCinemaScheduleActive(leapDay, CivilTime(2024, 2, 29, 12, 0)));
    EXPECT_FALSE(IsCinemaScheduleActive(leapDay, CivilTime(2024, 3, 1, 12, 0)));
    EXPECT_FALSE(IsCinemaScheduleActive(leapDay, CivilTime(2025, 2, 28, 12, 0)));
}

TEST(CinemaScheduleTests, AbsoluteDateRangeIsClosedAndDoesNotRepeat)
{
    CinemaSchedule festival = BaseSchedule();
    festival.period.kind = CinemaPeriodKind::AbsoluteRange;
    festival.period.start = {2026, 7, 10};
    festival.period.end = {2026, 7, 16};

    EXPECT_FALSE(IsCinemaScheduleActive(festival, CivilTime(2026, 7, 9, 23, 59)));
    EXPECT_TRUE(IsCinemaScheduleActive(festival, CivilTime(2026, 7, 10)));
    EXPECT_TRUE(IsCinemaScheduleActive(festival, CivilTime(2026, 7, 16, 23, 59)));
    EXPECT_FALSE(IsCinemaScheduleActive(festival, CivilTime(2026, 7, 17)));
    EXPECT_FALSE(IsCinemaScheduleActive(festival, CivilTime(2027, 7, 10)));
}

TEST(CinemaScheduleTests, GregorianEasterMatchesKnownDates)
{
    const std::array<std::pair<int, CinemaDate>, 6> expected = {{
        {2008, {2008, 3, 23}},
        {2024, {2024, 3, 31}},
        {2025, {2025, 4, 20}},
        {2026, {2026, 4, 5}},
        {2038, {2038, 4, 25}},
        {2049, {2049, 4, 18}}
    }};

    for (const auto& [yearValue, expectedDate] : expected)
    {
        const CinemaDate actual = GregorianEasterDate(yearValue);
        EXPECT_EQ(actual.year, expectedDate.year) << yearValue;
        EXPECT_EQ(actual.month, expectedDate.month) << yearValue;
        EXPECT_EQ(actual.day, expectedDate.day) << yearValue;
    }
}

TEST(CinemaScheduleTests, EasterWeekUsesInclusiveOffsets)
{
    CinemaSchedule easterWeek = BaseSchedule();
    easterWeek.period.kind = CinemaPeriodKind::EasterRange;
    easterWeek.period.startOffsetDays = -6;
    easterWeek.period.endOffsetDays = 1;

    EXPECT_FALSE(IsCinemaScheduleActive(easterWeek, CivilTime(2026, 3, 29)));
    EXPECT_TRUE(IsCinemaScheduleActive(easterWeek, CivilTime(2026, 3, 30)));
    EXPECT_TRUE(IsCinemaScheduleActive(easterWeek, CivilTime(2026, 4, 5)));
    EXPECT_TRUE(IsCinemaScheduleActive(easterWeek, CivilTime(2026, 4, 6, 23, 59)));
    EXPECT_FALSE(IsCinemaScheduleActive(easterWeek, CivilTime(2026, 4, 7)));
}

TEST(CinemaScheduleTests, EasterOffsetsCanCrossCivilYearBoundaries)
{
    CinemaSchedule easterOffset = BaseSchedule();
    easterOffset.period.kind = CinemaPeriodKind::EasterRange;
    easterOffset.period.startOffsetDays = 300;
    easterOffset.period.endOffsetDays = 300;

    // Three hundred days after Easter 2025 (20 April) is 14 February 2026.
    EXPECT_TRUE(IsCinemaScheduleActive(easterOffset, CivilTime(2026, 2, 14)));
    EXPECT_FALSE(IsCinemaScheduleActive(easterOffset, CivilTime(2026, 2, 15)));
}

TEST(CinemaScheduleTests, WeekdayFilterUsesIsoMondayThroughSunday)
{
    CinemaSchedule weekdays = BaseSchedule();
    weekdays.isoWeekdays = {1, 2, 3, 4, 5};

    EXPECT_TRUE(IsCinemaScheduleActive(weekdays, CivilTime(2026, 7, 13, 12, 0)));
    EXPECT_TRUE(IsCinemaScheduleActive(weekdays, CivilTime(2026, 7, 17, 12, 0)));
    EXPECT_FALSE(IsCinemaScheduleActive(weekdays, CivilTime(2026, 7, 18, 12, 0)));
    EXPECT_FALSE(IsCinemaScheduleActive(weekdays, CivilTime(2026, 7, 19, 12, 0)));
}

TEST(CinemaScheduleTests, OvernightWindowIsAnchoredToItsStartingCivilDate)
{
    CinemaSchedule halloweenNight = AnnualDateSchedule(10, 31);
    halloweenNight.windowStartMinute = 20 * 60;
    halloweenNight.windowEndMinute = 2 * 60;
    halloweenNight.isoWeekdays = {6}; // Halloween 2026 is a Saturday.

    EXPECT_FALSE(IsCinemaScheduleActive(
        halloweenNight, CivilTime(2026, 10, 31, 19, 59)));
    EXPECT_TRUE(IsCinemaScheduleActive(
        halloweenNight, CivilTime(2026, 10, 31, 20, 0)));
    EXPECT_TRUE(IsCinemaScheduleActive(
        halloweenNight, CivilTime(2026, 11, 1, 1, 59)));
    EXPECT_FALSE(IsCinemaScheduleActive(
        halloweenNight, CivilTime(2026, 11, 1, 2, 0)));
}

TEST(CinemaScheduleTests, WindowEndIsExclusiveAndEqualEndpointsMeanFullDay)
{
    CinemaSchedule daytime = BaseSchedule();
    daytime.windowStartMinute = 9 * 60;
    daytime.windowEndMinute = 18 * 60;
    EXPECT_FALSE(IsCinemaScheduleActive(daytime, CivilTime(2026, 7, 16, 8, 59)));
    EXPECT_TRUE(IsCinemaScheduleActive(daytime, CivilTime(2026, 7, 16, 9, 0)));
    EXPECT_TRUE(IsCinemaScheduleActive(daytime, CivilTime(2026, 7, 16, 17, 59)));
    EXPECT_FALSE(IsCinemaScheduleActive(daytime, CivilTime(2026, 7, 16, 18, 0)));

    CinemaSchedule fullDay = BaseSchedule();
    fullDay.windowStartMinute = 8 * 60;
    fullDay.windowEndMinute = 8 * 60;
    EXPECT_TRUE(IsCinemaScheduleActive(fullDay, CivilTime(2026, 7, 16, 0, 0)));
    EXPECT_TRUE(IsCinemaScheduleActive(fullDay, CivilTime(2026, 7, 16, 23, 59)));
}

TEST(CinemaScheduleTests, ResolutionUsesDescendingPriorityThenAscendingId)
{
    CinemaSchedule low = BaseSchedule("low", "ordinary");
    low.priority = 10;
    CinemaSchedule zulu = BaseSchedule("zulu", "seasonal-z");
    zulu.priority = 100;
    CinemaSchedule alpha = BaseSchedule("alpha", "seasonal-a");
    alpha.priority = 100;

    const std::vector<CinemaSchedule> schedules = {low, zulu, alpha};
    const CinemaScheduleResolution resolution =
        ResolveCinemaSchedule(schedules, CivilTime(2026, 7, 16, 12, 0));

    ASSERT_NE(resolution.selected, nullptr);
    EXPECT_EQ(resolution.selected->id, "alpha");
    ASSERT_EQ(resolution.matches.size(), 3u);
    EXPECT_EQ(resolution.matches[0]->id, "alpha");
    EXPECT_EQ(resolution.matches[1]->id, "zulu");
    EXPECT_EQ(resolution.matches[2]->id, "low");
}

TEST(CinemaScheduleTests, ResolutionIgnoresDisabledInactiveAndInvalidEntries)
{
    CinemaSchedule disabled = BaseSchedule("disabled");
    disabled.enabled = false;

    CinemaSchedule inactive = AnnualDateSchedule(10, 31);
    inactive.id = "inactive";

    CinemaSchedule invalid = BaseSchedule("invalid");
    invalid.priority = 1001;

    const std::vector<CinemaSchedule> schedules = {disabled, inactive, invalid};
    const CinemaScheduleResolution resolution =
        ResolveCinemaSchedule(schedules, CivilTime(2026, 7, 16, 12, 0));
    EXPECT_EQ(resolution.selected, nullptr);
    EXPECT_TRUE(resolution.matches.empty());
}

TEST(CinemaScheduleTests, InvalidCivilInputAndWeekdayMismatchFailClosed)
{
    const CinemaSchedule schedule = BaseSchedule();

    CinemaCivilTime invalidDate = CivilTime(2026, 7, 16, 12, 0);
    invalidDate.day = 32;
    EXPECT_FALSE(IsCinemaScheduleActive(schedule, invalidDate));

    CinemaCivilTime invalidMinute = CivilTime(2026, 7, 16, 12, 0);
    invalidMinute.minuteOfDay = 1440;
    EXPECT_FALSE(IsCinemaScheduleActive(schedule, invalidMinute));

    CinemaCivilTime inconsistentWeekday = CivilTime(2026, 7, 16, 12, 0);
    inconsistentWeekday.isoWeekday = inconsistentWeekday.isoWeekday == 7
        ? 1
        : inconsistentWeekday.isoWeekday + 1;
    EXPECT_FALSE(IsCinemaScheduleActive(schedule, inconsistentWeekday));
}

TEST(CinemaScheduleTests, CivilDstSpringGapKeepsHalfOpenWallClockSemantics)
{
    CinemaSchedule sundayWindow = BaseSchedule();
    sundayWindow.isoWeekdays = {7};
    sundayWindow.windowStartMinute = 90;  // 01:30 local.
    sundayWindow.windowEndMinute = 210;   // 03:30 local.

    // Europe/Paris jumps from 01:59 to 03:00 on 29 March 2026. The pure
    // calendar consumes civil time, so it remains active on both observable
    // sides of the skipped interval and stops at the configured civil end.
    EXPECT_TRUE(IsCinemaScheduleActive(
        sundayWindow, CivilTime(2026, 3, 29, 1, 59)));
    EXPECT_TRUE(IsCinemaScheduleActive(
        sundayWindow, CivilTime(2026, 3, 29, 3, 0)));
    EXPECT_FALSE(IsCinemaScheduleActive(
        sundayWindow, CivilTime(2026, 3, 29, 3, 30)));
}

TEST(CinemaScheduleTests, CivilDstRepeatedHourHasIdenticalCalendarResolution)
{
    CinemaSchedule sundayWindow = BaseSchedule("dst", "autumn");
    sundayWindow.isoWeekdays = {7};
    sundayWindow.windowStartMinute = 90;  // 01:30 local.
    sundayWindow.windowEndMinute = 210;   // 03:30 local.

    // Europe/Paris observes 02:30 twice on 25 October 2026. UTC offset/fold is
    // deliberately outside this pure civil predicate: both occurrences must
    // resolve to the same schedule, while occurrence de-duplication belongs to
    // the stateful runtime scheduler.
    const CinemaCivilTime firstFold = CivilTime(2026, 10, 25, 2, 30);
    const CinemaCivilTime secondFold = firstFold;
    const std::vector<CinemaSchedule> schedules = {sundayWindow};
    const auto first = ResolveCinemaSchedule(schedules, firstFold);
    const auto second = ResolveCinemaSchedule(schedules, secondFold);
    ASSERT_NE(first.selected, nullptr);
    ASSERT_NE(second.selected, nullptr);
    EXPECT_EQ(first.selected->id, "dst");
    EXPECT_EQ(second.selected->id, "dst");
}

TEST(CinemaScheduleTests, LocalClockConversionReturnsSelfConsistentCivilTime)
{
    const CinemaCivilTime local =
        GetLocalCinemaTime(std::chrono::system_clock::now());
    ASSERT_TRUE(IsValidCinemaDate(local));
    EXPECT_GE(local.minuteOfDay, 0);
    EXPECT_LT(local.minuteOfDay, 1440);

    const std::chrono::sys_days days{
        std::chrono::year{local.year} /
        std::chrono::month{static_cast<unsigned int>(local.month)} /
        std::chrono::day{static_cast<unsigned int>(local.day)}};
    EXPECT_EQ(
        local.isoWeekday,
        static_cast<int>(std::chrono::weekday{days}.iso_encoding()));
}

} // namespace TSM::Tests
