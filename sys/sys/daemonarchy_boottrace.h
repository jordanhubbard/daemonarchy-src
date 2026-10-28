/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */

/*
 * Reset-surviving boot trace ("boottrace").
 *
 * When the loader tunable debug.boottrace=1 is set, the kernel records a
 * breadcrumb before every SYSINIT, device probe/attach and interrupt-driven
 * configuration hook.  Once EFI runtime services are usable the newest
 * breadcrumbs and the tail of the kernel message buffer are written,
 * synchronously and before the step runs, into one UEFI variable that is
 * overwritten in place.  The loader (boottrace command), the kernel and a
 * Windows PowerShell reader decode the same little-endian record below.
 *
 * This is not boottrace(4); that facility keeps its log in RAM only.
 */

#ifndef _SYS_DAEMONARCHY_BOOTTRACE_H_
#define	_SYS_DAEMONARCHY_BOOTTRACE_H_

/* The record uses fixed-width types; buildworld's test-includes compiles this alone. */
#include <sys/types.h>

#define	DBT_VARNAME		"DaemonarchyBootTrace"
/* {9262df38-e448-4c99-bdd0-e70eada0faa1} */
#define	DBT_GUID_INIT		{ 0x9262df38, 0xe448, 0x4c99, \
    { 0xbd, 0xd0, 0xe7, 0x0e, 0xad, 0xa0, 0xfa, 0xa1 } }
#define	DBT_GUID_STRING		"9262df38-e448-4c99-bdd0-e70eada0faa1"
/* EFI_VARIABLE_NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS */
#define	DBT_ATTRIBUTES		0x00000007

/*
 * Per-step breadcrumbs: tiny variables written before every persisted
 * step, so a small NVRAM store still holds hundreds of steps.  They form a
 * ring of debug.boottrace.ring variables (default 16) named DBTStep00,
 * DBTStep01, ...: breadcrumb N (dbt_step.seq, from 1) overwrites slot
 * (N - 1) % ring, so the newest steps before a hang are always on NVRAM and
 * the live footprint is bounded.  Readers sort the slots by seq.  The
 * kernel deletes every slot (and the single DBTStep variable written by
 * older kernels, which readers still accept) when tracing starts.  The
 * full record above is written only at checkpoints, panic, completion and
 * the budget stop.  Readers show whichever is newer.
 */
#define	DBT_STEP_VARNAME	"DBTStep"	/* old single slot; ring prefix */
#define	DBT_STEP_TEXT		40
#define	DBT_RING_DEFAULT	16
#define	DBT_RING_MAX		64
#define	DBT_RING_NAMELEN	(sizeof(DBT_STEP_VARNAME) + 2)	/* "DBTStepNN" */

/*
 * Hardware-trace stick loader self-report ("DBTLoader", same GUID).  The
 * loader records each step of its stick cycle here, so a stick whose
 * writes never reach the media is detected on the next boot from NVRAM
 * alone, and the loop counter does not depend on the stick.
 */
#define	DBT_LDR_VARNAME		"DBTLoader"
#define	DBT_LDR_MAGIC		"DBTLDR01"

#define	DBT_LS_FOUND		1	/* found the stick's data volume */
#define	DBT_LS_STATE		2	/* read STATE.TXT */
#define	DBT_LS_FILED		3	/* filed the previous trace */
#define	DBT_LS_CHOSEN		4	/* chose the next experiment */
#define	DBT_LS_WROTE		5	/* wrote STATE.TXT */
#define	DBT_LS_VERIFIED		6	/* read STATE.TXT back from the media */
#define	DBT_LS_BOOTING		7	/* handed the experiment to the kernel */
#define	DBT_LS_STOPPED		8	/* stopped; text says why */

#define	DBT_LF_RUNNING		0x0001	/* loop running: stick should hold next/traces/pending */
#define	DBT_LF_NOPERSIST	0x0002	/* stopped: stick writes did not persist */
#define	DBT_LF_REPEAT		0x0004	/* stopped: same experiment staged too often */
#define	DBT_LF_MAP		0x0008	/* block map valid: raw writes */
#define	DBT_LF_MAPBAD		0x0010	/* block map present but rejected */
#define	DBT_LF_TEST_DROP	0x0020	/* test: writes deliberately dropped */

#define	DBT_LM_NONE		0
#define	DBT_LM_RAW		1	/* EFI_BLOCK_IO via the block map */
#define	DBT_LM_FAT		2	/* firmware FAT driver (no usable map) */

#define	DBT_LV_NONE		0
#define	DBT_LV_OK		1	/* read back from the media and equal */
#define	DBT_LV_MISMATCH		2
#define	DBT_LV_ERROR		3
#define	DBT_LV_SKIPPED		4	/* test drop */

struct dbt_loader {
	char		magic[8];	/* DBT_LDR_MAGIC, not NUL terminated */
	uint32_t	epoch;		/* STATE.TXT epoch= (queue generation) */
	uint32_t	cycle;		/* loader cycles in this epoch */
	uint32_t	step;		/* DBT_LS_* reached */
	uint32_t	flags;		/* DBT_LF_* */
	uint32_t	method;		/* DBT_LM_* */
	uint32_t	write_status;	/* last stick write: EFI status code */
	uint32_t	verify;		/* DBT_LV_* of the last stick write */
	uint32_t	next;		/* STATE.TXT written: next= */
	uint32_t	traces;		/* traces= */
	uint32_t	pending;	/* pending= (0: none) */
	uint32_t	repeat;		/* consecutive cycles that staged pending */
	uint32_t	stick_next;	/* STATE.TXT read at this cycle's start */
	uint32_t	stick_traces;
	uint32_t	stick_pending;
	char		text[64];	/* last message, NUL terminated */
};

#define	DBT_MAGIC		"DBTRACE1"
#define	DBT_VERSION		1

#define	DBT_STATUS_RUNNING	1	/* boot did not reach completion */
#define	DBT_STATUS_COMPLETE	2	/* init was started */
#define	DBT_STATUS_PANIC	3	/* kernel panicked; see last crumb */

#define	DBT_FLAG_LOADER_SEEN	0x0001	/* loader already reported it */
#define	DBT_FLAG_BUDGET		0x0002	/* NVRAM budget exhausted; stopped */
#define	DBT_FLAG_WRITE_ERROR	0x0004	/* SetVariable failed; stopped */
#define	DBT_FLAG_RESUMED	0x0008	/* resumed after earlier budget stop */
#define	DBT_FLAG_TEST		0x0010	/* a test hang was armed */
#define	DBT_FLAG_SAVED		0x0020	/* loader saved it to USB */
#define	DBT_FLAG_THINNED	0x0040	/* budget low: probes no longer persisted */
#define	DBT_FLAG_STRIDED	0x0080	/* budget low: every Nth step persisted */
#define	DBT_FLAG_REFRESHED	0x0100	/* firmware freed NVRAM at runtime */
/* Bits 16..23: log2 of the widest breadcrumb stride used (with STRIDED). */
#define	DBT_FLAG_STRIDE_SHIFT(f)	(((f) >> 16) & 0xff)

#define	DBT_KIND_SYSINIT	1
#define	DBT_KIND_PROBE		2
#define	DBT_KIND_ATTACH		3
#define	DBT_KIND_HOOK		4
#define	DBT_KIND_EVENT		5
#define	DBT_KIND_PANIC		6
#define	DBT_KIND_COMPLETE	7

#define	DBT_TEXT_SIZE		76
#define	DBT_KERNEL_SIZE		64
#define	DBT_MAX_CRUMBS		16
#define	DBT_MAX_MSGBUF		3072

struct dbt_header {
	char		magic[8];	/* DBT_MAGIC, not NUL terminated */
	uint16_t	version;
	uint16_t	header_size;	/* sizeof(struct dbt_header) */
	uint16_t	crumb_size;	/* sizeof(struct dbt_crumb) */
	uint16_t	crumb_count;	/* crumbs that follow, oldest first */
	uint32_t	record_size;	/* header + crumbs + msgbuf tail */
	uint32_t	seq;		/* NVRAM write number this boot */
	uint32_t	status;		/* DBT_STATUS_* */
	uint32_t	flags;		/* DBT_FLAG_* */
	uint32_t	steps;		/* index of the newest step seen */
	uint32_t	resume_from;	/* first step index persisted */
	uint32_t	msg_len;	/* message buffer bytes after crumbs */
	uint32_t	uptime_ms;	/* time of this write since kernel entry */
	uint32_t	nv_left_kb;	/* NVRAM left when tracing started */
	uint32_t	budget_writes;	/* full writes the budget allows */
	uint32_t	nv_used_kb;	/* NVRAM this boot has written so far */
	uint32_t	step_writes;	/* DBTStep breadcrumb writes so far */
	char		kernel[DBT_KERNEL_SIZE]; /* kernel version line */
};

struct dbt_crumb {
	uint32_t	index;		/* step number in this boot */
	uint32_t	ms;		/* milliseconds since kernel entry */
	uint16_t	kind;		/* DBT_KIND_* */
	uint16_t	cpu;
	uint32_t	a;		/* SYSINIT subsystem */
	uint32_t	b;		/* SYSINIT order */
	char		text[DBT_TEXT_SIZE]; /* NUL terminated */
};

struct dbt_step {
	uint32_t	seq;		/* breadcrumb write number this boot */
	uint32_t	index;		/* step number in this boot */
	uint32_t	a;		/* SYSINIT subsystem */
	uint32_t	b;		/* SYSINIT order */
	uint32_t	ms;		/* milliseconds since kernel entry */
	uint16_t	kind;		/* DBT_KIND_* */
	uint16_t	cpu;
	char		text[DBT_STEP_TEXT]; /* NUL terminated */
};

#ifdef _KERNEL
#ifndef _STANDALONE
_Static_assert(sizeof(struct dbt_header) == 128, "dbt_header layout");
_Static_assert(sizeof(struct dbt_crumb) == 96, "dbt_crumb layout");
_Static_assert(sizeof(struct dbt_step) == 64, "dbt_step layout");
_Static_assert(sizeof(struct dbt_loader) == 128, "dbt_loader layout");

struct sysinit;
struct _device;
struct kobj_class;

extern int dbt_active;

void	dbt_start(void);
void	dbt_sysinit(struct sysinit *);
void	dbt_device(int kind, struct _device *dev, struct kobj_class *drv);
void	dbt_hook(void *func);
void	dbt_event(const char *fmt, ...) __printflike(1, 2);
void	dbt_complete(void);
void	dbt_panic(const char *msg);

#define	DBT_SYSINIT(sip)	do {				\
	if (__predict_false(dbt_active))			\
		dbt_sysinit(sip);				\
} while (0)
#define	DBT_DEVICE(kind, dev, drv)	do {			\
	if (__predict_false(dbt_active))			\
		dbt_device((kind), (dev), (drv));		\
} while (0)
#define	DBT_HOOK(func)	do {					\
	if (__predict_false(dbt_active))			\
		dbt_hook(func);					\
} while (0)
#endif /* !_STANDALONE */
#endif /* _KERNEL */

#endif /* !_SYS_DAEMONARCHY_BOOTTRACE_H_ */
