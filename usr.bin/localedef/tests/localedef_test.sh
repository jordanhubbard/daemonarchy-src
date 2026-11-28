# SPDX-License-Identifier: BSD-2-Clause

atf_test_case gb18030
gb18030_head()
{
	atf_set "descr" "GB18030 locale data uses libc's wide encoding and preserves symbolic strings"
	atf_set "require.progs" "localedef"
}
gb18030_body()
{
	atf_check -s exit:0 -o empty -e empty localedef -D -U -c \
	    -f "$(atf_get_srcdir)/gb18030.charmap" \
	    -i "$(atf_get_srcdir)/gb18030.src" "${PWD}/generated"
	mkdir -p locales/localedef.GB18030
	ln -s "${PWD}/generated.LC_CTYPE" locales/localedef.GB18030/LC_CTYPE
	ln -s "${PWD}/generated.LC_MESSAGES" locales/localedef.GB18030/LC_MESSAGES
	atf_check -s exit:0 -o empty -e empty env PATH_LOCALE="${PWD}/locales" \
	    "$(atf_get_srcdir)/gb18030_helper"
}

atf_init_test_cases()
{
	atf_add_test_case gb18030
}
