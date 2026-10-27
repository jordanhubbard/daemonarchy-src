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

#define	DBT_VARNAME		"DaemonarchyBootTrace"
/* {9262df38-e448-4c99-bdd0-e70eada0faa1} */
#define	DBT_GUID_INIT		{ 0x9262df38, 0xe448, 0x4c99, \
    { 0xbd, 0xd0, 0xe7, 0x0e, 0xad, 0xa0, 0xfa, 0xa1 } }
#define	DBT_GUID_STRING		"9262df38-e448-4c99-bdd0-e70eada0faa1"
/* EFI_VARIABLE_NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS */
#define	DBT_ATTRIBUTES		0x00000007

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
	uint32_t	budget_writes;	/* writes the budget allows */
	uint32_t	reserved[2];
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

#ifdef _KERNEL
#ifndef _STANDALONE
_Static_assert(sizeof(struct dbt_header) == 128, "dbt_header layout");
_Static_assert(sizeof(struct dbt_crumb) == 96, "dbt_crumb layout");

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
