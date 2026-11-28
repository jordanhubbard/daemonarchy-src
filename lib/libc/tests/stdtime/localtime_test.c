/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>

#include <stdlib.h>
#include <time.h>

#include <atf-c.h>

struct tzcase {
	const char *zone;
	int day;
	int hour;
	int weekday;
	int yday;
	int isdst;
};

static const struct tzcase cases[] = {
	{ "UTC0", 5, 4, 1, 64, 0 },
	{ "EST5", 4, 23, 0, 63, 0 },
	{ "EST5EDT,M3.2.0,M11.1.0", 4, 23, 0, 63, 0 },
	{ "CET-1CEST,M3.5.0/2,M10.5.0/3", 5, 5, 1, 64, 0 },
	{ "NZST-12NZDT,M9.5.0/2,M4.1.0/3", 5, 17, 1, 64, 1 },
};

static void
check_time(const struct tzcase *tz, const struct tm *tm, const char *api)
{
	ATF_CHECK_EQ(112, tm->tm_year);
	ATF_CHECK_EQ(2, tm->tm_mon);
	ATF_CHECK_EQ(tz->day, tm->tm_mday);
	ATF_CHECK_EQ(tz->hour, tm->tm_hour);
	ATF_CHECK_EQ(59, tm->tm_min);
	ATF_CHECK_EQ(59, tm->tm_sec);
	ATF_CHECK_EQ_MSG(tz->weekday, tm->tm_wday,
	    "%s in %s: expected weekday %d, got %d", api, tz->zone,
	    tz->weekday, tm->tm_wday);
	ATF_CHECK_EQ(tz->yday, tm->tm_yday);
	ATF_CHECK_EQ(tz->isdst, tm->tm_isdst);
}

ATF_TC(posix_rule_weekday);
ATF_TC_HEAD(posix_rule_weekday, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "POSIX DST rules preserve the weekday after year extrapolation");
}
ATF_TC_BODY(posix_rule_weekday, tc)
{
	const time_t then = 1330923599; /* 2012-03-05 04:59:59 UTC */
	struct tm tm, *result;
	size_t i;

	(void)tc;
	for (i = 0; i < nitems(cases); i++) {
		ATF_REQUIRE_EQ(0, setenv("TZ", cases[i].zone, 1));
		tzset();
		ATF_REQUIRE(localtime_r(&then, &tm) != NULL);
		check_time(&cases[i], &tm, "localtime_r");
		ATF_REQUIRE((result = localtime(&then)) != NULL);
		tm = *result;
		check_time(&cases[i], &tm, "localtime");
	}
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, posix_rule_weekday);
	return (atf_no_error());
}
