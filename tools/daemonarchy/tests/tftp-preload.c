/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * TFTP preload checks around the verbatim libsa tftp_preload, tftp_read,
 * tftp_seek, tftp_close and cache release.  A simulated server feeds
 * blocks; a backward seek after a successful preload must not restart the
 * transfer, a platform allocator must be used and released, and a server
 * that sends more than its announced size must not overflow the cache.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifdef NDEBUG
#error "TFTP preload checks require assertions"
#endif

#define	DEBUG_PRINTF(level, args)	do { } while (0)
#define	bcopy(src, dst, len)	memmove((dst), (src), (len))
#define	twiddle(n)		do { } while (0)
#define	TFTP_TRIES		2
#define	EOFFSET			(ELAST + 8)
#ifndef ELAST
#define	ELAST			200
#endif
#define	TOPEN_MAX		8

struct iodesc { int unused; };
struct open_file { void *f_fsdata; };
struct tftphdr {
	short th_opcode;
	unsigned short th_block;
	char th_data[1];
} __attribute__((__packed__));

struct tftp_handle;
static struct tftp_handle *handles[TOPEN_MAX];
static int tftp_getnextblock(struct tftp_handle *);
static int tftp_makereq(struct tftp_handle *);
static void tftp_senderr(struct tftp_handle *, unsigned short, const char *);

/* PRODUCTION_FUNCTIONS */

/* Simulated server: file contents and behaviour. */
static unsigned char *file;
static size_t file_size, extra_bytes;
static int requests, blocks_sent, errors_sent;
static unsigned char pktbuf[70000];

static int
tftp_getnextblock(struct tftp_handle *h)
{
	size_t off = (size_t)h->currblock * h->tftp_blksize;
	size_t n = off >= file_size ? 0 : file_size - off;

	if (n > h->tftp_blksize)
		n = h->tftp_blksize;
	else if (extra_bytes != 0 && n < h->tftp_blksize) {
		/* A misbehaving server keeps sending past its tsize. */
		n = h->tftp_blksize;
		extra_bytes = extra_bytes > n ? extra_bytes - n : 0;
	}
	h->tftp_hdr = (struct tftphdr *)(void *)pktbuf;
	memset(pktbuf, 0x5a, sizeof(pktbuf));
	if (off < file_size)
		memcpy(h->tftp_hdr->th_data, file + off,
		    off + n <= file_size ? n : file_size - off);
	h->currblock++;
	h->validsize = (int)n;
	h->islastblock = n < h->tftp_blksize;
	blocks_sent++;
	return (0);
}

static int
tftp_makereq(struct tftp_handle *h)
{
	requests++;
	h->currblock = 0;
	h->islastblock = 0;
	h->validsize = 0;
	return (tftp_getnextblock(h));
}

static void
tftp_senderr(struct tftp_handle *h, unsigned short code, const char *msg)
{
	(void)h; (void)code; (void)msg;
	errors_sent++;
}

/* Platform allocator, like the EFI loader's page allocator. */
static int platform_allocs, platform_frees, platform_fail;
static void *
platform_alloc(size_t size)
{
	if (platform_fail)
		return (NULL);
	platform_allocs++;
	return (malloc(size));
}

static void
platform_free(void *p, size_t size)
{
	(void)size;
	platform_frees++;
	free(p);
}



static struct open_file of;
static struct tftp_handle *h;

static void
open_file(size_t size, unsigned blksize, bool announce)
{
	free(file);
	file_size = size;
	file = malloc(size ? size : 1);
	for (size_t i = 0; i < size; i++)
		file[i] = (unsigned char)(i * 131 + (i >> 9));
	h = calloc(1, sizeof(*h));
	h->tftp_blksize = blksize;
	h->tftp_tsize = announce ? size : 0;
	h->path = strdup("kernel");
	h->id = 0;
	handles[0] = h;
	of.f_fsdata = h;
	requests = blocks_sent = errors_sent = 0;
	tftp_makereq(h);	/* tftp_open's first request */
	requests = 0;
}

static void
read_at(off_t off, size_t len)
{
	unsigned char *buf = malloc(len);
	size_t resid = 12345;

	assert(tftp_seek(&of, off, SEEK_SET) == off);
	assert(tftp_read(&of, buf, len, &resid) == 0);
	size_t got = len - resid;
	size_t want = (size_t)off >= file_size ? 0 :
	    (file_size - off < len ? file_size - off : len);
	assert(got == want);
	assert(memcmp(buf, file + off, got) == 0);
	free(buf);
}

/* An ELF load reads the header, then sections, then seeks back. */
static void
elf_pattern(void)
{
	read_at(0, 4096);
	read_at(file_size / 2, 70000);
	read_at(4096, 1000);
	read_at(file_size - 5000, 5000);
	read_at(128, 64);
}

static void
platform_cache(void)
{
	tftp_cache_alloc = platform_alloc;
	tftp_cache_free = platform_free;
	open_file(3 * 1024 * 1024 + 77, 1428, true);
	assert(tftp_preload(&of) == 0);
	assert(platform_allocs == 1 && h->tftp_cache_platform);
	int sent = blocks_sent;
	elf_pattern();
	assert(requests == 0 && blocks_sent == sent);	/* no restart */
	read_at(file_size + 10, 100);	/* past the end reads nothing */
	assert(tftp_close(&of) == 0);
	assert(platform_frees == 1);
}

static void
heap_fallback(void)
{
	tftp_cache_alloc = platform_alloc;
	tftp_cache_free = platform_free;
	platform_fail = 1;
	open_file(200000, 512, true);
	assert(tftp_preload(&of) == 0);
	assert(platform_allocs == 0 && !h->tftp_cache_platform);
	elf_pattern();
	assert(requests == 0);
	assert(tftp_close(&of) == 0 && platform_frees == 0);
}

static void
preload_after_progress_restarts_once(void)
{
	tftp_cache_alloc = platform_alloc;
	tftp_cache_free = platform_free;
	open_file(100000, 1428, true);
	read_at(0, 9000);	/* the transfer has moved past block 1 */
	assert(tftp_preload(&of) == 0);
	assert(requests == 1);
	elf_pattern();
	assert(requests == 1);
	assert(tftp_close(&of) == 0);
}

static void
no_size_declines(void)
{
	tftp_cache_alloc = platform_alloc;
	tftp_cache_free = platform_free;
	open_file(50000, 1428, false);
	assert(tftp_preload(&of) == -1);
	assert(platform_allocs == 0 && h->tftp_cache == NULL);
	elf_pattern();	/* uncached: correct data, restarts on backward seeks */
	assert(requests > 0);
	assert(tftp_close(&of) == 0);
}

static void
oversize_server_refused(void)
{
	tftp_cache_alloc = platform_alloc;
	tftp_cache_free = platform_free;
	open_file(10000, 1428, true);
	extra_bytes = 5000;	/* sends beyond the announced tsize */
	assert(tftp_preload(&of) == EIO);
	assert(h->tftp_cache == NULL && platform_frees == platform_allocs);
	extra_bytes = 0;
	assert(tftp_close(&of) == 0);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } cases[] = {
		{ "platform-cache", platform_cache },
		{ "heap-fallback", heap_fallback },
		{ "restart-once", preload_after_progress_restarts_once },
		{ "no-size", no_size_declines },
		{ "oversize", oversize_server_refused },
	};
	if (argc != 2)
		return (2);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		if (strcmp(argv[1], cases[i].name) == 0) {
			cases[i].run();
			printf("%s: ok\n", argv[1]);
			free(file);
			return (0);
		}
	return (2);
}
