/* SPDX-License-Identifier: BSD-2-Clause */
/* Injected host VM/scheduler primitives. No hardware or native-DMA claim. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#undef __amd64__
#define KMSAN 1
#define __unused __attribute__((unused))
#define __predict_true(x) (x)
#define KASSERT(c, msg) assert(c)
#define panic(...) abort()
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define min MIN
#define roundup2(a, b) (((a) + ((b) - 1)) & ~((b) - 1))
#define trunc_page(a) ((__typeof__(a))((uintptr_t)(a) & ~(uint64_t)PAGE_MASK))
#define howmany(a, b) (((a) + (b) - 1) / (b))
#define __is_aligned(a, b) (((uintptr_t)(a) & ((b) - 1)) == 0)
#define PAGE_SIZE UINT64_C(4096)
#define PAGE_MASK (PAGE_SIZE - 1)
#define atop(a) ((a) / PAGE_SIZE)
#define ptoa(a) ((a) * PAGE_SIZE)
#define MAX_BPAGES 4096
#define BUS_SPACE_MAXADDR UINT64_MAX
#define BUS_SPACE_MAXSIZE UINT64_MAX
#define BUS_SPACE_UNRESTRICTED (~0)
#define M_NOWAIT 1
#define M_WAITOK 2
#define M_ZERO 4
#define M_UNPROTECTED 8
#define M_DEVBUF 0
#define M_BUSDMA 0
#define M_BOUNCE 0
#define MTX_DEF 0
#define MA_OWNED 0
#define MTX_SYSINIT(...)
#define MALLOC_DEFINE(...) int __attribute__((unused)) malloc_stub
#define CTR1(...)
#define CTR2(...)
#define CTR3(...)
#define CTR4(...)
#define CTR5(...)
#define DOMAINSET_PREF(d) (d)
#define VM_MEMATTR_DEFAULT 0
#define VM_MEMATTR_UNCACHEABLE 1
#define BUS_DMA_WAITOK 0
#define BUS_DMA_NOWAIT 1
#define BUS_DMA_ALLOCNOW 2
#define BUS_DMA_COHERENT 4
#define BUS_DMA_ZERO 8
#define BUS_DMA_NOCACHE 0x200
#define BUS_DMA_KEEP_PG_OFFSET 0x400
#define BUS_DMA_LOAD_MBUF 0x800
#define BUS_DMASYNC_PREREAD 1
#define BUS_DMASYNC_POSTREAD 2
#define BUS_DMASYNC_PREWRITE 4
#define BUS_DMASYNC_POSTWRITE 8
#define BUS_DMA_LOCK 1
#define BUS_DMA_UNLOCK 2
#define STAILQ_FOREACH_SAFE(v, h, f, n)                                        \
	for ((v) = STAILQ_FIRST(h); (v) && ((n) = STAILQ_NEXT(v, f), 1);       \
	     (v) = (n))
#define MEMDESC_VADDR 1
#define MEMDESC_PADDR 2
#define MEMDESC_VLIST 3
#define MEMDESC_PLIST 4
#define MEMDESC_UIO 5
#define MEMDESC_MBUF 6
#define MEMDESC_VMPAGES 7
#define UIO_USERSPACE 1
#define SIZE_T_MAX SIZE_MAX
#define PRI_ITHD 0
#define PI_SWI(x) 0
#define SWI_BUSDMA 0
#define CTLFLAG_RD 0
#define OID_AUTO 0
#define SYSCTL_INT(...)
typedef uint64_t bus_addr_t, bus_size_t, vm_paddr_t, vm_offset_t, vm_size_t;
typedef int bus_dmasync_op_t, bus_dma_lock_op_t, vm_memattr_t;
typedef int64_t __sbintime_t, sbintime_t;
struct pmap;
typedef struct pmap *pmap_t;
struct bus_dma_tag;
struct bus_dmamap;
typedef struct bus_dma_tag *bus_dma_tag_t;
typedef struct bus_dmamap *bus_dmamap_t;
typedef struct {
	bus_addr_t ds_addr;
	bus_size_t ds_len;
} bus_dma_segment_t;
typedef void bus_dmamap_callback_t(void *, bus_dma_segment_t *, int, int);
typedef void bus_dma_lock_t(void *, bus_dma_lock_op_t);
struct vm_page {
	vm_paddr_t phys;
	char *bytes;
	bool fictitious;
};
typedef struct vm_page *vm_page_t;
struct memdesc {
	int md_type;
	uint64_t md_len;
	int md_nseg, md_offset;
	union {
		void *md_vaddr;
		vm_paddr_t md_paddr;
		bus_dma_segment_t *md_list;
		struct uio *md_uio;
		void *md_mbuf;
		vm_page_t *md_ma;
	} u;
};
struct proc {
	void *p_vmspace;
};
struct thread {
	struct proc *td_proc;
};
struct mbuf {
	int m_len, m_flags;
	struct mbuf *m_next;
	char *m_data;
	struct {
		int len;
	} m_pkthdr;
	int m_epg_hdrlen, m_epg_1st_off, m_epg_npgs, m_epg_last_len,
	    m_epg_trllen;
	char *m_epg_hdr, *m_epg_trail;
	uint64_t m_epg_pa[4];
};
#define M_EXTPG 1
#define M_ASSERTEXTPG(m) assert((m)->m_flags &M_EXTPG)
#define mtod(m, t) ((t)((m)->m_data))
static int m_epg_pagelen(struct mbuf *m, int i, int off)
{
	return i == m->m_epg_npgs - 1 ? m->m_epg_last_len : PAGE_SIZE - off;
}
struct iovec {
	void *iov_base;
	size_t iov_len;
};
struct uio {
	int uio_segflg;
	struct thread *uio_td;
	uint64_t uio_resid;
	struct iovec *uio_iov;
	int uio_iovcnt;
};
struct sysctl_ctx_list {
	int unused;
};
struct sysctl_oid {
	int unused;
};
struct mtx {
	int held;
};
typedef void *busdma_bufalloc_t;
struct busdma_bufzone {
	size_t size;
	void *umazone;
};
#define BUS_DMA_TEMPLATE_MD_FIELDS unsigned arm64_requires_translation;
/* INSERT_TEMPLATE */
/* INSERT_PARAMS */
#define BUS_DMA_MEMDESC_UNSUPPORTED(dmat)                                      \
	(bus_dma_bounce_ram_translated(dmat) ? EINVAL : 0)
#define BUS_DMA_TEMPLATE_MD_CHECK(t) ((t)->arm64_requires_translation != 0)
#define BUS_DMA_TEMPLATE_MD_INIT(t) ((t)->arm64_requires_translation = 0)
#include <arm64/broadcom/bcm2712_dma_ram.h>
#include <arm64/include/bus_dma_impl.h>
static busdma_bufalloc_t nonsecure_allocator;
static const long Maxmem = 2 * 1024 * 1024;
static const unsigned dcache_line_size = 64;
static const uint64_t prot_ns_shared_pa = 0;
static pmap_t kernel_pmap = (void *)1;
static int fail_alloc = -1, fail_zone, realm, cache_calls, wake_calls,
	   use_bufzone, kmem_frees;
static uint64_t matched_limit;
static struct vm_page host_pages[1024];
static unsigned host_page_count;
static uint64_t next_phys = 0x10000;
static void *host_alloc(size_t n, int flags)
{
	if (fail_alloc == 0)
		return NULL;
	if (fail_alloc > 0)
		fail_alloc--;
	return flags & M_ZERO ? calloc(1, n) : malloc(n);
}
static void host_free(void *p) { free(p); }
#define malloc(n, t, f) host_alloc(n, f)
#define free(p, t) host_free(p)
#define malloc_domainset(n, t, d, f) host_alloc(n, f)
#define mallocarray_domainset(n, s, t, d, f) host_alloc((n) * (s), f)
static bool in_realm(void) { return realm; }
static bool vm_addr_align_ok(uint64_t a, uint64_t n)
{
	return n <= 1 || !(a & (n - 1));
}
static bool vm_addr_bound_ok(uint64_t a, uint64_t n, uint64_t b)
{
	return !b || (n && (a & ~(b - 1)) == ((a + n - 1) & ~(b - 1)));
}
static int vm_phys_domain_match(int d, uint64_t low, uint64_t high)
{
	(void)low;
	matched_limit = high;
	return d;
}
static unsigned atomic_fetchadd_int(unsigned *p, int n)
{
	unsigned old = *p;
	*p += n;
	return old;
}
static bool atomic_cmpset_int(unsigned *p, unsigned old, unsigned value)
{
	if (*p != old)
		return false;
	*p = value;
	return true;
}
static void atomic_add_int(unsigned *p, unsigned n) { *p += n; }
static void atomic_subtract_int(unsigned *p, unsigned n) { *p -= n; }
static unsigned atomic_load_acq_int(unsigned *p) { return *p; }
static void atomic_clear_int(unsigned *p, unsigned n) { *p &= ~n; }
static void mtx_lock(struct mtx *m)
{
	assert(!m->held);
	m->held = 1;
}
static void mtx_unlock(struct mtx *m)
{
	assert(m->held);
	m->held = 0;
}
static void mtx_assert(struct mtx *m, int op)
{
	(void)op;
	assert(m->held);
}
static int sbinuptime(void) { return 1; }
static void wakeup(void *p)
{
	(void)p;
	wake_calls++;
}
static void _busdma_dflt_lock(void *p, int op)
{
	(void)p;
	(void)op;
	abort();
}
static void host_register(void *v, size_t n, uint64_t physical)
{
	for (size_t off = 0; off < n; off += PAGE_SIZE) {
		assert(host_page_count < 1024);
		host_pages[host_page_count++] = (struct vm_page){
		    .phys = physical + off, .bytes = (char *)v + off};
	}
}
static vm_page_t host_phys_page(uint64_t physical)
{
	for (unsigned i = 0; i < host_page_count; i++)
		if (host_pages[i].phys == trunc_page(physical))
			return &host_pages[i];
	return NULL;
}
static vm_page_t vm_phys_paddr_to_vm_page(uint64_t physical)
{
	vm_page_t p = host_phys_page(physical);
	return p && !p->fictitious ? p : NULL;
}
#define PHYS_TO_VM_PAGE(a) host_phys_page(a)
#define VM_PAGE_TO_PHYS(p) ((p)->phys)
static uint64_t pmap_kextract(uint64_t v)
{
	for (unsigned i = 0; i < host_page_count; i++)
		if (v >= (uintptr_t)host_pages[i].bytes &&
		    v - (uintptr_t)host_pages[i].bytes < PAGE_SIZE)
			return host_pages[i].phys + v -
			       (uintptr_t)host_pages[i].bytes;
	abort();
}
#define vtophys(v) pmap_kextract((uintptr_t)(v))
static uint64_t pmap_extract(pmap_t p, uint64_t v)
{
	(void)p;
	return pmap_kextract(v);
}
static void *pmap_quick_enter_page(vm_page_t p) { return p->bytes; }
static void pmap_quick_remove_page(void *p) { (void)p; }
static void cpu_dcache_wb_range(void *v, size_t n)
{
	(void)v;
	(void)n;
	cache_calls++;
}
static void cpu_dcache_inv_range(void *v, size_t n)
{
	(void)v;
	(void)n;
	cache_calls++;
}
static void cpu_dcache_wbinv_range(void *v, size_t n)
{
	(void)v;
	(void)n;
	cache_calls++;
}
static void dsb(int v) { (void)v; }
#define sy 0
#define oshld 0
#define oshst 0
#define dmb dsb
static void kmsan_bus_dmamap_sync(struct memdesc *m, int op)
{
	(void)m;
	(void)op;
}
static void *vmspace_pmap(void *v)
{
	(void)v;
	return kernel_pmap;
}
static void *host_kmem(size_t n, int flags, uint64_t low, uint64_t high,
		       uint64_t align)
{
	void *v;
	uint64_t physical =
	    roundup2(MAX(next_phys, low), MAX(align, PAGE_SIZE));
	if (fail_alloc == 0 || n == 0 || physical > high ||
	    n - 1 > high - physical)
		return NULL;
	if (posix_memalign(&v, MAX(align, PAGE_SIZE), roundup2(n, PAGE_SIZE)))
		return NULL;
	if (flags & M_ZERO)
		memset(v, 0, n);
	host_register(v, roundup2(n, PAGE_SIZE), physical);
	next_phys = physical + roundup2(n, PAGE_SIZE);
	return v;
}
static void *kmem_alloc_attr_domainset(int d, size_t n, int f, uint64_t l,
				       uint64_t h, int a)
{
	(void)d;
	(void)a;
	return host_kmem(n, f, l, h, PAGE_SIZE);
}
static void *kmem_alloc_contig_domainset(int d, size_t n, int f, uint64_t l,
					 uint64_t h, uint64_t al, uint64_t b,
					 int a)
{
	(void)d;
	(void)a;
	(void)b;
	return host_kmem(n, f, l, h, al);
}
static void kmem_free(void *v, size_t n)
{
	(void)n;
	kmem_frees++;
	host_free(v);
}
static struct busdma_bufzone *busdma_bufalloc_findzone(void *a, size_t n)
{
	static struct busdma_bufzone z = {4096, NULL};
	(void)a;
	return use_bufzone && n <= PAGE_SIZE ? &z : NULL;
}
static void *uma_zalloc(void *z, int f)
{
	(void)z;
	return host_kmem(PAGE_SIZE, f, 0, UINT64_MAX, PAGE_SIZE);
}
static int alloc_bounce_zone(bus_dma_tag_t);
static int alloc_bounce_pages(bus_dma_tag_t, unsigned);
static int bus_dmamap_load_mem(bus_dma_tag_t, bus_dmamap_t, struct memdesc *,
			       bus_dmamap_callback_t *, void *, int);
static int bus_dma_tag_create(bus_dma_tag_t, bus_size_t, bus_addr_t, bus_addr_t,
			      bus_addr_t, void *, void *, bus_size_t, int,
			      bus_size_t, int, bus_dma_lock_t *, void *,
			      bus_dma_tag_t *);
static void bus_dmamap_unload(bus_dma_tag_t, bus_dmamap_t);
static int _bus_dmamap_load_buffer(bus_dma_tag_t, bus_dmamap_t, void *,
				   bus_size_t, pmap_t, int, bus_dma_segment_t *,
				   int *);
static int _bus_dmamap_load_phys(bus_dma_tag_t, bus_dmamap_t, vm_paddr_t,
				 bus_size_t, int, bus_dma_segment_t *, int *);
static int _bus_dmamap_load_ma(bus_dma_tag_t, bus_dmamap_t, vm_page_t *,
			       bus_size_t, int, int, bus_dma_segment_t *,
			       int *);
static void _bus_dmamap_waitok(bus_dma_tag_t, bus_dmamap_t, struct memdesc *,
			       bus_dmamap_callback_t *, void *);
static void _bus_dmamap_load_kmsan(bus_dma_tag_t, bus_dmamap_t,
				   struct memdesc *);
static bus_dma_segment_t *_bus_dmamap_complete(bus_dma_tag_t, bus_dmamap_t,
					       bus_dma_segment_t *, int, int);
