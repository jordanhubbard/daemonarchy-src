/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Mountroot console output checks around the verbatim production globals,
 * dnc_putc() and dnc_flush(): output written before the first poll keeps
 * the stage marker, overflow drops the oldest bytes with a count, and a
 * failed send keeps the remaining output for the next poll.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#error "Console output checks require assertions"
#endif

#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#define	__unused	__attribute__((__unused__))
#define	DEBUGNET_DATA	3
struct debugnet_pcb { int unused; };
struct consdev { int unused; };
typedef void *if_t;
typedef void *eventhandler_tag;

static char sent[65536];
static size_t nsent;
static int sends, fail_sends;

static int
debugnet_send(struct debugnet_pcb *pcb, uint32_t type, const void *data,
    uint32_t len, const void *aux)
{
	(void)pcb; (void)aux;
	assert(type == DEBUGNET_DATA && len > 0 && len <= 1024);
	sends++;
	if (fail_sends > 0) {
		fail_sends--;
		return (60);	/* ETIMEDOUT */
	}
	assert(nsent + len <= sizeof(sent));
	memcpy(sent + nsent, data, len);
	nsent += len;
	return (0);
}

/* PRODUCTION_FUNCTIONS */

static struct debugnet_pcb pcb;

static void
put(const char *text)
{
	for (; *text; text++)
		dnc_putc(NULL, *text);
}

static void
marker_survives_prompt(void)
{
	static const char marker[] =
	    "netdebug-stage: kernel-rootconf boot_id=00 if=vtnet0\n";
	char help[6000];

	dnc_pcb = &pcb;
	put(marker);
	memset(help, 'h', sizeof(help) - 1);
	help[sizeof(help) - 1] = 0;
	put(help);
	put("\nmountroot> ");
	dnc_flush();
	assert(dnc_output_lost == 0 && dnc_output_len == 0);
	assert(strncmp(sent, "netdebug-stage: kernel-rootconf", 31) == 0);
	assert(nsent == strlen(marker) + 5999 + 12);
	assert(memcmp(sent + nsent - 11, "mountroot> ", 11) == 0);
	assert(sends == (int)((nsent + 1023) / 1024));
}

static void
overflow_drops_oldest_with_count(void)
{
	dnc_pcb = &pcb;
	for (int i = 0; i < DNC_OUTSIZE + 300; i++)
		dnc_putc(NULL, 'a' + i % 26);
	assert(dnc_output_len == DNC_OUTSIZE && dnc_output_lost == 300);
	dnc_flush();
	assert(nsent == DNC_OUTSIZE && sent[0] == 'a' + 300 % 26);
}

static void
failed_send_keeps_the_rest(void)
{
	dnc_pcb = &pcb;
	for (int i = 0; i < 3000; i++)
		dnc_putc(NULL, 'x');
	fail_sends = 1;
	dnc_flush();
	assert(sends == 1 && dnc_output_lost == 1024 && dnc_output_len == 3000 - 1024);
	dnc_flush();
	assert(nsent == 3000 - 1024 && dnc_output_len == 0);
}

static void
inactive_or_busy_drops_nothing_sent(void)
{
	dnc_pcb = NULL;
	put("ignored");
	dnc_flush();
	assert(dnc_output_len == 0 && sends == 0);
	dnc_pcb = &pcb;
	dnc_busy = true;
	put("reentrant");
	dnc_flush();
	assert(dnc_output_len == 0 && sends == 0);
	dnc_busy = false;
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } cases[] = {
		{ "marker-survives", marker_survives_prompt },
		{ "overflow", overflow_drops_oldest_with_count },
		{ "send-failure", failed_send_keeps_the_rest },
		{ "inactive", inactive_or_busy_drops_nothing_sent },
	};
	if (argc != 2)
		return (2);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		if (strcmp(argv[1], cases[i].name) == 0) {
			cases[i].run();
			printf("%s: ok\n", argv[1]);
			return (0);
		}
	return (2);
}
