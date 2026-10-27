/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */

/*
 * Loader side of the reset-surviving boot trace.  The kernel (with
 * debug.boottrace=1) keeps one UEFI variable describing the step it was
 * about to run; this file shows, clears and saves it, and prints a
 * one-screen summary at startup when the previous traced boot never
 * reached init.
 */

#include <stand.h>
#include <stdarg.h>
#include <sys/param.h>
#include <sys/daemonarchy_boottrace.h>
#include <efi.h>
#include <efilib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>

#include "bootstrap.h"
#include "boottrace_blk.h"
#include "netconsole_report.h"

#define	BT_BUFSIZE	8192
#define	BT_REPORT	16384

static EFI_GUID bt_guid = DBT_GUID_INIT;
static uint8_t bt_buf[BT_BUFSIZE];
static size_t bt_len;
static char bt_report[BT_REPORT];
static struct dbt_step bt_stepv;		/* newest breadcrumb */
static bool bt_step_ok;
static struct dbt_step bt_ringv[DBT_RING_MAX + 1];	/* oldest first */
static unsigned bt_ring_n;
static char bt_nvdiag[256];
static size_t bt_report_len;

static int bt_startup(unsigned);
static int bt_cycle(void);

static void
bt_ring_name(char *name, size_t len, unsigned slot)
{
	snprintf(name, len, "%s%02u", DBT_STEP_VARNAME, slot);
}

/*
 * Breadcrumbs: the ring DBTStep00.. and the single DBTStep of older
 * kernels, sorted by sequence number into bt_ringv.
 */
static void
bt_load_ring(void)
{
	struct dbt_step st, tmp;
	char name[DBT_RING_NAMELEN];
	unsigned slot, i, j;
	size_t slen;

	bt_ring_n = 0;
	for (slot = 0; slot <= DBT_RING_MAX; slot++) {
		if (slot < DBT_RING_MAX)
			bt_ring_name(name, sizeof(name), slot);
		else
			strlcpy(name, DBT_STEP_VARNAME, sizeof(name));
		slen = sizeof(st);
		if (efi_getenv(&bt_guid, name, &st, &slen) != EFI_SUCCESS ||
		    slen < sizeof(st) || st.seq == 0)
			continue;
		st.text[DBT_STEP_TEXT - 1] = '\0';
		bt_ringv[bt_ring_n++] = st;
	}
	for (i = 1; i < bt_ring_n; i++)
		for (j = i; j > 0 && bt_ringv[j - 1].seq > bt_ringv[j].seq;
		    j--) {
			tmp = bt_ringv[j];
			bt_ringv[j] = bt_ringv[j - 1];
			bt_ringv[j - 1] = tmp;
		}
	bt_step_ok = bt_ring_n > 0;
	if (bt_step_ok)
		bt_stepv = bt_ringv[bt_ring_n - 1];
}

/* Delete the kernel's trace variables (not the loader's self-report). */
static EFI_STATUS
bt_delvars(void)
{
	char name[DBT_RING_NAMELEN];
	unsigned slot;

	for (slot = 0; slot < DBT_RING_MAX; slot++) {
		bt_ring_name(name, sizeof(name), slot);
		(void)efi_delenv(&bt_guid, name);
	}
	(void)efi_delenv(&bt_guid, DBT_STEP_VARNAME);
	bt_ring_n = 0;
	bt_step_ok = false;
	return (efi_delenv(&bt_guid, DBT_VARNAME));
}

static const char *
bt_kind(unsigned kind)
{
	static const char *names[] = { "?", "sysinit", "probe", "attach",
	    "hook", "event", "panic", "complete" };

	return (kind < nitems(names) ? names[kind] : "?");
}

static const char *
bt_status(unsigned status)
{
	switch (status) {
	case DBT_STATUS_RUNNING:
		return ("INCOMPLETE: the kernel never reached init "
		    "(hang or reset)");
	case DBT_STATUS_COMPLETE:
		return ("complete: the kernel reached init");
	case DBT_STATUS_PANIC:
		return ("PANIC: the kernel panicked");
	}
	return ("unknown");
}

/* Load and validate the record; returns 0 when a usable record exists. */
static int
bt_load(void)
{
	struct dbt_header *h;
	size_t need;
	EFI_STATUS status;

	bt_load_ring();
	bt_len = sizeof(bt_buf);
	status = efi_getenv(&bt_guid, DBT_VARNAME, bt_buf, &bt_len);
	if (status == EFI_NOT_FOUND)
		return (ENOENT);
	if (status != EFI_SUCCESS)
		return (EIO);
	h = (struct dbt_header *)bt_buf;
	if (bt_len < sizeof(*h) || memcmp(h->magic, DBT_MAGIC,
	    sizeof(h->magic)) != 0 || h->version != DBT_VERSION ||
	    h->header_size < sizeof(*h) ||
	    h->crumb_size < sizeof(struct dbt_crumb) ||
	    h->crumb_count > DBT_MAX_CRUMBS || h->msg_len > DBT_MAX_MSGBUF)
		return (EINVAL);
	need = h->header_size + (size_t)h->crumb_count * h->crumb_size +
	    h->msg_len;
	if (need > bt_len)
		return (EINVAL);
	return (0);
}

static struct dbt_crumb *
bt_crumb(unsigned i)
{
	struct dbt_header *h = (struct dbt_header *)bt_buf;

	return ((struct dbt_crumb *)(bt_buf + h->header_size +
	    (size_t)i * h->crumb_size));
}

static const char *
bt_msgs(void)
{
	struct dbt_header *h = (struct dbt_header *)bt_buf;

	return ((const char *)bt_buf + h->header_size +
	    (size_t)h->crumb_count * h->crumb_size);
}

static void
bt_append(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (bt_report_len >= sizeof(bt_report) - 1)
		return;
	va_start(ap, fmt);
	n = vsnprintf(bt_report + bt_report_len,
	    sizeof(bt_report) - bt_report_len, fmt, ap);
	va_end(ap);
	if (n > 0)
		bt_report_len = MIN(bt_report_len + n, sizeof(bt_report) - 1);
}

static void
bt_crumb_line(const char *prefix, struct dbt_crumb *c)
{
	char text[DBT_TEXT_SIZE + 1];

	memcpy(text, c->text, DBT_TEXT_SIZE);
	text[DBT_TEXT_SIZE] = '\0';
	bt_append("%s#%u %u.%03us cpu%u %s: %s", prefix, c->index,
	    c->ms / 1000, c->ms % 1000, c->cpu, bt_kind(c->kind), text);
	if (c->kind == DBT_KIND_SYSINIT)
		bt_append(" [sub 0x%x order 0x%x]", c->a, c->b);
	bt_append("\n");
}

static void
bt_step_line(const char *prefix, struct dbt_step *s)
{
	bt_append("%sb%u #%u %u.%03us cpu%u %s: %s", prefix, s->seq, s->index,
	    s->ms / 1000, s->ms % 1000, s->cpu, bt_kind(s->kind), s->text);
	if (s->kind == DBT_KIND_SYSINIT)
		bt_append(" [sub 0x%x order 0x%x]", s->a, s->b);
	bt_append("\n");
}

/* Append up to `lines' trailing message lines (all if 0). */
static void
bt_tail(const char *indent, unsigned lines)
{
	struct dbt_header *h = (struct dbt_header *)bt_buf;
	const char *m = bt_msgs(), *start, *end;
	unsigned seen;

	end = m + h->msg_len;
	while (end > m && (end[-1] == '\n' || end[-1] == '\0'))
		end--;
	start = end;
	for (seen = 0; start > m; start--) {
		if (start[-1] == '\n' && lines != 0 && ++seen >= lines)
			break;
	}
	while (start < end) {
		const char *nl = memchr(start, '\n', end - start);
		size_t n = (nl != NULL ? nl : end) - start;

		bt_append("%s%.*s\n", indent, (int)n, start);
		start += n + 1;
	}
}

static void
bt_format(bool summary)
{
	struct dbt_header *h = (struct dbt_header *)bt_buf;
	char kernel[DBT_KERNEL_SIZE + 1];
	unsigned i, first;

	bt_report_len = 0;
	bt_report[0] = '\0';
	memcpy(kernel, h->kernel, DBT_KERNEL_SIZE);
	kernel[DBT_KERNEL_SIZE] = '\0';
	bt_append("Daemonarchy boot trace (EFI variable %s, write #%u)\n",
	    DBT_VARNAME, h->seq);
	bt_append("Status: %s\n", bt_status(h->status));
	if (bt_step_ok && (h->crumb_count == 0 || bt_stepv.index >
	    bt_crumb(h->crumb_count - 1)->index) &&
	    h->status == DBT_STATUS_RUNNING) {
		bt_append("Last step: #%u %u.%03us cpu%u %s: %s", bt_stepv.index,
		    bt_stepv.ms / 1000, bt_stepv.ms % 1000, bt_stepv.cpu,
		    bt_kind(bt_stepv.kind), bt_stepv.text);
		if (bt_stepv.kind == DBT_KIND_SYSINIT)
			bt_append(" [sub 0x%x order 0x%x]", bt_stepv.a,
			    bt_stepv.b);
		bt_append("\n      (breadcrumb #%u; the full record below is "
		    "from step #%u)\n", bt_stepv.seq, h->steps);
	} else if (h->crumb_count > 0)
		bt_crumb_line("Last step: ", bt_crumb(h->crumb_count - 1));
	if ((h->flags & DBT_FLAG_BUDGET) != 0)
		bt_append("NOTE: the NVRAM write budget ran out at step #%u, "
		    "before the failure.\n      Narrow with "
		    "debug.boottrace.from=%u or a later from_sub.\n",
		    h->steps, h->steps > 8 ? h->steps - 8 : 1);
	if ((h->flags & DBT_FLAG_WRITE_ERROR) != 0)
		bt_append("NOTE: firmware rejected later writes; the trace "
		    "stops early.\n");
	if ((h->flags & DBT_FLAG_STRIDED) != 0)
		bt_append("NOTE: the NVRAM budget ran low; near the end only "
		    "every %uth step was persisted.\n      Zoom in with "
		    "debug.boottrace.from=<a step before the last one>.\n",
		    1u << DBT_FLAG_STRIDE_SHIFT(h->flags));
	if ((h->flags & DBT_FLAG_REFRESHED) != 0)
		bt_append("NOTE: the firmware freed NVRAM at runtime; the "
		    "budget was refreshed.\n");
	if ((h->flags & DBT_FLAG_TEST) != 0)
		bt_append("NOTE: a test hang/reset was armed for this boot.\n");
	if (!summary) {
		bt_append("Kernel: %s\n", kernel);
		bt_append("Steps seen: %u, last full write at %u.%03us, NVRAM "
		    "free at start %u KiB, %u KiB written, %u breadcrumbs "
		    "(budget ~%u), flags 0x%x\n", h->steps,
		    h->uptime_ms / 1000, h->uptime_ms % 1000, h->nv_left_kb,
		    h->nv_used_kb, h->step_writes, h->budget_writes, h->flags);
		if (h->resume_from == UINT32_MAX)
			bt_append("Persisted: coarse before debug.boottrace."
			    "from_sub, every step after it\n");
		else
			bt_append("Persisted: every step from #%u\n",
			    h->resume_from);
	}
	bt_append("Recent steps (oldest first):\n");
	first = summary && h->crumb_count > 5 ? h->crumb_count - 5 : 0;
	for (i = first; i < h->crumb_count; i++)
		bt_crumb_line("  ", bt_crumb(i));
	if (bt_ring_n > 0) {
		bt_append("NVRAM breadcrumb ring, %u newest (oldest first):\n",
		    bt_ring_n);
		first = summary && bt_ring_n > 5 ? bt_ring_n - 5 : 0;
		for (i = first; i < bt_ring_n; i++)
			bt_step_line("  ", &bt_ringv[i]);
	}
	bt_append("Last kernel messages:\n");
	bt_tail("  ", summary ? 8 : 0);
}

static int
bt_save(bool quiet)
{
	EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
	EFI_FILE_PROTOCOL *root;
	EFI_STATUS status, closed;
	unsigned slot;

	bt_format(false);
	/* Only the medium the loader came from is eligible. */
	status = BS->HandleProtocol(boot_img->DeviceHandle, &fs_guid,
	    (void **)&fs);
	if (status == EFI_SUCCESS)
		status = fs->OpenVolume(fs, &root);
	if (status == EFI_SUCCESS) {
		status = nc_report_write_named(root, "TRACE", bt_report,
		    bt_report_len, &slot);
		closed = root->Close(root);
		if (status == EFI_SUCCESS)
			status = closed;
	}
	if (status != EFI_SUCCESS && quiet)
		return (CMD_ERROR);
	if (status != EFI_SUCCESS) {
		printf("boottrace save FAILED (firmware status %lu); the boot "
		    "medium needs a writable FAT\nfilesystem with "
		    "DAEMDBG/PROBE.TAG.  The trace stays in NVRAM.\n",
		    DECODE_ERROR(status));
		return (CMD_ERROR);
	}
	printf("Saved and verified boot trace: DAEMDBG/TRACE%04u.TXT\n", slot);
	return (CMD_OK);
}

static void
bt_update_flags(uint32_t set)
{
	struct dbt_header *h = (struct dbt_header *)bt_buf;

	if ((h->flags & set) == set)
		return;
	h->flags |= set;
	(void)efi_setenv(&bt_guid, DBT_VARNAME, DBT_ATTRIBUTES, bt_buf,
	    bt_len);
}

static void
bt_print(void)
{
	pager_open();
	(void)pager_output(bt_report);
	pager_close();
}

static int
command_boottrace(int argc, char *argv[])
{
	const char *verb = argc > 1 ? argv[1] : "show";
	int error;

	if (argc > 3 || (argc == 3 && strcmp(verb, "startup") != 0))
		goto usage;
	if (strcmp(verb, "startup") == 0)
		return (bt_startup(argc == 3 ? strtoul(argv[2], NULL, 10) : 60));
	if (strcmp(verb, "cycle") == 0)
		return (bt_cycle());
	if (strcmp(verb, "clear") == 0) {
		EFI_STATUS status = bt_delvars();

		(void)efi_delenv(&bt_guid, DBT_LDR_VARNAME);
		if (status != EFI_SUCCESS && status != EFI_NOT_FOUND) {
			snprintf(command_errbuf, sizeof(command_errbuf),
			    "could not clear the trace (status %lu)",
			    DECODE_ERROR(status));
			return (CMD_ERROR);
		}
		printf("Boot trace cleared.\n");
		return (CMD_OK);
	}
	if (strcmp(verb, "show") != 0 && strcmp(verb, "save") != 0 &&
	    strcmp(verb, "summary") != 0 && strcmp(verb, "report") != 0)
		goto usage;
	error = bt_load();
	if (strcmp(verb, "report") == 0) {
		/* Unpaged and bracketed, for a remote controller to parse. */
		printf("BOOTTRACE-BEGIN\n");
		if (error == 0) {
			bt_format(false);
			printf("%s", bt_report);
		} else
			printf("No usable boot trace (%s).\n",
			    error == ENOENT ? "none stored" : "unreadable");
		printf("BOOTTRACE-END\n");
		return (CMD_OK);
	}
	if (error == ENOENT) {
		printf("No boot trace is stored.  Boot with the boot trace "
		    "entry (debug.boottrace=1) first.\n");
		return (CMD_OK);
	}
	if (error != 0) {
		snprintf(command_errbuf, sizeof(command_errbuf),
		    "the stored boot trace is unreadable (%d)", error);
		return (CMD_ERROR);
	}
	if (strcmp(verb, "save") == 0) {
		error = bt_save(false);
		if (error == CMD_OK)
			bt_update_flags(DBT_FLAG_SAVED);
		return (error);
	}
	bt_format(strcmp(verb, "summary") == 0);
	bt_print();
	return (CMD_OK);
usage:
	command_errmsg = "usage: boottrace [show|summary|report|save|clear|cycle|startup [SECONDS]]";
	return (CMD_ERROR);
}
COMMAND_SET(boottrace, "boottrace",
    "show, save or clear the previous boot's reset-surviving trace",
    command_boottrace);

/*
 * "boottrace startup [SECONDS]", run by loader.lua once loader.conf is read.
 * If the previous traced boot never reached init, put a one-screen summary
 * in front of the operator, save it to the boot medium when that medium is
 * prepared for it, and wait up to SECONDS for a key (10 s when this trace
 * was already shown; 0 never waits, for unattended loops).
 */
static int
bt_startup(unsigned wait)
{
	struct dbt_header *h = (struct dbt_header *)bt_buf;
	unsigned i;

	if (bt_load() != 0 || h->status == DBT_STATUS_COMPLETE)
		return (CMD_OK);
	if ((h->flags & DBT_FLAG_LOADER_SEEN) != 0 && wait > 10)
		wait = 10;
	bt_format(true);
	printf("\n================ PREVIOUS BOOT FAILED ================\n");
	printf("%s", bt_report);
	if ((h->flags & DBT_FLAG_SAVED) == 0) {
		if (bt_save(true) == CMD_OK) {
			printf("Saved to the boot medium as DAEMDBG/TRACExxxx.TXT\n");
			h->flags |= DBT_FLAG_SAVED;
		}
	}
	bt_update_flags(DBT_FLAG_LOADER_SEEN);
	printf("Full trace: 'boottrace show'.  Clear it: 'boottrace clear'.\n");
	if (wait == 0)
		return (CMD_OK);
	printf("Press any key to continue (continuing in %u s)...", wait);
	for (i = 0; i < wait * 10; i++) {
		if (ischar()) {
			(void)getchar();
			break;
		}
		delay(100000);
	}
	printf("\n");
	return (CMD_OK);
}

/*
 * Stick workflow ("boottrace cycle").  A FAT volume on the stick whose root
 * holds DATRACE.TAG carries:
 *
 *   STATE.TXT          next=N, traces=M, pending=ID (rewritten each cycle)
 *   QUEUE/NNNN.CNF     experiment N: loader settings, one per line
 *                      (name=value, unset name, read-conf /path, # comments)
 *   TRACES/NNNN.TXT    trace M: the experiment, its settings and the report
 *
 * A cycle files the previous boot's NVRAM trace under the pending
 * experiment, clears the variable and stages the next queue entry as
 * boottrace_q_* variables, which boottrace.lua applies before booting.
 *
 * STATE.TXT and the TRACES/ slots are preallocated and contiguous.  When the
 * volume carries a valid SLOTS.MAP (boottrace_blk.c), they are rewritten as
 * raw sectors through EFI_BLOCK_IO, flushed, and read back through the
 * disk's BlockIo: on the MSI Raider (AMI Aptio) writes through the firmware
 * FAT driver were acknowledged, read back from its cache, and never reached
 * the stick.  Without a map, files go through the firmware's FAT driver
 * (Simple File System protocol); libsa's dosfs is not used.
 *
 * Every step of the cycle is also recorded in NVRAM (DBTLoader).  When the
 * previous cycle wrote STATE.TXT but the stick does not hold it, the
 * writes did not persist: the loop stops with that reason instead of
 * running the same experiment again.
 */
#define	BT_TAG		"Daemonarchy boottrace stick v1"
#define	BT_FILEMAX	16384
#define	BT_MAXLINES	64

static char bt_file[BT_FILEMAX];
static char bt_out[BT_REPORT + BT_FILEMAX];

static void
bt_wide(CHAR16 *wide, const char *ascii)
{
	while ((*wide++ = (unsigned char)*ascii++) != 0)
		;
}

static EFI_STATUS
bt_open(EFI_FILE_PROTOCOL *root, EFI_FILE_PROTOCOL **file, const char *path,
    UINT64 mode, UINT64 attr)
{
	CHAR16 wide[128];
	EFI_FILE_PROTOCOL *opened = NULL;
	EFI_STATUS status;

	if (strlen(path) >= nitems(wide))
		return (EFI_INVALID_PARAMETER);
	bt_wide(wide, path);
	status = root->Open(root, &opened, wide, mode, attr);
	*file = status == EFI_SUCCESS ? opened : NULL;
	return (status == EFI_SUCCESS && opened == NULL ? EFI_DEVICE_ERROR :
	    status);
}

/* Read a whole small file into bt_file (NUL terminated). */
static EFI_STATUS
bt_read(EFI_FILE_PROTOCOL *root, const char *path, size_t *lenp)
{
	EFI_FILE_PROTOCOL *f;
	EFI_STATUS status;
	UINTN size;
	size_t len = 0;

	status = bt_open(root, &f, path, EFI_FILE_MODE_READ, 0);
	if (status != EFI_SUCCESS)
		return (status);
	for (;;) {
		size = sizeof(bt_file) - 1 - len;
		if (size == 0)
			break;
		status = f->Read(f, &size, bt_file + len);
		if (status != EFI_SUCCESS || size == 0)
			break;
		len += size;
	}
	(void)f->Close(f);
	bt_file[len] = '\0';
	if (lenp != NULL)
		*lenp = len;
	return (status);
}

/*
 * Replace a file's contents and verify by reading back.  The stick's files
 * are preallocated (stick.py, build-hwtrace-image.sh): they are rewritten
 * in place and padded with newlines to their existing size, so the loader
 * never allocates clusters.  Some firmware FAT drivers keep FAT updates in
 * a cache that is not written before the kernel starts; on OVMF a kernel
 * that then wrote DUMPS/ reused clusters the loader had just allocated.
 * A file that does not exist yet is still created, for older sticks.
 */
static EFI_STATUS
bt_write(EFI_FILE_PROTOCOL *root, const char *path, const char *data,
    size_t len)
{
	static char pad[512];
	EFI_FILE_PROTOCOL *f;
	EFI_STATUS status;
	UINT64 fsize = 0;
	UINTN size;
	size_t off;

	if (bt_open(root, &f, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
	    0) == EFI_SUCCESS) {
		if (f->SetPosition(f, UINT64_MAX) != EFI_SUCCESS ||
		    f->GetPosition(f, &fsize) != EFI_SUCCESS ||
		    f->SetPosition(f, 0) != EFI_SUCCESS)
			fsize = 0;
		if (fsize == 0)
			(void)f->Delete(f);	/* closes the handle */
	}
	if (fsize == 0) {
		status = bt_open(root, &f, path, EFI_FILE_MODE_READ |
		    EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
		if (status != EFI_SUCCESS)
			return (status);
	} else if (len > fsize)
		len = fsize;		/* keep the head of an oversized report */
	status = EFI_SUCCESS;
	for (off = 0; off < len; off += size) {
		size = len - off;
		status = f->Write(f, &size, (void *)(data + off));
		if (status != EFI_SUCCESS || size == 0)
			break;
	}
	if (pad[0] != '\n')
		memset(pad, '\n', sizeof(pad));
	for (off = len; status == EFI_SUCCESS && off < fsize; off += size) {
		size = MIN(sizeof(pad), fsize - off);
		status = f->Write(f, &size, pad);
		if (size == 0)
			status = EFI_DEVICE_ERROR;
	}
	if (status == EFI_SUCCESS)
		status = f->Flush(f);
	(void)f->Close(f);
	if (status != EFI_SUCCESS)
		return (status);
	if (len < sizeof(bt_file)) {
		size_t got;

		/* bt_write is never handed bt_file itself. */
		status = bt_read(root, path, &got);
		if (status == EFI_SUCCESS && (got < len ||
		    memcmp(bt_file, data, len) != 0))
			status = EFI_DEVICE_ERROR;
	}
	return (status);
}

static void
bt_connect_all(void)
{
	EFI_GUID blkio = BLOCK_IO_PROTOCOL;
	EFI_HANDLE *handles;
	UINTN size = 0, i;

	if (BS->LocateHandle(ByProtocol, &blkio, NULL, &size, NULL) !=
	    EFI_BUFFER_TOO_SMALL)
		return;
	handles = malloc(size);
	if (handles == NULL)
		return;
	if (BS->LocateHandle(ByProtocol, &blkio, NULL, &size, handles) ==
	    EFI_SUCCESS)
		for (i = 0; i < size / sizeof(EFI_HANDLE); i++)
			(void)BS->ConnectController(handles[i], NULL, NULL,
			    TRUE);
	free(handles);
}

/* Find the stick's data volume by its tag file. */
static EFI_FILE_PROTOCOL *
bt_volume(EFI_HANDLE *handlep)
{
	EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
	EFI_FILE_PROTOCOL *root;
	EFI_HANDLE *handles;
	UINTN size, i;
	int pass;

	for (pass = 0; pass < 2; pass++) {
		if (pass == 1)
			bt_connect_all();
		size = 0;
		if (BS->LocateHandle(ByProtocol, &fs_guid, NULL, &size,
		    NULL) != EFI_BUFFER_TOO_SMALL)
			continue;
		handles = malloc(size);
		if (handles == NULL)
			return (NULL);
		if (BS->LocateHandle(ByProtocol, &fs_guid, NULL, &size,
		    handles) != EFI_SUCCESS)
			size = 0;
		for (i = 0; i < size / sizeof(EFI_HANDLE); i++) {
			if (BS->HandleProtocol(handles[i], &fs_guid,
			    (void **)&fs) != EFI_SUCCESS ||
			    fs->OpenVolume(fs, &root) != EFI_SUCCESS)
				continue;
			if (bt_read(root, "\\DATRACE.TAG", NULL) ==
			    EFI_SUCCESS && strncmp(bt_file, BT_TAG,
			    sizeof(BT_TAG) - 1) == 0) {
				*handlep = handles[i];
				free(handles);
				return (root);
			}
			(void)root->Close(root);
		}
		free(handles);
	}
	return (NULL);
}

static unsigned
bt_state_get(const char *state, const char *key, char *val, size_t len)
{
	const char *p = state;
	size_t klen = strlen(key);

	val[0] = '\0';
	while (p != NULL && *p != '\0') {
		if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
			size_t n = strcspn(p + klen + 1, "\r\n");

			n = MIN(n, len - 1);
			memcpy(val, p + klen + 1, n);
			val[n] = '\0';
			break;
		}
		p = strchr(p, '\n');
		if (p != NULL)
			p++;
	}
	return (strtoul(val, NULL, 10));
}

static void
bt_out_append(size_t *len, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*len >= sizeof(bt_out) - 1)
		return;
	va_start(ap, fmt);
	n = vsnprintf(bt_out + *len, sizeof(bt_out) - *len, fmt, ap);
	va_end(ap);
	if (n > 0)
		*len = MIN(*len + n, sizeof(bt_out) - 1);
}

/*
 * Variable stores append on every write; most firmware reclaims deleted
 * copies only during boot services, when a write does not fit.  A heavy
 * traced boot leaves the store full of stale copies, which would starve the
 * next boot's budget, so rewrite a scratch variable until the store has
 * wrapped once (forcing a reclaim) and delete it.  Firmware that cannot
 * reclaim simply refuses a write and the loop stops early.
 */
static void
bt_reclaim(uint64_t free_estimate)
{
	UINT32 attr = EFI_VARIABLE_NON_VOLATILE |
	    EFI_VARIABLE_BOOTSERVICE_ACCESS;
	UINT64 maxstore, before, after, maxvar;
	CHAR16 name[40];
	uint32_t *buf;
	size_t size;
	unsigned i, n;

	EFI_STATUS st = EFI_SUCCESS;

	if (RS->Hdr.Revision < EFI_2_00_SYSTEM_TABLE_REVISION ||
	    RS->QueryVariableInfo(attr, &maxstore, &before, &maxvar) !=
	    EFI_SUCCESS) {
		snprintf(bt_nvdiag, sizeof(bt_nvdiag), "loader nvram: "
		    "QueryVariableInfo unavailable; no compaction\n");
		return;
	}
	if (maxvar < 2048) {
		snprintf(bt_nvdiag, sizeof(bt_nvdiag), "loader nvram: store "
		    "%ju KiB, %ju KiB free (boot time), max variable %ju; too "
		    "small to compact\n", (uintmax_t)maxstore / 1024,
		    (uintmax_t)before / 1024, (uintmax_t)maxvar);
		return;
	}
	/* Small scratch writes keep the garbage left after the reclaim small. */
	size = MIN(4096, maxvar - 512);
	/*
	 * Boot-time QueryVariableInfo ignores stale copies, so the kernel's
	 * runtime figure says how full the store really is.  Write just past
	 * it: one write triggers the reclaim, and little garbage follows.
	 */
	if (free_estimate == 0 || free_estimate > maxstore)
		free_estimate = maxstore;
	n = MIN(free_estimate / (size + 128) + 2, 64);
	buf = calloc(1, size);
	if (buf == NULL)
		return;
	bt_wide(name, "DaemonarchyBootTraceReclaim");
	for (i = 0; i < n; i++) {
		buf[0] = i;	/* identical data may be skipped by firmware */
		st = RS->SetVariable(name, &bt_guid, attr, size, buf);
		if (st != EFI_SUCCESS)
			break;
	}
	(void)RS->SetVariable(name, &bt_guid, 0, 0, NULL);
	free(buf);
	after = 0;
	(void)RS->QueryVariableInfo(attr, &maxstore, &after, &maxvar);
	snprintf(bt_nvdiag, sizeof(bt_nvdiag), "loader nvram: store %ju KiB, "
	    "max variable %ju; boot-time free %ju -> %ju KiB after %u of %u "
	    "scratch writes of %zu bytes (last status %lu)\n",
	    (uintmax_t)maxstore / 1024, (uintmax_t)maxvar,
	    (uintmax_t)before / 1024, (uintmax_t)after / 1024, i, n, size,
	    DECODE_ERROR(st));
	printf("Hardware trace: %s", bt_nvdiag);
}

static bool
bt_compact_enabled(void)
{
	const char *v = getenv("boottrace_compact");

	return (v != NULL && strcasecmp(v, "YES") == 0);
}

/* Record the boot-time store figures without changing anything. */
static void
bt_nvinfo(void)
{
	UINT32 attr = EFI_VARIABLE_NON_VOLATILE |
	    EFI_VARIABLE_BOOTSERVICE_ACCESS;
	UINT64 maxstore, left, maxvar;

	if (RS->Hdr.Revision < EFI_2_00_SYSTEM_TABLE_REVISION ||
	    RS->QueryVariableInfo(attr, &maxstore, &left, &maxvar) !=
	    EFI_SUCCESS)
		snprintf(bt_nvdiag, sizeof(bt_nvdiag), "loader nvram: "
		    "QueryVariableInfo unavailable\n");
	else
		snprintf(bt_nvdiag, sizeof(bt_nvdiag), "loader nvram: store "
		    "%ju KiB, %ju KiB free at boot time, max variable %ju; "
		    "compaction off\n", (uintmax_t)maxstore / 1024,
		    (uintmax_t)left / 1024, (uintmax_t)maxvar);
}


/*
 * Loader self-report (DBTLoader): the previous cycle's record, read at the
 * start of this one, and this cycle's, saved at every step.
 */
static struct dbt_loader bt_ldr, bt_ldr_prev;
static bool bt_ldr_prev_ok;
static struct bt_map bt_mapv;
static struct bt_blk bt_blkv;
static bool bt_raw;			/* SLOTS.MAP accepted: raw writes */
static char bt_mapwhy[96];
static char bt_checked[64];		/* mapped file compared with FAT */
static char bt_cmp[4096];
static bool bt_drop_fat;		/* test: FAT writes are lost */

static const char *
bt_ls_name(unsigned step)
{
	static const char *names[] = { "none", "found stick",
	    "read STATE.TXT", "filed trace", "chose experiment",
	    "wrote STATE.TXT", "verified STATE.TXT", "booting", "stopped" };

	return (step < nitems(names) ? names[step] : "?");
}

static const char *
bt_lm_name(unsigned method)
{
	return (method == DBT_LM_RAW ? "raw blocks (SLOTS.MAP)" :
	    method == DBT_LM_FAT ? "firmware FAT driver" : "-");
}

static const char *
bt_lv_name(unsigned verify)
{
	static const char *names[] = { "-", "ok", "MISMATCH", "read error",
	    "skipped (test drop)" };

	return (verify < nitems(names) ? names[verify] : "?");
}

static void
bt_ldr_load(void)
{
	size_t len = sizeof(bt_ldr_prev);

	bt_ldr_prev_ok = efi_getenv(&bt_guid, DBT_LDR_VARNAME, &bt_ldr_prev,
	    &len) == EFI_SUCCESS && len == sizeof(bt_ldr_prev) &&
	    memcmp(bt_ldr_prev.magic, DBT_LDR_MAGIC,
	    sizeof(bt_ldr_prev.magic)) == 0;
	if (bt_ldr_prev_ok)
		bt_ldr_prev.text[sizeof(bt_ldr_prev.text) - 1] = '\0';
	else
		memset(&bt_ldr_prev, 0, sizeof(bt_ldr_prev));
	memset(&bt_ldr, 0, sizeof(bt_ldr));
	memcpy(bt_ldr.magic, DBT_LDR_MAGIC, sizeof(bt_ldr.magic));
}

static void
bt_ldr_save(unsigned step, const char *fmt, ...)
{
	va_list ap;
	EFI_STATUS status;

	bt_ldr.step = step;
	if (fmt != NULL) {
		va_start(ap, fmt);
		vsnprintf(bt_ldr.text, sizeof(bt_ldr.text), fmt, ap);
		va_end(ap);
	}
	status = efi_setenv(&bt_guid, DBT_LDR_VARNAME, DBT_ATTRIBUTES,
	    &bt_ldr, sizeof(bt_ldr));
	if (status != EFI_SUCCESS)
		printf("Hardware trace: recording loader step '%s' in NVRAM "
		    "failed (status %lu)\n", bt_ls_name(step),
		    DECODE_ERROR(status));
}

/* One line per record, for the trace file and the screen. */
static void
bt_ldr_describe(size_t *len, const char *label, struct dbt_loader *l)
{
	bt_out_append(len, "loader %s: cycle %u (epoch %u) reached '%s'; "
	    "stick %s; flags 0x%x\n", label, l->cycle, l->epoch,
	    bt_ls_name(l->step), bt_lm_name(l->method), l->flags);
	if (l->step >= DBT_LS_WROTE)
		bt_out_append(len, "  wrote next=%u traces=%u pending=%04u, "
		    "status %u, read-back %s\n", l->next, l->traces,
		    l->pending, l->write_status, bt_lv_name(l->verify));
	bt_out_append(len, "  had read next=%u traces=%u pending=%04u; "
	    "repeat %u; %s\n", l->stick_next, l->stick_traces,
	    l->stick_pending, l->repeat, l->text);
}

static bool
bt_test_drop(const char *what)
{
	const char *v = getenv("boottrace_test_drop");

	return (v != NULL && (strcmp(v, what) == 0 || strcmp(v, "all") == 0));
}

/*
 * Accept SLOTS.MAP only when it describes this partition and its sectors
 * for STATE.TXT hold what the FAT driver returned for the file (nothing
 * has been written yet, so the two views must agree).
 */
static void
bt_raw_setup(EFI_HANDLE h, EFI_FILE_PROTOCOL *root, size_t slen,
    const char *state)
{
	EFI_GUID blkio = BLOCK_IO_PROTOCOL;
	EFI_DEVICE_PATH *dp, *node, *trim, *walk;
	EFI_BLOCK_IO_PROTOCOL *disk;
	EFI_HANDLE dh;
	const struct bt_map_ent *e;
	size_t len, n;

	bt_raw = false;
	bt_checked[0] = '\0';
	memset(&bt_blkv, 0, sizeof(bt_blkv));
	if (bt_read(root, "\\SLOTS.MAP", &len) != EFI_SUCCESS) {
		strlcpy(bt_mapwhy, "no SLOTS.MAP (older stick)",
		    sizeof(bt_mapwhy));
		return;
	}
	if (bt_map_parse(bt_file, len, &bt_mapv, bt_mapwhy,
	    sizeof(bt_mapwhy)) != 0)
		goto bad;
	if (BS->HandleProtocol(h, &blkio, (void **)&bt_blkv.part) !=
	    EFI_SUCCESS) {
		strlcpy(bt_mapwhy, "no BlockIo on the volume",
		    sizeof(bt_mapwhy));
		goto bad;
	}
	dp = efi_lookup_devpath(h);
	node = dp != NULL ? efi_devpath_last_node(dp) : NULL;
	if (node != NULL && DevicePathType(node) == MEDIA_DEVICE_PATH &&
	    DevicePathSubType(node) == MEDIA_HARDDRIVE_DP) {
		bt_blkv.disk_lba0 = ((HARDDRIVE_DEVICE_PATH *)node)->
		    PartitionStart;
		trim = efi_devpath_trim(dp);
		walk = trim;
		if (trim != NULL && BS->LocateDevicePath(&blkio, &walk, &dh) ==
		    EFI_SUCCESS && IsDevicePathEnd(walk) &&
		    BS->HandleProtocol(dh, &blkio, (void **)&disk) ==
		    EFI_SUCCESS && disk->Media != NULL &&
		    !disk->Media->LogicalPartition)
			bt_blkv.disk = disk;
		free(trim);
	}
	if (bt_blk_check(&bt_blkv, &bt_mapv, bt_mapwhy, sizeof(bt_mapwhy)) !=
	    EFI_SUCCESS)
		goto bad;
	e = bt_map_find(&bt_mapv, "STATE.TXT");
	n = e != NULL ? MIN(MIN(slen, e->size), sizeof(bt_cmp)) : 0;
	if (n == 0 || bt_blk_read(&bt_blkv, e, bt_cmp, n) != EFI_SUCCESS ||
	    memcmp(bt_cmp, state, n) != 0) {
		strlcpy(bt_mapwhy, "STATE.TXT is not where SLOTS.MAP says",
		    sizeof(bt_mapwhy));
		goto bad;
	}
	bt_blkv.drop = bt_test_drop("raw");
	bt_raw = true;
	bt_ldr.flags |= DBT_LF_MAP;
	snprintf(bt_mapwhy, sizeof(bt_mapwhy), "%u mapped files, read-back "
	    "via the %s", bt_mapv.count, bt_blkv.disk != NULL ? "whole disk" :
	    "partition");
	return;
bad:
	bt_ldr.flags |= DBT_LF_MAPBAD;
}

/* Write a stick file: raw through the map when possible, else FAT. */
static EFI_STATUS
bt_stick_write(EFI_FILE_PROTOCOL *root, const char *path, const char *data,
    size_t len)
{
	const struct bt_map_ent *e;
	EFI_STATUS status;
	size_t n;

	e = bt_raw ? bt_map_find(&bt_mapv, path) : NULL;
	/* First write of a slot this cycle: it must be where the map says. */
	if (e != NULL && strcmp(bt_checked, path) != 0) {
		strlcpy(bt_checked, path, sizeof(bt_checked));
		if (bt_read(root, path, &n) == EFI_SUCCESS) {
			n = MIN(MIN(n, e->size), sizeof(bt_cmp));
			if (n == 0 || bt_blk_read(&bt_blkv, e, bt_cmp, n) !=
			    EFI_SUCCESS || memcmp(bt_cmp, bt_file, n) != 0) {
				printf("Hardware trace: %s is not where "
				    "SLOTS.MAP says; using the FAT driver.\n",
				    path);
				bt_raw = false;
				bt_ldr.flags = (bt_ldr.flags & ~DBT_LF_MAP) |
				    DBT_LF_MAPBAD;
				e = NULL;
			}
		}
	}
	if (e != NULL) {
		bt_ldr.method = DBT_LM_RAW;
		if (bt_blkv.drop)
			bt_ldr.flags |= DBT_LF_TEST_DROP;
		status = bt_blk_write(&bt_blkv, e, data, len);
		bt_ldr.verify = bt_blkv.verify;
	} else {
		bt_ldr.method = DBT_LM_FAT;
		if (bt_drop_fat) {
			/* Test: acknowledged, served from cache, never written. */
			bt_ldr.flags |= DBT_LF_TEST_DROP;
			bt_ldr.verify = DBT_LV_SKIPPED;
			status = EFI_SUCCESS;
		} else {
			status = bt_write(root, path, data, len);
			/* FAT read-back may come from the driver's cache. */
			bt_ldr.verify = status == EFI_SUCCESS ? DBT_LV_OK :
			    DBT_LV_ERROR;
		}
	}
	bt_ldr.write_status = (uint32_t)DECODE_ERROR(status);
	return (status);
}

static void
bt_stop(const char *why)
{
	printf("\n=========== HARDWARE TRACE: STOPPED ===========\n%s\n"
	    "Take the stick to the controller; it reads TRACES/ and writes "
	    "a new QUEUE/.\n", why);
	bt_ldr.flags &= ~DBT_LF_RUNNING;
	bt_ldr_save(DBT_LS_STOPPED, "%.63s", why);
	setenv("boottrace_cycle", "stop", 1);
	setenv("autoboot_delay", "NO", 1);
}

static int
bt_cycle(void)
{
	struct dbt_header *h = (struct dbt_header *)bt_buf;
	EFI_FILE_PROTOCOL *root;
	EFI_HANDLE vol = NULL;
	EFI_STATUS status;
	char state[512], val[80], pending[80], path[64], name[32];
	unsigned next, traces, n, epoch, pending_n, max_repeat;
	size_t len, slen;
	int error;
	bool complete = false, reclaim, same;
	uint64_t nv_free;
	char *line, *end;
	const char *v;

	unsetenv("boottrace_cycle");
	bt_ldr_load();
	bt_drop_fat = bt_test_drop("fat");
	v = getenv("boottrace_max_repeat");
	max_repeat = v != NULL ? strtoul(v, NULL, 10) : 2;
	if (max_repeat == 0)
		max_repeat = 2;
	root = bt_volume(&vol);
	if (root == NULL) {
		printf("Hardware trace: no stick data volume (DATRACE.TAG) "
		    "found.\n");
		setenv("boottrace_cycle", "novolume", 1);
		return (CMD_OK);
	}
	state[0] = '\0';
	slen = 0;
	if (bt_read(root, "\\STATE.TXT", &slen) == EFI_SUCCESS)
		strlcpy(state, bt_file, sizeof(state));
	else
		slen = 0;
	next = bt_state_get(state, "next", val, sizeof(val));
	if (next == 0)
		next = 1;
	traces = bt_state_get(state, "traces", val, sizeof(val));
	epoch = bt_state_get(state, "epoch", val, sizeof(val));
	pending_n = bt_state_get(state, "pending", pending, sizeof(pending));
	bt_raw_setup(vol, root, MIN(slen, sizeof(state) - 1), state);

	same = bt_ldr_prev_ok && bt_ldr_prev.epoch == epoch;
	bt_ldr.epoch = epoch;
	bt_ldr.cycle = same ? bt_ldr_prev.cycle + 1 : 1;
	bt_ldr.stick_next = next;
	bt_ldr.stick_traces = traces;
	bt_ldr.stick_pending = pending_n;
	bt_ldr.method = bt_raw ? DBT_LM_RAW : DBT_LM_FAT;
	printf("Hardware trace: cycle %u; stick writes via %s (%s)\n",
	    bt_ldr.cycle, bt_lm_name(bt_ldr.method), bt_mapwhy);
	if (bt_drop_fat || bt_blkv.drop)
		printf("Hardware trace: TEST: boottrace_test_drop=%s; those "
		    "writes are acknowledged but never made\n",
		    getenv("boottrace_test_drop"));
	bt_ldr_save(DBT_LS_STATE, "stick next=%u traces=%u pending=%s", next,
	    traces, pending[0] != '\0' ? pending : "-");

	/*
	 * 0: the previous cycle staged an experiment and recorded what it
	 * wrote to STATE.TXT.  If the stick does not hold that, its writes
	 * never reached the media; looping would repeat the same experiment.
	 * The verdict sticks until the stick is queued again (new epoch) or
	 * "boottrace clear" deletes DBTLoader.
	 */
	if (same && (((bt_ldr_prev.flags & DBT_LF_RUNNING) != 0 &&
	    (bt_ldr_prev.next != next || bt_ldr_prev.traces != traces ||
	    bt_ldr_prev.pending != pending_n)) ||
	    (bt_ldr_prev.flags & DBT_LF_NOPERSIST) != 0)) {
		(void)root->Close(root);
		bt_ldr.flags |= DBT_LF_NOPERSIST;
		bt_ldr.cycle = bt_ldr_prev.cycle;
		bt_ldr.next = bt_ldr_prev.next;
		bt_ldr.traces = bt_ldr_prev.traces;
		bt_ldr.pending = bt_ldr_prev.pending;
		bt_ldr.method = bt_ldr_prev.method;
		bt_ldr.verify = bt_ldr_prev.verify;
		bt_ldr.write_status = bt_ldr_prev.write_status;
		len = 0;
		bt_out_append(&len, "STICK WRITES ARE NOT PERSISTING.\n"
		    "Cycle %u wrote STATE.TXT next=%u traces=%u pending=%04u "
		    "via %s (read-back %s),\nbut the stick still reads next=%u "
		    "traces=%u pending=%s.\n", bt_ldr_prev.cycle,
		    bt_ldr_prev.next, bt_ldr_prev.traces, bt_ldr_prev.pending,
		    bt_lm_name(bt_ldr_prev.method),
		    bt_lv_name(bt_ldr_prev.verify), next, traces,
		    pending[0] != '\0' ? pending : "-");
		if (bt_load() == 0) {
			bt_format(true);
			bt_out_append(&len, "The kernel trace of experiment %04u "
			    "stays in NVRAM ('boottrace show'; Windows: "
			    "read-boottrace.ps1):\n%s", bt_ldr_prev.pending,
			    bt_report);
		}
		bt_out_append(&len, "Queue the stick again (stick.py queue) or "
		    "run 'boottrace clear' to start over.");
		printf("\n");
		bt_stop(bt_out);
		return (CMD_OK);
	}

	/* 1-3: file the previous boot's trace, then clear the variable. */
	bt_nvdiag[0] = '\0';
	error = bt_load();
	/* A boot that ran the store low leaves stale copies behind. */
	/*
	 * Compaction is opt-in (boottrace_compact="YES"): on OVMF it can
	 * reclaim early and then refill the store with scratch copies.
	 * Without it, each trace still records the store figures so the
	 * next boot's "NVRAM free at start" shows whether firmware reclaims.
	 */
	reclaim = error == 0 && (h->nv_left_kb < 192 ||
	    (h->flags & (DBT_FLAG_BUDGET | DBT_FLAG_WRITE_ERROR)) != 0);
	if (!bt_compact_enabled()) {
		bt_nvinfo();
		reclaim = false;
	}
	/*
	 * Runtime free space when the boot ended: what the full record had
	 * counted, less the breadcrumbs written after it (~160 bytes each).
	 */
	nv_free = 1;
	if (error == 0) {
		uint64_t spent = (uint64_t)h->nv_used_kb * 1024;

		if (bt_step_ok && bt_stepv.seq > h->step_writes)
			spent += (uint64_t)(bt_stepv.seq - h->step_writes) * 160;
		if ((uint64_t)h->nv_left_kb * 1024 > spent)
			nv_free = (uint64_t)h->nv_left_kb * 1024 - spent;
	}
	if (error != ENOENT || pending[0] != '\0') {
		len = 0;
		bt_out_append(&len, "Daemonarchy hardware trace %04u\n",
		    traces + 1);
		bt_out_append(&len, "experiment: %s\n",
		    pending[0] != '\0' ? pending : "(none: booted outside "
		    "the queue)");
		if (pending[0] != '\0') {
			n = strtoul(pending, NULL, 10);
			snprintf(path, sizeof(path), "\\QUEUE\\%04u.CNF", n);
			if (bt_read(root, path, NULL) == EFI_SUCCESS) {
				bt_out_append(&len, "settings (%s):\n", path);
				for (line = bt_file; *line != '\0'; line = end) {
					end = line + strcspn(line, "\n");
					bt_out_append(&len, "  %.*s\n",
					    (int)(end - line), line);
					if (*end == '\n')
						end++;
				}
			}
		}
		if (bt_ldr_prev_ok)
			bt_ldr_describe(&len, "previous", &bt_ldr_prev);
		bt_out_append(&len, "loader now: cycle %u, stick via %s (%s)\n",
		    bt_ldr.cycle, bt_lm_name(bt_ldr.method), bt_mapwhy);
		if (error == 0) {
			bt_out_append(&len, "nvram: write #%u, status %u, "
			    "%u steps\n\n", h->seq, h->status, h->steps);
			bt_format(false);
			bt_out_append(&len, "%s", bt_report);
			complete = h->status == DBT_STATUS_COMPLETE;
		} else
			bt_out_append(&len, "nvram: %s\n", error == ENOENT ?
			    "no trace (the kernel never reached EFI runtime "
			    "services, or tracing was off)" :
			    "unreadable trace");
		traces++;
		snprintf(path, sizeof(path), "\\TRACES\\%04u.TXT", traces);
		status = bt_stick_write(root, path, bt_out, len);
		if (status == EFI_SUCCESS && !reclaim && bt_nvdiag[0] != '\0') {
			bt_out_append(&len, "\n%s", bt_nvdiag);
			status = bt_stick_write(root, path, bt_out, len);
		}
		if (status == EFI_SUCCESS && reclaim) {
			/* Only now is it safe to drop the variables and compact. */
			(void)bt_delvars();
			bt_reclaim(nv_free);
			bt_out_append(&len, "\n%s", bt_nvdiag);
			status = bt_stick_write(root, path, bt_out, len);
			reclaim = false;
		}
		if (status != EFI_SUCCESS) {
			printf("Hardware trace: writing %s FAILED (status %lu, "
			    "read-back %s); the trace stays in NVRAM.\n", path,
			    DECODE_ERROR(status), bt_lv_name(bt_ldr.verify));
			(void)root->Close(root);
			snprintf(bt_out, sizeof(bt_out), "Stick write failed: "
			    "%s via %s, status %lu, read-back %s.", path + 1,
			    bt_lm_name(bt_ldr.method), DECODE_ERROR(status),
			    bt_lv_name(bt_ldr.verify));
			bt_stop(bt_out);
			return (CMD_OK);
		}
		if (error == 0) {
			bt_format(true);
			printf("\n%s", bt_report);
		}
		printf("Hardware trace: saved %s via %s (read-back %s)\n", path,
		    bt_lm_name(bt_ldr.method), bt_lv_name(bt_ldr.verify));
		bt_ldr_save(DBT_LS_FILED, "filed %s", path + 1);
		(void)bt_delvars();
	}

	if (reclaim)
		bt_reclaim(nv_free);

	/* 4: stage the next experiment, unless a boot completed. */
	snprintf(path, sizeof(path), "\\QUEUE\\%04u.CNF", next);
	if (complete) {
		snprintf(bt_out, sizeof(bt_out), "Experiment %s reached init "
		    "(boot complete).", pending);
		pending[0] = '\0';
	} else if (bt_read(root, path, &len) != EFI_SUCCESS) {
		snprintf(bt_out, sizeof(bt_out), "The experiment queue is "
		    "empty (no %s).", path + 1);
		pending[0] = '\0';
	} else {
		snprintf(pending, sizeof(pending), "%04u", next);
		n = 0;
		for (line = bt_file; *line != '\0' && n < BT_MAXLINES;
		    line = end) {
			end = line + strcspn(line, "\r\n");
			while (*line == ' ' || *line == '\t')
				line++;
			if (end > line && *line != '#') {
				snprintf(name, sizeof(name), "boottrace_q_%u",
				    ++n);
				snprintf(val, sizeof(val), "%.*s",
				    (int)(end - line), line);
				setenv(name, val, 1);
			}
			end += strspn(end, "\r\n");
		}
		snprintf(val, sizeof(val), "%u", n);
		setenv("boottrace_q_count", val, 1);
		setenv("boottrace_q_id", pending, 1);
		next++;
	}
	pending_n = strtoul(pending, NULL, 10);
	/* Belt and braces: the same experiment staged again and again. */
	if (pending_n != 0 && same && bt_ldr_prev.pending == pending_n)
		bt_ldr.repeat = bt_ldr_prev.repeat + 1;
	bt_ldr.next = next;
	bt_ldr.traces = traces;
	bt_ldr.pending = pending_n;
	bt_ldr_save(DBT_LS_CHOSEN, "chose %s", pending[0] != '\0' ? pending :
	    "none");
	if (bt_ldr.repeat >= max_repeat) {
		(void)root->Close(root);
		bt_ldr.flags |= DBT_LF_REPEAT;
		snprintf(bt_out, sizeof(bt_out), "Experiment %s would run for "
		    "the %u%s time in a row: stick writes are not persisting "
		    "(boottrace_max_repeat=%u).", pending, bt_ldr.repeat + 1,
		    bt_ldr.repeat + 1 == 2 ? "nd" : bt_ldr.repeat + 1 == 3 ?
		    "rd" : "th", max_repeat);
		bt_stop(bt_out);
		return (CMD_OK);
	}
	if (epoch != 0)
		len = snprintf(state, sizeof(state), "next=%u\ntraces=%u\n"
		    "pending=%s\nepoch=%u\n", next, traces, pending, epoch);
	else
		len = snprintf(state, sizeof(state), "next=%u\ntraces=%u\n"
		    "pending=%s\n", next, traces, pending);
	status = bt_stick_write(root, "\\STATE.TXT", state, len);
	(void)root->Close(root);
	bt_ldr_save(bt_ldr.verify == DBT_LV_OK && bt_ldr.method == DBT_LM_RAW ?
	    DBT_LS_VERIFIED : DBT_LS_WROTE, "STATE.TXT status %lu",
	    DECODE_ERROR(status));
	if (status != EFI_SUCCESS) {
		snprintf(bt_out, sizeof(bt_out), "Writing STATE.TXT on the "
		    "stick failed via %s (status %lu, read-back %s).",
		    bt_lm_name(bt_ldr.method), DECODE_ERROR(status),
		    bt_lv_name(bt_ldr.verify));
		bt_stop(bt_out);
		return (CMD_OK);
	}
	if (pending[0] == '\0') {
		bt_stop(bt_out);
		return (CMD_OK);
	}
	printf("Hardware trace: running experiment %s (%s); STATE.TXT via %s, "
	    "read-back %s\n", pending, path + 1, bt_lm_name(bt_ldr.method),
	    bt_lv_name(bt_ldr.verify));
	bt_ldr.flags |= DBT_LF_RUNNING;
	bt_ldr_save(DBT_LS_BOOTING, "booting %s", pending);
	setenv("boottrace_cycle", "boot", 1);
	return (CMD_OK);
}
