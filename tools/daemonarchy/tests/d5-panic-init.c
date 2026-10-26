/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Static init for the D5 diagnostic VM: announce itself, then panic on
 * purpose after root mount so NetGDB can be entered from DDB with an
 * attached native NIC.  Build with: cc -static -O2 -o init d5-panic-init.c
 */
#include <sys/types.h>
#include <sys/sysctl.h>

#include <fcntl.h>
#include <unistd.h>

int
main(void)
{
	static const char msg[] =
	    "DAEMONARCHY-D5-INIT: deliberate panic after root mount\n";
	int fd, one = 1;

	fd = open("/dev/console", O_WRONLY);
	if (fd >= 0)
		(void)write(fd, msg, sizeof(msg) - 1);
	(void)sysctlbyname("debug.kdb.panic", NULL, NULL, &one, sizeof(one));
	for (;;)
		pause();
}
