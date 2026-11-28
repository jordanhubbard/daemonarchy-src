/* SPDX-License-Identifier: BSD-2-Clause */

#include <sys/param.h>

#include <err.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

int
main(void)
{
	static const struct {
		const char *name;
		char upper[5];
		char lower[5];
		size_t length;
		wchar_t upper_wc;
		wchar_t lower_wc;
	} cases[] = {
		{ "ASCII", "A", "a", 1, 0x41, 0x61 },
		{ "fullwidth G", "\xa3\xc7", "\xa3\xe7", 2, 0xa3c7, 0xa3e7 },
		{ "L with stroke", "\x81\x30\x91\x39", "\x81\x30\x92\x30",
		    4, 0x01309139, 0x01309230 },
	};
	mbstate_t state;
	wchar_t upper, lower;
	char bytes[MB_LEN_MAX];
	size_t i, length;

	if (setlocale(LC_CTYPE, "localedef.GB18030") == NULL)
		err(EXIT_FAILURE, "setlocale LC_CTYPE");
	for (i = 0; i < nitems(cases); i++) {
		memset(&state, 0, sizeof(state));
		length = mbrtowc(&upper, cases[i].upper, cases[i].length, &state);
		if (length != cases[i].length || upper != cases[i].upper_wc)
			errx(EXIT_FAILURE, "%s: upper wide conversion", cases[i].name);
		memset(&state, 0, sizeof(state));
		length = mbrtowc(&lower, cases[i].lower, cases[i].length, &state);
		if (length != cases[i].length || lower != cases[i].lower_wc)
			errx(EXIT_FAILURE, "%s: lower wide conversion", cases[i].name);
		if (!iswalpha(upper) || !iswupper(upper) || iswlower(upper) ||
		    !iswalpha(lower) || !iswlower(lower) || iswupper(lower))
			errx(EXIT_FAILURE, "%s: character classification", cases[i].name);
		if (towlower(upper) != (wint_t)lower ||
		    towupper(lower) != (wint_t)upper)
			errx(EXIT_FAILURE, "%s: case mapping", cases[i].name);
		memset(&state, 0, sizeof(state));
		length = wcrtomb(bytes, upper, &state);
		if (length != cases[i].length ||
		    memcmp(bytes, cases[i].upper, length) != 0)
			errx(EXIT_FAILURE, "%s: upper byte round trip", cases[i].name);
		memset(&state, 0, sizeof(state));
		length = wcrtomb(bytes, lower, &state);
		if (length != cases[i].length ||
		    memcmp(bytes, cases[i].lower, length) != 0)
			errx(EXIT_FAILURE, "%s: lower byte round trip", cases[i].name);
	}
	if (setlocale(LC_MESSAGES, "localedef.GB18030") == NULL)
		err(EXIT_FAILURE, "setlocale LC_MESSAGES");
	if (strcmp(nl_langinfo(YESSTR), "\x81\x30\x91\x39") != 0)
		errx(EXIT_FAILURE, "four-byte symbolic string encoding");
	return (EXIT_SUCCESS);
}
