# SPDX-License-Identifier: BSD-2-Clause

make_fixture()
{

	cat > provider.d <<'EOF'
provider disabled_check {
	probe tick();
};
EOF
	cat > main.c <<'EOF'
#include <stdio.h>
#include "provider.h"

__attribute__((noinline)) static int
tail(int seed)
{
	__asm__ volatile ("" : "+r" (seed));
	return DISABLED_CHECK_TICK_ENABLED();
}

__attribute__((noinline)) static int
ordinary(int seed)
{
	__asm__ volatile ("" : "+r" (seed));
	int enabled = DISABLED_CHECK_TICK_ENABLED();
	__asm__ volatile ("" : "+r" (enabled));
	return enabled;
}

int
main(void)
{
	int seeds[] = { 0, 1, 37, -7 };

	for (unsigned i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
		int a = tail(seeds[i]);
		int b = ordinary(seeds[i]);
		if (a != 0 || b != 0) {
			fprintf(stderr, "seed %d: tail %d, ordinary %d\n",
			    seeds[i], a, b);
			return (1);
		}
	}
	return (0);
}
EOF
}

check_disabled()
{
	compiler=$1
	language=$2
	dtrace=$(atf_config_get dtrace /usr/sbin/dtrace)
	bits=$(getconf LONG_BIT)
	atf_require_prog "${dtrace}"
	make_fixture
	atf_check "${dtrace}" "-${bits}" -h -s provider.d -o provider.h
	for opt in -O0 -O2 -O3 -Os; do
		atf_check "${compiler}" -x "${language}" "${opt}" -c main.c -o main.o
		# Reprocessing an object must retain the disabled value as well.
		for pass in 1 2; do
			atf_check "${dtrace}" "-${bits}" -G -s provider.d main.o -o provider.o
			atf_check "${compiler}" main.o provider.o -o control
			atf_check ./control
		done
	done
}

atf_test_case c_disabled
c_disabled_head()
{
	atf_set "descr" "Disabled generated predicates return zero in C calls and tails"
	atf_set "require.progs" "cc getconf"
}
c_disabled_body()
{
	check_disabled cc c
}

atf_test_case cxx_disabled
cxx_disabled_head()
{
	atf_set "descr" "Disabled generated predicates return zero in C++ calls and tails"
	atf_set "require.progs" "c++ getconf"
}
cxx_disabled_body()
{
	check_disabled c++ c++
}

atf_init_test_cases()
{
	atf_add_test_case c_disabled
	atf_add_test_case cxx_disabled
}
