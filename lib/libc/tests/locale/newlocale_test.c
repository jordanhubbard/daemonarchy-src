/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2023 Yuri Pankov <yuripv@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>

#include <limits.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <atf-c.h>

struct {
	int		lpmask;
	const char	*lpname;
} lparts[] = {
	{ LC_COLLATE_MASK,	"LC_COLLATE" },
	{ LC_CTYPE_MASK,	"LC_CTYPE" },
	{ LC_MONETARY_MASK,	"LC_MONETARY" },
	{ LC_NUMERIC_MASK,	"LC_NUMERIC" },
	{ LC_TIME_MASK,		"LC_TIME" },
	{ LC_MESSAGES_MASK,	"LC_MESSAGES" },
};

static void
check_lparts(const char *expected)
{
	int i;

	for (i = 0; i < nitems(lparts); i++) {
		const char *actual;

		actual = querylocale(lparts[i].lpmask, uselocale(NULL));
		ATF_CHECK_STREQ_MSG(expected, actual, "wrong value for %s",
		    lparts[i].lpname);
	}
}

static void
do_locale_switch(const char *loc1, const char *loc2)
{
	locale_t l1, l2;

	/* Create and use the first locale */
	l1 = newlocale(LC_ALL_MASK, loc1, NULL);
	ATF_REQUIRE(l1 != NULL);
	ATF_REQUIRE(uselocale(l1) != NULL);
	check_lparts(loc1);
	/*
	 * Create and use second locale, creation deliberately done only after
	 * the first locale check as newlocale() call would previously clobber
	 * the first locale contents.
	 */
	l2 = newlocale(LC_ALL_MASK, loc2, NULL);
	ATF_REQUIRE(l2 != NULL);
	ATF_REQUIRE(uselocale(l2) != NULL);
	check_lparts(loc2);
	/* Switch back to first locale */
	ATF_REQUIRE(uselocale(l1) != NULL);
	check_lparts(loc1);

	freelocale(l1);
	freelocale(l2);
}

/*
 * PR 255646, 269375: Check that newlocale()/uselocale() used to switch between
 * C, POSIX, and C.UTF-8 locales (and only these) do not stomp on other locale
 * contents (collate part specifically).
 * The issue is cosmetic only as all three have empty collate parts, but we need
 * to correctly report the one in use in any case.
 */

ATF_TC_WITHOUT_HEAD(newlocale_c_posix_cu8_test);
ATF_TC_BODY(newlocale_c_posix_cu8_test, tc)
{
	do_locale_switch("C", "POSIX");
	do_locale_switch("C", "C.UTF-8");
	do_locale_switch("POSIX", "C");
	do_locale_switch("POSIX", "C.UTF-8");
	do_locale_switch("C.UTF-8", "C");
	do_locale_switch("C.UTF-8", "POSIX");
}

static void
check_big5_numeric(void)
{
	const struct lconv *lc;
	const char input[] = "\xa4\x40";
	char output[MB_LEN_MAX];
	mbstate_t state = { 0 };
	wchar_t wc;

	lc = localeconv();
	ATF_CHECK_STREQ(lc->decimal_point, "\xa1\x44");
	ATF_CHECK_STREQ(lc->thousands_sep, "\xa1\x41");
	ATF_CHECK_EQ(lc->grouping[0], 3);
	ATF_CHECK_EQ(lc->grouping[1], '\0');
	ATF_REQUIRE_EQ(MB_CUR_MAX, 2);
	ATF_REQUIRE_EQ(mbrtowc(&wc, input, sizeof(input) - 1, &state), 2);
	memset(&state, 0, sizeof(state));
	ATF_REQUIRE_EQ(wcrtomb(output, wc, &state), 2);
	ATF_CHECK_EQ(memcmp(input, output, sizeof(input) - 1), 0);
}

/* The numeric definition must be installed with the other Big5 categories. */
ATF_TC_WITHOUT_HEAD(newlocale_big5_test);
ATF_TC_BODY(newlocale_big5_test, tc)
{
	locale_t loc, previous;

	loc = newlocale(LC_ALL_MASK, "zh_TW.Big5", NULL);
	ATF_REQUIRE_MSG(loc != NULL, "cannot load the complete Big5 locale");
	previous = uselocale(loc);
	ATF_REQUIRE(previous != NULL);
	check_lparts("zh_TW.Big5");
	check_big5_numeric();
	ATF_REQUIRE(uselocale(previous) != NULL);
	freelocale(loc);
}

ATF_TC_WITHOUT_HEAD(setlocale_big5_test);
ATF_TC_BODY(setlocale_big5_test, tc)
{

	ATF_REQUIRE_MSG(setlocale(LC_ALL, "zh_TW.Big5") != NULL,
	    "cannot load the complete Big5 locale");
	check_big5_numeric();
	ATF_REQUIRE(setlocale(LC_ALL, "C") != NULL);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, newlocale_c_posix_cu8_test);
	ATF_TP_ADD_TC(tp, newlocale_big5_test);
	ATF_TP_ADD_TC(tp, setlocale_big5_test);

	return (atf_no_error());
}
