/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <dev/filemon/filemon.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct trace {
	int device;
	int output;
	int release;
	pid_t holder;
};

static void
create_input(const char *name)
{
	int fd;

	ATF_REQUIRE((fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0600)) >= 0);
	ATF_REQUIRE_EQ(write(fd, "x", 1), 1);
	ATF_REQUIRE_EQ(close(fd), 0);
}

static void
wait_child(pid_t pid)
{
	pid_t result;
	int status;

	do {
		result = waitpid(pid, &status, 0);
	} while (result == -1 && errno == EINTR);
	ATF_REQUIRE_EQ(result, pid);
	ATF_REQUIRE(WIFEXITED(status));
	ATF_REQUIRE_EQ(WEXITSTATUS(status), 0);
}

static void
open_trace(struct trace *trace)
{
	char name[] = "filemon.XXXXXX";

	trace->holder = -1;
	trace->release = -1;
	ATF_REQUIRE((trace->output = mkstemp(name)) >= 0);
	ATF_REQUIRE((trace->device = open("/dev/filemon", O_RDWR | O_CLOEXEC))
	    >= 0);
	ATF_REQUIRE_EQ(ioctl(trace->device, FILEMON_SET_FD, &trace->output), 0);
}

static void
start_trace(struct trace *trace, bool hold)
{
	pid_t child, me;
	int fd, release[2];
	char token;

	create_input("input.txt");
	open_trace(trace);
	if (hold) {
		ATF_REQUIRE_EQ(pipe(release), 0);
		ATF_REQUIRE((trace->holder = fork()) >= 0);
		if (trace->holder == 0) {
			alarm(10);
			close(release[1]);
			close(trace->output);
			if (read(release[0], &token, 1) != 1)
				_exit(0);
			close(release[0]);
			_exit(close(trace->device) == 0 ? 0 : 1);
		}
		close(release[0]);
		trace->release = release[1];
	}
	ATF_REQUIRE((child = fork()) >= 0);
	if (child == 0) {
		alarm(10);
		close(trace->output);
		if (trace->release >= 0)
			close(trace->release);
		me = getpid();
		if (ioctl(trace->device, FILEMON_SET_PID, &me) < 0)
			_exit(1);
		if ((fd = open("input.txt", O_RDONLY)) < 0)
			_exit(2);
		close(fd);
		_exit(0);
	}
	wait_child(child);
}

static ssize_t
read_log(struct trace *trace, char *buffer, size_t size)
{
	ssize_t length;

	length = pread(trace->output, buffer, size - 1, 0);
	ATF_REQUIRE(length >= 0 && (size_t)length < size - 1);
	buffer[length] = '\0';
	return (length);
}

static unsigned
count(const char *text, const char *needle)
{
	unsigned result;

	for (result = 0; (text = strstr(text, needle)) != NULL; result++)
		text += strlen(needle);
	return (result);
}

static void
flush_trace(struct trace *trace)
{
	int result, saved_errno;

	result = ioctl(trace->device, FILEMON_FLUSH_IOCTL, 0);
	saved_errno = errno;
	ATF_CHECK_MSG(result == 0, "FILEMON_FLUSH_IOCTL: %s",
	    strerror(saved_errno));
}

ATF_TC(legacy_close);
ATF_TC_HEAD(legacy_close, tc)
{
	atf_tc_set_md_var(tc, "descr", "Legacy final close retains one footer");
}
ATF_TC_BODY(legacy_close, tc)
{
	struct trace trace;
	char text[8192];

	(void)tc;
	start_trace(&trace, false);
	ATF_REQUIRE_EQ(close(trace.device), 0);
	read_log(&trace, text, sizeof(text));
	ATF_CHECK_EQ(count(text, "# filemon version"), 1);
	ATF_CHECK_EQ(count(text, "# Bye bye"), 1);
	ATF_REQUIRE_EQ(close(trace.output), 0);
}

ATF_TC(flush_inherited_descriptor);
ATF_TC_HEAD(flush_inherited_descriptor, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Flush finalizes the log before inherited descriptors close");
}
ATF_TC_BODY(flush_inherited_descriptor, tc)
{
	struct trace trace;
	char before[8192], after[8192];
	ssize_t length, later;

	(void)tc;
	start_trace(&trace, true);
	flush_trace(&trace);
	length = read_log(&trace, before, sizeof(before));
	ATF_CHECK_EQ(count(before, "# Bye bye"), 1);
	ATF_REQUIRE_EQ(close(trace.device), 0);
	ATF_REQUIRE_EQ(lseek(trace.output, 0, SEEK_SET), 0);
	ATF_REQUIRE_EQ(write(trace.release, "x", 1), 1);
	close(trace.release);
	wait_child(trace.holder);
	later = read_log(&trace, after, sizeof(after));
	ATF_CHECK_EQ(later, length);
	ATF_CHECK(later == length &&
	    memcmp(before, after, (size_t)length) == 0);
	ATF_CHECK_EQ(count(after, "# filemon version"), 1);
	ATF_CHECK_EQ(count(after, "# Bye bye"), 1);
	ATF_REQUIRE_EQ(close(trace.output), 0);
}

ATF_TC(flush_idempotent);
ATF_TC_HEAD(flush_idempotent, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Repeated flush and close do not append");
}
ATF_TC_BODY(flush_idempotent, tc)
{
	struct trace trace;
	char before[8192], after[8192];
	ssize_t length, later;

	(void)tc;
	start_trace(&trace, false);
	flush_trace(&trace);
	length = read_log(&trace, before, sizeof(before));
	flush_trace(&trace);
	flush_trace(&trace);
	ATF_REQUIRE_EQ(close(trace.device), 0);
	later = read_log(&trace, after, sizeof(after));
	ATF_CHECK_EQ(later, length);
	ATF_CHECK(later == length &&
	    memcmp(before, after, (size_t)length) == 0);
	ATF_CHECK_EQ(count(after, "# Bye bye"), 1);
	ATF_REQUIRE_EQ(close(trace.output), 0);
}

ATF_TC(flush_detaches_process);
ATF_TC_HEAD(flush_detaches_process, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Flush detaches a still-running process");
}
ATF_TC_BODY(flush_detaches_process, tc)
{
	struct trace trace;
	char before[8192], after[8192], token;
	int ready[2], release[2], fd;
	pid_t child, me;
	ssize_t length, later;

	(void)tc;
	create_input("before.txt");
	create_input("after.txt");
	open_trace(&trace);
	ATF_REQUIRE_EQ(pipe(ready), 0);
	ATF_REQUIRE_EQ(pipe(release), 0);
	ATF_REQUIRE((child = fork()) >= 0);
	if (child == 0) {
		alarm(10);
		close(ready[0]);
		close(release[1]);
		close(trace.output);
		me = getpid();
		if (ioctl(trace.device, FILEMON_SET_PID, &me) < 0)
			_exit(1);
		if ((fd = open("before.txt", O_RDONLY)) < 0)
			_exit(2);
		close(fd);
		if (write(ready[1], "x", 1) != 1 ||
		    read(release[0], &token, 1) != 1)
			_exit(3);
		if ((fd = open("after.txt", O_RDONLY)) < 0)
			_exit(4);
		close(fd);
		_exit(0);
	}
	close(ready[1]);
	close(release[0]);
	ATF_REQUIRE_EQ(read(ready[0], &token, 1), 1);
	close(ready[0]);
	flush_trace(&trace);
	length = read_log(&trace, before, sizeof(before));
	ATF_CHECK(strstr(before, "before.txt") != NULL);
	ATF_CHECK_EQ(count(before, "# Bye bye"), 1);
	ATF_REQUIRE_EQ(close(trace.device), 0);
	ATF_REQUIRE_EQ(write(release[1], "x", 1), 1);
	close(release[1]);
	wait_child(child);
	later = read_log(&trace, after, sizeof(after));
	ATF_CHECK_EQ(later, length);
	ATF_CHECK(later == length &&
	    memcmp(before, after, (size_t)length) == 0);
	ATF_CHECK(strstr(after, "after.txt") == NULL);
	ATF_REQUIRE_EQ(close(trace.output), 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, legacy_close);
	ATF_TP_ADD_TC(tp, flush_inherited_descriptor);
	ATF_TP_ADD_TC(tp, flush_idempotent);
	ATF_TP_ADD_TC(tp, flush_detaches_process);
	return (atf_no_error());
}
