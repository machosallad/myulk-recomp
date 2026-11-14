#include "core/calendar.hpp"
#include "test.hpp"

using myulk::Calendar;
using myulk::Season;

TEST(calendar_new_game_is_first_of_march_year_1) {
    CHECK_EQ(Calendar::year(Calendar::kNewGameDay), 1);
    CHECK_EQ(Calendar::month0(Calendar::kNewGameDay), 2);   // March
    CHECK_EQ(Calendar::dayOfMonth(Calendar::kNewGameDay), 1);
    CHECK(Calendar::season(Calendar::kNewGameDay) == Season::Spring);
}

TEST(calendar_chapter_start_days_are_first_of_march) {
    for (int day : {Calendar::kChapter2Day, Calendar::kChapter3Day}) {
        CHECK_EQ(Calendar::month0(day), 2);
        CHECK_EQ(Calendar::dayOfMonth(day), 1);
    }
    CHECK_EQ(Calendar::year(Calendar::kChapter2Day), 2);
    CHECK_EQ(Calendar::year(Calendar::kChapter3Day), 3);
}

TEST(calendar_month_boundaries) {
    CHECK_EQ(Calendar::month0(1), 0);     // 1 Jan
    CHECK_EQ(Calendar::month0(30), 0);    // 30 Jan
    CHECK_EQ(Calendar::month0(31), 1);    // 1 Feb
    CHECK_EQ(Calendar::dayOfMonth(30), 30);
    CHECK_EQ(Calendar::month0(359), 11);  // 29 Dec
    // day mod 360 == 0: Pascal's truncating div gives month 0, as the original
    CHECK_EQ(Calendar::month0(360), 0);
    CHECK_EQ(Calendar::year(360), 2);
}

TEST(calendar_seasons) {
    CHECK(Calendar::season(1) == Season::Winter);     // Jan
    CHECK(Calendar::season(61) == Season::Spring);    // Mar
    CHECK(Calendar::season(151) == Season::Summer);   // Jun
    CHECK(Calendar::season(241) == Season::Autumn);   // Sep
    CHECK(Calendar::season(331) == Season::Winter);   // Dec
}

TEST(calendar_season_change_detection) {
    CHECK(Calendar::seasonChanged(1));
    CHECK(Calendar::seasonChanged(61));     // Feb -> Mar
    CHECK(!Calendar::seasonChanged(62));
    CHECK(Calendar::seasonChangesTomorrow(420));   // last day of winter, year 2
    CHECK(Calendar::season(420) == Season::Winter);
}
