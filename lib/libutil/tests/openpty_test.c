/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jordan Hubbard
 */

#include <sys/types.h>

#include <errno.h>
#include <libutil.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

#define NTHREADS 4
#define ITERATIONS 1000

struct worker {
	pthread_barrier_t *barrier;
	unsigned int failures;
};

static void *
openpty_worker(void *arg)
{
	struct worker *worker = arg;
	char mastername[PATH_MAX], slavename[PATH_MAX], name[PATH_MAX];
	int master, slave;

	(void)pthread_barrier_wait(worker->barrier);
	for (unsigned int i = 0; i < ITERATIONS; i++) {
		if (openpty(&master, &slave, name, NULL, NULL) == -1) {
			worker->failures++;
			continue;
		}
		if (ptsname_r(master, mastername, sizeof(mastername)) != 0 ||
		    ttyname_r(slave, slavename, sizeof(slavename)) != 0 ||
		    strcmp(mastername, slavename) != 0 ||
		    strcmp(mastername, name) != 0)
			worker->failures++;
		(void)close(slave);
		(void)close(master);
	}
	return (NULL);
}

ATF_TC(concurrent);
ATF_TC_HEAD(concurrent, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Concurrent openpty calls return matching master/slave pairs");
	atf_tc_set_md_var(tc, "timeout", "30");
}

ATF_TC_BODY(concurrent, tc)
{
	pthread_barrier_t barrier;
	pthread_t threads[NTHREADS];
	struct worker workers[NTHREADS] = {0};
	unsigned int failures = 0;

	ATF_REQUIRE_EQ(0, pthread_barrier_init(&barrier, NULL, NTHREADS));
	for (unsigned int i = 0; i < NTHREADS; i++) {
		workers[i].barrier = &barrier;
		ATF_REQUIRE_EQ(0, pthread_create(&threads[i], NULL,
		    openpty_worker, &workers[i]));
	}
	for (unsigned int i = 0; i < NTHREADS; i++) {
		ATF_REQUIRE_EQ(0, pthread_join(threads[i], NULL));
		failures += workers[i].failures;
	}
	ATF_REQUIRE_EQ(0, pthread_barrier_destroy(&barrier));
	ATF_CHECK_EQ_MSG(0, failures, "%u of %u PTY pairs failed or mismatched",
	    failures, NTHREADS * ITERATIONS);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, concurrent);
	return (atf_no_error());
}
