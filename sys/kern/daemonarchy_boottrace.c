/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */

/*
 * Reset-surviving boot trace.  See sys/sys/daemonarchy_boottrace.h for the
 * record layout.  Everything here is inert unless the loader sets
 * debug.boottrace=1.
 *
 * Design constraints:
 *  - The step being entered must be on NVRAM before it runs, because a hang
 *    with interrupts disabled never gives a later callout a chance to run.
 *  - UEFI variable stores append a new copy on every SetVariable and most
 *    firmware cannot reclaim the space at runtime; a laptop store may have
 *    only tens of KiB free.  Each boot works from a byte budget derived
 *    from QueryVariableInfo minus a small reserve.  Every persisted step
 *    is a 64-byte breadcrumb in a ring of small variables (DBTStep00..,
 *    debug.boottrace.ring), overwritten in place, so the newest steps
 *    before a hang are on NVRAM and the live footprint stays a few KiB.
 *    The full record with breadcrumbs and the message-buffer tail is
 *    written only at the first opportunity, at checkpoints, at panic,
 *    completion and the budget stop.  Two full writes stay reserved for
 *    the last two.
 *  - The budget is re-read from QueryVariableInfo every DBT_REQUERY
 *    breadcrumbs; firmware that reclaims at runtime refills it.  When it
 *    runs low, breadcrumbs are persisted with a growing stride (every 2nd,
 *    4th, ... step) instead of stopping, so the last breadcrumb stays close
 *    to a hang; debug.boottrace.from then zooms in.
 *  - Before debug.boottrace.from_sub (default SI_SUB_CONFIGURE) only the
 *    first SYSINIT of each subsystem is persisted; from there every SYSINIT,
 *    attach, config hook and each device's first probe is.  When the
 *    budget runs low, per-device probes are dropped first.
 *  - Writing stops at boot completion (just before init is executed).
 */

#include "opt_ddb.h"
#if defined(__amd64__) || defined(__aarch64__)
#include "opt_efirt.h"
#endif

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/daemonarchy_boottrace.h>
#include <sys/kdb.h>
#include <sys/kernel.h>
#include <sys/linker.h>
#include <sys/lock.h>
#include <sys/msgbuf.h>
#include <sys/mutex.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#ifdef EFIRT
#include <sys/efi.h>
#endif

#include <machine/stdarg.h>
#if defined(__amd64__)
#include <machine/clock.h>
#include <machine/cpufunc.h>
#include <machine/fpu.h>
#include <machine/segments.h>
#endif

#ifdef DDB
#include <ddb/ddb.h>
#include <ddb/db_sym.h>
#endif

#define	DBT_RING	32
#define	DBT_BUFSIZE	(sizeof(struct dbt_header) + \
    DBT_MAX_CRUMBS * sizeof(struct dbt_crumb) + DBT_MAX_MSGBUF)
/* Firmware variable header, UTF-16 name and alignment, conservatively. */
#define	DBT_WRITE_OVERHEAD	128
#define	DBT_REQUERY		32	/* breadcrumbs between budget re-reads */
#define	DBT_STRIDE_MAX		6	/* widest stride: every 64th step */

int dbt_active;

static struct mtx dbt_mtx;
static struct dbt_crumb dbt_ring[DBT_RING];
static uint32_t dbt_steps;		/* index of the newest crumb */
static uint32_t dbt_persisted;		/* index of the newest crumb on NVRAM */
static uint32_t dbt_seq;
static uint32_t dbt_flags;
static uint32_t dbt_status = DBT_STATUS_RUNNING;
static uint32_t dbt_resume_from = UINT32_MAX;	/* first fine-grained step */
static uint32_t dbt_budget;		/* full-size writes the budget allows */
static uint64_t dbt_bytes_left;		/* NVRAM bytes this boot may use */
static uint64_t dbt_bytes_used;		/* NVRAM bytes written this boot */
static uint32_t dbt_last_sub;
static uint64_t dbt_bytes_start;	/* budget when tracing started */
static uint32_t dbt_step_cost = 192;	/* estimate until measured */
static uint32_t dbt_step_writes;
static uint32_t dbt_since_full;
static bool dbt_thinned;
static bool dbt_fine_seen;
static device_t dbt_last_probe_dev;
static uint32_t dbt_nv_left_kb;
static uint64_t dbt_reserve;		/* NVRAM bytes never used */
static bool dbt_query_tracks;		/* QueryVariableInfo follows writes */
static u_int dbt_stride_shift;		/* current breadcrumb stride (log2) */
static u_int dbt_stride_count;
static int dbt_errors;
static int dbt_last_error;
static bool dbt_rt_ready;		/* EFI runtime services usable */
static bool dbt_rt_checked;
static bool dbt_stopped;		/* NVRAM writing finished or failed */
static bool dbt_in_write;
static uint64_t dbt_t0;

/* Tunables. */
static int dbt_msgbuf_len = 1024;
static int dbt_crumbs = 6;
static int dbt_probes = 1;
static int dbt_reserve_kb = -1;		/* auto: max variable / 4, 8..16 KiB */
static u_int dbt_from_sub = SI_SUB_CONFIGURE;
static int dbt_full_every = 64;
static int dbt_max_writes;
static int dbt_from = -1;
static int dbt_test_reset;
static char dbt_test_hang[64];
static int dbt_ring_k = DBT_RING_DEFAULT;
static int dbt_stride_on = 1;

static uint8_t dbt_buf[DBT_BUFSIZE] __aligned(8);
static uint16_t dbt_name[sizeof(DBT_VARNAME)];
static uint16_t dbt_step_name[sizeof(DBT_STEP_VARNAME)];
static uint16_t dbt_ring_name[DBT_RING_MAX][DBT_RING_NAMELEN];
static struct dbt_step dbt_stepbuf;
#ifdef EFIRT
static efi_guid_t dbt_guid = DBT_GUID_INIT;
#endif

SYSCTL_NODE(_debug, OID_AUTO, boottrace, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "Daemonarchy reset-surviving boot trace (loader tunable debug.boottrace)");
SYSCTL_UINT(_debug_boottrace, OID_AUTO, steps, CTLFLAG_RD, &dbt_steps, 0,
    "Steps recorded during boot");
SYSCTL_UINT(_debug_boottrace, OID_AUTO, writes, CTLFLAG_RD, &dbt_seq, 0,
    "NVRAM writes this boot");
SYSCTL_UINT(_debug_boottrace, OID_AUTO, budget, CTLFLAG_RD, &dbt_budget, 0,
    "NVRAM writes the budget allowed this boot");
SYSCTL_UINT(_debug_boottrace, OID_AUTO, status, CTLFLAG_RD, &dbt_status, 0,
    "1 running, 2 complete, 3 panic");
SYSCTL_UINT(_debug_boottrace, OID_AUTO, flags, CTLFLAG_RD, &dbt_flags, 0,
    "Record flags");
SYSCTL_UINT(_debug_boottrace, OID_AUTO, resume_from, CTLFLAG_RD,
    &dbt_resume_from, 0, "First step persisted fine-grained (debug.boottrace.from)");
SYSCTL_UINT(_debug_boottrace, OID_AUTO, nv_left_kb, CTLFLAG_RD,
    &dbt_nv_left_kb, 0, "NVRAM space reported when tracing started");
SYSCTL_INT(_debug_boottrace, OID_AUTO, last_error, CTLFLAG_RD,
    &dbt_last_error, 0, "Last SetVariable error");

static uint32_t
dbt_ms(void)
{
#if defined(__amd64__)
	uint64_t freq;

	freq = atomic_load_acq_64(&tsc_freq);
	if (freq == 0)
		return (0);
	return ((uint32_t)((rdtsc() - dbt_t0) / (freq / 1000)));
#else
	return ((uint32_t)(sbinuptime() / (SBT_1S / 1000)));
#endif
}

static bool
dbt_can_block(void)
{
	struct thread *td;

	td = curthread;
	if (SCHEDULER_STOPPED() || kdb_active || td->td_critnest != 0 ||
	    (td->td_pflags & TDP_EFIRT) != 0)
		return (false);
	return (true);
}

static void
dbt_msgbuf_tail(struct dbt_header *h, char *out, int want)
{
	struct msgbuf *mbp;
	u_int seq, avail;
	int got, n, i, j;
	bool bol;

	h->msg_len = 0;
	mbp = msgbufp;
	if (mbp == NULL || want <= 0)
		return;
	/* Copy oldest-to-newest and strip "<pri>" line prefixes. */
	msgbuf_peekbytes(mbp, NULL, 0, &seq);
	avail = MSGBUF_SEQSUB(mbp, mbp->msg_wseq, seq);
	if (avail > (u_int)want)
		seq = MSGBUF_SEQADD(mbp, seq, avail - want);
	got = 0;
	while (got < want) {
		n = msgbuf_peekbytes(mbp, out + got, want - got, &seq);
		if (n <= 0)
			break;
		got += n;
	}
	bol = false;
	for (i = 0, j = 0; i < got; i++) {
		if (bol && out[i] == '<') {
			int k;

			for (k = i + 1; k < got && out[k] >= '0' &&
			    out[k] <= '9'; k++)
				;
			if (k < got && k > i + 1 && out[k] == '>') {
				i = k;
				continue;
			}
		}
		bol = (out[i] == '\n');
		if (out[i] != '\0')
			out[j++] = out[i];
	}
	h->msg_len = j;
}

static size_t
dbt_full_cost(void)
{
	return (sizeof(struct dbt_header) + dbt_crumbs *
	    sizeof(struct dbt_crumb) + dbt_msgbuf_len + DBT_WRITE_OVERHEAD);
}

static size_t
dbt_build(int msglen)
{
	struct dbt_header *h;
	struct dbt_crumb *c;
	uint32_t first;
	int count, i;

	h = (struct dbt_header *)dbt_buf;
	memset(h, 0, sizeof(*h));
	memcpy(h->magic, DBT_MAGIC, sizeof(h->magic));
	h->version = DBT_VERSION;
	h->header_size = sizeof(*h);
	h->crumb_size = sizeof(struct dbt_crumb);
	count = imin(dbt_crumbs, (int)dbt_steps);
	h->crumb_count = count;
	h->seq = dbt_seq;
	h->status = dbt_status;
	h->flags = dbt_flags;
	h->steps = dbt_steps;
	h->resume_from = dbt_resume_from;
	h->uptime_ms = dbt_ms();
	h->nv_left_kb = dbt_nv_left_kb;
	h->budget_writes = dbt_budget;
	h->nv_used_kb = dbt_bytes_used / 1024;
	h->step_writes = dbt_step_writes;
	strlcpy(h->kernel, version, sizeof(h->kernel));
	for (i = 0; h->kernel[i] != '\0'; i++)
		if (h->kernel[i] == '\n')
			h->kernel[i] = '\0';

	c = (struct dbt_crumb *)(h + 1);
	first = dbt_steps - count + 1;
	for (i = 0; i < count; i++)
		c[i] = dbt_ring[(first + i) % DBT_RING];
	dbt_msgbuf_tail(h, (char *)(c + count), msglen);
	h->record_size = sizeof(*h) + count * sizeof(*c) + h->msg_len;
	return (h->record_size);
}

#ifdef EFIRT
static int
dbt_query(uint64_t *left, uint64_t *maxvar)
{
	uint64_t maxstore;

	return (efi_var_query(DBT_ATTRIBUTES, &maxstore, left, maxvar));
}

static void dbt_write(bool force);

static void
dbt_rt_start(void)
{
	uint64_t left, maxvar, reserve;
	size_t i;
	int error;

	dbt_rt_checked = true;
	for (i = 0; i < nitems(dbt_name); i++)
		dbt_name[i] = DBT_VARNAME[i];
	for (i = 0; i < nitems(dbt_step_name); i++)
		dbt_step_name[i] = DBT_STEP_VARNAME[i];
	/*
	 * Breadcrumbs left by an earlier boot (or by an older kernel's single
	 * DBTStep) would be sorted in with this boot's; delete every slot.
	 */
	(void)efi_var_set(dbt_step_name, &dbt_guid, 0, 0, NULL);
	for (i = 0; i < DBT_RING_MAX; i++) {
		size_t n = sizeof(DBT_STEP_VARNAME) - 1;

		memcpy(dbt_ring_name[i], dbt_step_name, n * sizeof(uint16_t));
		dbt_ring_name[i][n] = '0' + i / 10;
		dbt_ring_name[i][n + 1] = '0' + i % 10;
		dbt_ring_name[i][n + 2] = 0;
		(void)efi_var_set(dbt_ring_name[i], &dbt_guid, 0, 0, NULL);
	}
	if (dbt_from >= 0) {
		dbt_resume_from = dbt_from;
		dbt_flags |= DBT_FLAG_RESUMED;
	}

	maxvar = 0;
	error = dbt_query(&left, &maxvar);
	if (error != 0)
		left = 64 * 1024;	/* unknown: assume a small store */
	if (dbt_reserve_kb >= 0)
		reserve = (uint64_t)dbt_reserve_kb * 1024;
	else
		reserve = MIN(MAX(maxvar / 4, 8 * 1024), 16 * 1024);
	dbt_nv_left_kb = left / 1024;
	dbt_reserve = reserve;
	dbt_bytes_left = left > reserve ? left - reserve : 0;
	if (dbt_max_writes > 0)
		dbt_bytes_left = MIN(dbt_bytes_left,
		    (uint64_t)dbt_max_writes * dbt_step_cost);
	/* A cramped store gets smaller full records. */
	if (dbt_bytes_left < 6 * dbt_full_cost()) {
		dbt_msgbuf_len = imin(dbt_msgbuf_len, 512);
		dbt_crumbs = imin(dbt_crumbs, 4);
	}
	dbt_bytes_start = dbt_bytes_left;
	dbt_budget = dbt_bytes_left / dbt_step_cost;
	printf("boottrace: EFI variables %s/%s00..%02d-%s; %u KiB NVRAM "
	    "free (query %d, max variable %ju), reserve %ju KiB, budget %ju "
	    "KiB (~%u breadcrumbs, ring of %d)\n", DBT_VARNAME,
	    DBT_STEP_VARNAME, dbt_ring_k - 1, DBT_GUID_STRING, dbt_nv_left_kb,
	    error, (uintmax_t)maxvar, (uintmax_t)reserve / 1024,
	    (uintmax_t)dbt_bytes_left / 1024, dbt_budget, dbt_ring_k);
	if (dbt_resume_from != UINT32_MAX)
		printf("boottrace: persisting every step from #%u\n",
		    dbt_resume_from);
	else
		printf("boottrace: persisting every step from subsystem "
		    "0x%x (debug.boottrace.from_sub)\n", dbt_from_sub);
	dbt_rt_ready = true;
	if (dbt_bytes_left < 2 * dbt_full_cost()) {
		/* Leave one full record so the loader can say why. */
		printf("boottrace: not enough NVRAM for tracing\n");
		dbt_flags |= DBT_FLAG_BUDGET;
		dbt_bytes_left = dbt_full_cost();
		dbt_write(true);
		dbt_stopped = true;
	}
}

static int
dbt_set(uint16_t *name, size_t size, void *data)
{
	int error;

	error = efi_var_set(name, &dbt_guid, DBT_ATTRIBUTES, size, data);
	if (error != 0) {
		/* An old copy with different attributes blocks the update. */
		(void)efi_var_set(name, &dbt_guid, 0, 0, NULL);
		error = efi_var_set(name, &dbt_guid, DBT_ATTRIBUTES, size,
		    data);
	}
	if (error != 0) {
		dbt_last_error = error;
		if (++dbt_errors >= 3) {
			dbt_flags |= DBT_FLAG_WRITE_ERROR;
			dbt_stopped = true;
			printf("boottrace: SetVariable failed (%d); "
			    "NVRAM tracing stopped\n", error);
		}
	}
	return (error);
}

/* Full record: breadcrumbs plus the message-buffer tail. */
static void
dbt_write(bool force)
{
	size_t size, full;

	if (!dbt_rt_ready || dbt_stopped || dbt_in_write)
		return;
	full = dbt_full_cost();
	/* Two full writes stay reserved for completion, panic or the stop. */
	if (!force && dbt_bytes_left < 3 * full)
		return;
	dbt_in_write = true;
	dbt_seq++;
	size = dbt_build(dbt_msgbuf_len);
	if (size + DBT_WRITE_OVERHEAD > dbt_bytes_left) {
		dbt_seq--;
		dbt_in_write = false;
		dbt_stopped = true;
		return;
	}
	(void)dbt_set(dbt_name, size, dbt_buf);
	dbt_in_write = false;
	dbt_bytes_left -= size + DBT_WRITE_OVERHEAD;
	dbt_bytes_used += size + DBT_WRITE_OVERHEAD;
	dbt_since_full = 0;
	dbt_persisted = dbt_steps;
	if ((dbt_flags & DBT_FLAG_BUDGET) != 0)
		dbt_stopped = true;
}

/*
 * Re-read the free space.  Adopt a smaller figure always; a larger one
 * (firmware reclaimed at runtime, or breadcrumbs cost less than measured)
 * only when QueryVariableInfo was seen to follow writes, so firmware that
 * reports a constant can never lead the trace into the reserve.
 */
static void
dbt_requery(void)
{
	uint64_t left, maxvar, avail;

	if (dbt_max_writes > 0 || dbt_query(&left, &maxvar) != 0)
		return;
	avail = left > dbt_reserve ? left - dbt_reserve : 0;
	if (avail < dbt_bytes_left)
		dbt_bytes_left = avail;
	else if (dbt_query_tracks && avail > dbt_bytes_left) {
		if (avail > dbt_bytes_left + 4 * dbt_full_cost() &&
		    (dbt_flags & DBT_FLAG_REFRESHED) == 0) {
			dbt_flags |= DBT_FLAG_REFRESHED;
			printf("boottrace: firmware freed NVRAM at runtime; "
			    "budget refreshed to %ju KiB\n",
			    (uintmax_t)avail / 1024);
		}
		dbt_bytes_left = avail;
	}
}

/* Breadcrumb stride for the remaining budget: 1, 2, 4, ... (log2). */
static u_int
dbt_stride(void)
{
	uint64_t q;
	u_int shift;

	if (!dbt_stride_on)
		return (0);
	q = dbt_bytes_start / 4;
	for (shift = 0; shift < DBT_STRIDE_MAX && dbt_bytes_left < q; shift++)
		q /= 2;
	return (shift);
}

/* One 64-byte breadcrumb for the step about to run. */
static void
dbt_write_step(struct dbt_crumb *c, bool must)
{
	uint64_t before, after, maxvar;
	u_int shift, slot;
	bool measure;

	if (!dbt_rt_ready || dbt_stopped || dbt_in_write)
		return;
	if (dbt_bytes_left < dbt_step_cost + 2 * dbt_full_cost())
		dbt_requery();
	if (dbt_bytes_left < dbt_step_cost + 2 * dbt_full_cost()) {
		/* Budget gone: file the stop in the full record and quit. */
		dbt_flags |= DBT_FLAG_BUDGET;
		printf("boottrace: NVRAM budget used at step %u; narrow with "
		    "debug.boottrace.from or from_sub\n", dbt_steps);
		dbt_write(true);
		return;
	}
	if (!dbt_thinned && dbt_bytes_left < dbt_bytes_start * 2 / 5) {
		dbt_thinned = true;
		dbt_flags |= DBT_FLAG_THINNED;
	}
	shift = dbt_stride();
	if (shift != dbt_stride_shift) {
		dbt_stride_shift = shift;
		dbt_stride_count = 0;
		if (shift > 0) {
			dbt_flags |= DBT_FLAG_STRIDED;
			if (shift > DBT_FLAG_STRIDE_SHIFT(dbt_flags))
				dbt_flags = (dbt_flags & ~0xff0000u) |
				    (shift << 16);
			printf("boottrace: NVRAM budget low at step %u; "
			    "persisting every %u%s step\n", dbt_steps, 1u << shift,
			    shift == 1 ? "nd" : "th");
		}
	}
	if (!must && shift > 0 && c->kind != DBT_KIND_EVENT &&
	    ++dbt_stride_count < (1u << shift))
		return;
	dbt_stride_count = 0;
	memset(&dbt_stepbuf, 0, sizeof(dbt_stepbuf));
	dbt_stepbuf.seq = ++dbt_step_writes;
	slot = (dbt_stepbuf.seq - 1) % dbt_ring_k;
	dbt_stepbuf.index = c->index;
	dbt_stepbuf.a = c->a;
	dbt_stepbuf.b = c->b;
	dbt_stepbuf.ms = c->ms;
	dbt_stepbuf.kind = c->kind;
	dbt_stepbuf.cpu = c->cpu;
	strlcpy(dbt_stepbuf.text, c->text, sizeof(dbt_stepbuf.text));
	dbt_in_write = true;
	/* Measure what one breadcrumb really costs on this firmware. */
	measure = dbt_step_writes == 2 && dbt_query(&before, &maxvar) == 0;
	(void)dbt_set(dbt_ring_name[slot], sizeof(dbt_stepbuf), &dbt_stepbuf);
	if (measure && dbt_query(&after, &maxvar) == 0 && before > after &&
	    before - after >= sizeof(dbt_stepbuf) &&
	    before - after <= 1024) {
		dbt_step_cost = before - after;
		dbt_query_tracks = true;
	}
	dbt_in_write = false;
	dbt_bytes_left -= MIN(dbt_bytes_left, dbt_step_cost);
	dbt_bytes_used += dbt_step_cost;
	dbt_since_full++;
	dbt_persisted = dbt_steps;
	if (dbt_step_writes % DBT_REQUERY == 0)
		dbt_requery();
	/* Periodic checkpoint while the budget is comfortable. */
	if (dbt_full_every > 0 && dbt_since_full >= (u_int)dbt_full_every &&
	    dbt_bytes_left > dbt_bytes_start / 2)
		dbt_write(false);
}
#else
static void
dbt_write(bool force __unused)
{
}

static void
dbt_write_step(struct dbt_crumb *c __unused, bool must __unused)
{
}
#endif

static void
dbt_test_trap(const char *what)
{
	if (dbt_test_hang[0] == '\0' || strcmp(what, dbt_test_hang) != 0)
		return;
	if (dbt_test_reset == 2)
		panic("boottrace: TEST panic at %s", what);
	printf("boottrace: TEST %s at %s\n",
	    dbt_test_reset ? "triple fault" : "hang with interrupts disabled",
	    what);
	DELAY(100000);
	disable_intr();
#if defined(__amd64__)
	if (dbt_test_reset) {
		struct region_descriptor rd = { 0, 0 };

		lidt(&rd);
		__asm __volatile("int3");
	}
#endif
	for (;;)
		cpu_spinwait();
}

/*
 * Record one step and, when policy allows, persist it before it runs.
 */
static void
dbt_step(int kind, uint32_t a, uint32_t b, bool fine, bool coarse,
    const char *test, const char *fmt, ...)
{
	struct dbt_crumb *c;
	va_list ap;
	bool block, persist;

	block = dbt_can_block();
	if (block)
		mtx_lock(&dbt_mtx);
	dbt_steps++;
	persist = block && (dbt_steps >= dbt_resume_from ||
	    dbt_last_sub >= dbt_from_sub ? fine : coarse);
	c = &dbt_ring[dbt_steps % DBT_RING];
	memset(c, 0, sizeof(*c));
	c->index = dbt_steps;
	c->ms = dbt_ms();
	c->kind = kind;
	c->cpu = PCPU_GET(cpuid);
	c->a = a;
	c->b = b;
	va_start(ap, fmt);
	vsnprintf(c->text, sizeof(c->text), fmt, ap);
	va_end(ap);
#ifdef EFIRT
	if (block && !dbt_rt_checked && efi_rt_ok() == 0)
		dbt_rt_start();
#endif
	if (test != NULL && dbt_test_hang[0] != '\0' &&
	    strcmp(test, dbt_test_hang) == 0) {
		dbt_flags |= DBT_FLAG_TEST;
		if (block) {
			dbt_write_step(c, true);
			dbt_write(false);
		}
	} else if (block && dbt_rt_ready && dbt_seq == 0)
		dbt_write(true);	/* first opportunity: flush RAM crumbs */
	else if (persist) {
		/* Checkpoint the messages when the fine-grained region starts. */
		if (!dbt_fine_seen && dbt_last_sub >= dbt_from_sub) {
			dbt_fine_seen = true;
			dbt_write(false);
		}
		dbt_write_step(c, false);
	}
	if (block)
		mtx_unlock(&dbt_mtx);
	if (test != NULL)
		dbt_test_trap(test);
}

static const char *
dbt_symbol(void *addr, char *buf, size_t len)
{
#ifdef DDB
	c_db_sym_t sym;
	db_expr_t offset;
	const char *name;

	sym = db_search_symbol((vm_offset_t)addr, DB_STGY_PROC, &offset);
	if (sym != C_DB_SYM_NULL) {
		db_symbol_values(sym, &name, NULL);
		if (name != NULL && offset == 0)
			return (name);
	}
#else
	long offset;

	if (linker_search_symbol_name_flags(addr, buf, len, &offset,
	    M_NOWAIT) == 0 && offset == 0)
		return (buf);
#endif
	snprintf(buf, len, "%p", addr);
	return (buf);
}

void
dbt_start(void)
{
	int on;

	on = 0;
	TUNABLE_INT_FETCH("debug.boottrace", &on);
	if (on == 0)
		return;
	TUNABLE_INT_FETCH("debug.boottrace.msgbuf", &dbt_msgbuf_len);
	TUNABLE_INT_FETCH("debug.boottrace.crumbs", &dbt_crumbs);
	TUNABLE_INT_FETCH("debug.boottrace.probes", &dbt_probes);
	TUNABLE_INT_FETCH("debug.boottrace.reserve_kb", &dbt_reserve_kb);
	TUNABLE_INT_FETCH("debug.boottrace.max_writes", &dbt_max_writes);
	TUNABLE_INT_FETCH("debug.boottrace.from", &dbt_from);
	getenv_uint("debug.boottrace.from_sub", &dbt_from_sub);
	TUNABLE_INT_FETCH("debug.boottrace.full_every", &dbt_full_every);
	TUNABLE_INT_FETCH("debug.boottrace.test_reset", &dbt_test_reset);
	TUNABLE_STR_FETCH("debug.boottrace.test_hang", dbt_test_hang,
	    sizeof(dbt_test_hang));
	TUNABLE_INT_FETCH("debug.boottrace.ring", &dbt_ring_k);
	TUNABLE_INT_FETCH("debug.boottrace.stride", &dbt_stride_on);
	dbt_ring_k = imax(1, imin(dbt_ring_k, DBT_RING_MAX));
	dbt_msgbuf_len = imax(0, imin(dbt_msgbuf_len, DBT_MAX_MSGBUF));
	dbt_crumbs = imax(1, imin(dbt_crumbs, DBT_MAX_CRUMBS));
	if (dbt_reserve_kb >= 0)
		dbt_reserve_kb = imax(4, dbt_reserve_kb);
#if defined(__amd64__)
	dbt_t0 = rdtsc();
#endif
	mtx_init(&dbt_mtx, "boottrace", NULL, MTX_DEF | MTX_NOWITNESS);
	dbt_active = 1;
	printf("boottrace: enabled%s%s\n",
	    dbt_test_hang[0] != '\0' ? "; TEST trap armed at " : "",
	    dbt_test_hang);
}

void
dbt_sysinit(struct sysinit *sip)
{
	char buf[64];
	const char *func;

	bool first;

	first = sip->subsystem != dbt_last_sub;
	dbt_last_sub = sip->subsystem;
	func = dbt_symbol((void *)sip->func, buf, sizeof(buf));
	dbt_step(DBT_KIND_SYSINIT, sip->subsystem, sip->order, true, first,
	    func, "%s", func);
}

void
dbt_device(int kind, device_t dev, driver_t *drv)
{
	char loc[64];
	struct sbuf sb;
	device_t parent;
	const char *pname, *what;
	bool fine, coarse, fixed;

	fixed = (kind & 0x100) != 0;
	kind &= 0xff;
	parent = device_get_parent(dev);
	pname = parent != NULL ? device_get_nameunit(parent) : "-";
	loc[0] = '\0';
	if (parent != NULL && (kind == DBT_KIND_ATTACH || fixed) &&
	    dbt_can_block() && device_get_name(parent) != NULL &&
	    (strcmp(device_get_name(parent), "pci") == 0 ||
	    strcmp(device_get_name(parent), "acpi") == 0 ||
	    strcmp(device_get_name(parent), "isa") == 0)) {
		sbuf_new(&sb, loc, sizeof(loc), SBUF_FIXEDLEN);
		(void)bus_child_location(dev, &sb);
		(void)sbuf_finish(&sb);
		if (sbuf_len(&sb) < 0)
			loc[0] = '\0';
	}
	if (kind == DBT_KIND_PROBE) {
		/* Each device's first probe attempt; every hinted one. */
		fine = dbt_probes >= 2 || (dbt_probes == 1 && (fixed ||
		    (dev != dbt_last_probe_dev && !dbt_thinned)));
		coarse = dbt_probes >= 1 && fixed;
		dbt_last_probe_dev = dev;
		what = "probe";
	} else {
		fine = coarse = true;
		what = "attach";
	}
	/* The breadcrumb keeps the first 39 characters: name, parent, driver. */
	dbt_step(kind, 0, 0, fine, coarse, device_get_nameunit(dev),
	    "%s %s@%s %s %s", what, device_get_nameunit(dev), pname,
	    drv != NULL ? drv->name : "?", loc);
}

void
dbt_hook(void *func)
{
	char buf[64];
	const char *name;

	name = dbt_symbol(func, buf, sizeof(buf));
	dbt_step(DBT_KIND_HOOK, 0, 0, true, true, name, "hook %s", name);
}

void
dbt_event(const char *fmt, ...)
{
	char text[DBT_TEXT_SIZE];
	va_list ap;

	if (!dbt_active)
		return;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	dbt_step(DBT_KIND_EVENT, 0, 0, true, true, text, "%s", text);
}

void
dbt_complete(void)
{
	if (!dbt_active)
		return;
	mtx_lock(&dbt_mtx);
	dbt_status = DBT_STATUS_COMPLETE;
	dbt_steps++;
	memset(&dbt_ring[dbt_steps % DBT_RING], 0, sizeof(struct dbt_crumb));
	dbt_ring[dbt_steps % DBT_RING].index = dbt_steps;
	dbt_ring[dbt_steps % DBT_RING].ms = dbt_ms();
	dbt_ring[dbt_steps % DBT_RING].kind = DBT_KIND_COMPLETE;
	strlcpy(dbt_ring[dbt_steps % DBT_RING].text,
	    "boot complete: root mounted, executing init",
	    sizeof(dbt_ring[0].text));
	if ((dbt_flags & DBT_FLAG_WRITE_ERROR) == 0)
		dbt_stopped = false;	/* the reserved write */
	dbt_write(true);
	dbt_stopped = true;
	dbt_active = 0;
	mtx_unlock(&dbt_mtx);
	printf("boottrace: boot complete; %u steps, %u full records, %u "
	    "breadcrumbs of ~%u bytes (ring of %d)\n", dbt_steps, dbt_seq,
	    dbt_step_writes, dbt_step_cost, dbt_ring_k);
}

void
dbt_panic(const char *msg)
{
	struct dbt_crumb *c;

	if (!dbt_active || dbt_in_write ||
	    (curthread->td_pflags & TDP_EFIRT) != 0)
		return;
#if defined(__amd64__)
	if (is_fpu_kern_thread(0))
		return;
#endif
	dbt_status = DBT_STATUS_PANIC;
	dbt_steps++;
	c = &dbt_ring[dbt_steps % DBT_RING];
	memset(c, 0, sizeof(*c));
	c->index = dbt_steps;
	c->ms = dbt_ms();
	c->kind = DBT_KIND_PANIC;
	c->cpu = PCPU_GET(cpuid);
	snprintf(c->text, sizeof(c->text), "panic: %s", msg);
	if ((dbt_flags & DBT_FLAG_WRITE_ERROR) == 0)
		dbt_stopped = false;
	/* The scheduler is stopped: locks are bypassed from here on. */
	dbt_write(true);
	dbt_stopped = true;
	dbt_active = 0;
}
