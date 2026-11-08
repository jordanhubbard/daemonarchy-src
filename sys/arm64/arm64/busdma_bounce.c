/*-
 * Copyright (c) 1997, 1998 Justin T. Gibbs.
 * Copyright (c) 2015-2016 The FreeBSD Foundation
 * All rights reserved.
 *
 * Portions of this software were developed by Andrew Turner
 * under sponsorship of the FreeBSD Foundation.
 *
 * Portions of this software were developed by Semihalf
 * under sponsorship of the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification, immediately at the beginning of the file.
 * 2. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/domainset.h>
#include <sys/malloc.h>
#include <sys/bus.h>
#include <sys/busdma_bufalloc.h>
#include <sys/interrupt.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/lock.h>
#include <sys/memdesc.h>
#include <sys/msan.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/vm_page.h>
#include <vm/vm_map.h>
#include <vm/vm_phys.h>

#include <machine/atomic.h>
#include <machine/bus.h>
#include <machine/md_var.h>
#include <machine/rsi.h>
#include <arm64/include/bus_dma_impl.h>

#include <arm64/broadcom/bcm2712_dma_ram.h>

#define MAX_BPAGES 4096

enum {
	BF_COULD_BOUNCE		= 0x01,
	BF_MIN_ALLOC_COMP	= 0x02,
	BF_KMEM_ALLOC_PAGES	= 0x04,
	BF_KMEM_ALLOC_CONTIG	= 0x08,
	BF_KMEM_ALLOC		= BF_KMEM_ALLOC_PAGES | BF_KMEM_ALLOC_CONTIG,
	BF_COHERENT		= 0x10,
	BF_FORCE_MAP		= 0x20,
};

struct bounce_page;
struct bounce_zone;

struct bus_dma_ram_context {
	struct bcm2712_dma_policy policy;
	u_int refs;
};

struct bus_dma_tag {
	struct bus_dma_tag_common common;
	size_t			alloc_size;
	size_t			alloc_alignment;
	int			map_count;
	int			bounce_flags;
	bus_dma_segment_t	*segments;
	struct bounce_zone	*bounce_zone;
	struct bus_dma_ram_context *ram;
	struct bcm2712_dma_ram ram_view;
	bus_dma_tag_t ram_parent;
	u_int ram_children;
	int committed_domain;
};

static SYSCTL_NODE(_hw, OID_AUTO, busdma, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "Busdma parameters");

struct sync_list {
	char		*vaddr;		/* kva of client data */
	bus_addr_t	paddr;		/* physical address */
	vm_page_t	pages;		/* starting page of client data */
	bus_size_t	datacount;	/* client data count */
};

struct bus_dmamap {
	STAILQ_HEAD(, bounce_page) bpages;
	int		       pagesneeded;
	int		       pagesreserved;
	bus_dma_tag_t	       dmat;
	struct memdesc	       mem;
	bus_dmamap_callback_t *callback;
	void		      *callback_arg;
	__sbintime_t	       queued_time;
	STAILQ_ENTRY(bus_dmamap) links;
	u_int			flags;
#define	DMAMAP_COHERENT		(1 << 0)
#define	DMAMAP_FROM_DMAMEM	(1 << 1)
#define	DMAMAP_MBUF		(1 << 2)
#define DMAMAP_PENDING		(1 << 3)
#define DMAMAP_REPLAY		(1 << 4)
#define DMAMAP_QUEUED		(1 << 5)
#define DMAMAP_CALLBACK		(1 << 6)
#define DMAMAP_COUNTING		(1 << 7)
#define DMAMAP_PREPARED		(1 << 8)
#define DMAMAP_LOADED		(1 << 9)
	int			sync_count;
	bus_size_t ram_bytes;
	int ram_error;
	int ram_alloc_flags;
#ifdef KMSAN
	struct memdesc	       kmsan_mem;
#endif
	struct sync_list	slist[];
};

static struct bus_dmamap nobounce_dmamap = {
	.flags = DMAMAP_COHERENT,
};

static bool _bus_dmamap_pagesneeded(bus_dma_tag_t dmat, bus_dmamap_t map,
    vm_paddr_t buf, bus_size_t buflen, int *pagesneeded);
static void _bus_dmamap_count_pages(bus_dma_tag_t dmat, bus_dmamap_t map,
    pmap_t pmap, void *buf, bus_size_t buflen, int flags);
static void _bus_dmamap_count_phys(bus_dma_tag_t dmat, bus_dmamap_t map,
    vm_paddr_t buf, bus_size_t buflen, int flags);

static busdma_bufalloc_t nonsecure_allocator;

static MALLOC_DEFINE(M_BUSDMA, "busdma", "busdma metadata");

#define	dmat_alignment(dmat)	((dmat)->common.alignment)
#define	dmat_bounce_flags(dmat)	((dmat)->bounce_flags)
#define	dmat_boundary(dmat)	((dmat)->common.boundary)
#define	dmat_domain(dmat)	((dmat)->common.domain)
#define	dmat_flags(dmat)	((dmat)->common.flags)
#define	dmat_highaddr(dmat)	((dmat)->common.highaddr)
#define	dmat_lowaddr(dmat) ((dmat)->ram != NULL ? \
    (dmat)->ram_view.physical_limit : (dmat)->common.lowaddr)
#define	dmat_lockfunc(dmat)	((dmat)->common.lockfunc)
#define	dmat_lockfuncarg(dmat)	((dmat)->common.lockfuncarg)
#define	dmat_maxsegsz(dmat)	((dmat)->common.maxsegsz)
#define	dmat_nsegments(dmat)	((dmat)->common.nsegments)

/* These hooks are private to this inclusion of the common bounce engine. */
#define BUSDMA_DEFERRED(map) do { \
	if ((map)->dmat->ram != NULL) \
		(map)->flags = ((map)->flags & ~DMAMAP_REPLAY) | \
		    DMAMAP_PENDING | DMAMAP_QUEUED; \
} while (0)
#define BUSDMA_REPLAY_BEGIN(map) do { \
	if ((map)->dmat->ram != NULL) { \
		(map)->flags &= ~DMAMAP_QUEUED; \
		(map)->flags |= DMAMAP_REPLAY; \
	} \
} while (0)
#define BUSDMA_SEGMENT_START_OK(dmat, addr) \
    ((dmat)->ram == NULL || vm_addr_align_ok((addr), (dmat)->common.alignment))
#define BUSDMA_REPLAY_SAVED_LOCK
#include "../../kern/subr_busdma_bounce.c"

static bool
ram_map_busy(bus_dmamap_t map)
{
	return ((atomic_load_acq_int(&map->flags) & DMAMAP_PENDING) != 0 &&
	    (map->flags & DMAMAP_REPLAY) == 0);
}

/* Only segment output crosses into the device domain.  Cache maintenance,
 * bounce-page ownership and physical page lookups never see this address.
 */
static bool
bounce_bus_dmamap_addsegs(bus_dma_tag_t dmat, bus_dmamap_t map,
    bus_addr_t physical, bus_size_t length, bus_dma_segment_t *segs, int *segp)
{
	bus_addr_t device;

	if (dmat->ram == NULL)
		return (_bus_dmamap_addsegs(dmat, map, physical, length,
		    segs, segp));
	if (bcm2712_dma_ram_address(&dmat->ram_view, physical, length,
	    &device) != 0)
		return (false);
	return (_bus_dmamap_addsegs(dmat, map, device, length, segs, segp));
}

static int
bounce_bus_dma_zone_setup(bus_dma_tag_t dmat)
{
	struct bounce_zone *bz;
	bus_size_t maxsize;
	int error;

	/*
	 * Round size up to a full page, and add one more page because
	 * there can always be one more boundary crossing than the
	 * number of pages in a transfer.
	 */
	maxsize = roundup2(dmat->common.maxsize, PAGE_SIZE) + PAGE_SIZE;

	/* Must bounce */
	if ((error = alloc_bounce_zone(dmat)) != 0)
		return (error);
	bz = dmat->bounce_zone;

	if (ptoa(bz->total_bpages) < maxsize) {
		int pages;

		pages = atop(maxsize) + 1 - bz->total_bpages;

		/* Add pages to our bounce pool */
		if (alloc_bounce_pages(dmat, pages) < pages)
			return (ENOMEM);
	}
	/* Performed initial allocation */
	dmat->bounce_flags |= BF_MIN_ALLOC_COMP;

	return (error);
}

/*
 * Return true if the DMA should bounce because the start or end does not fall
 * on a cacheline boundary (which would require a partial cacheline flush).
 * COHERENT memory doesn't trigger cacheline flushes.  Memory allocated by
 * bus_dmamem_alloc() is always aligned to cacheline boundaries, and there's a
 * strict rule that such memory cannot be accessed by the CPU while DMA is in
 * progress (or by multiple DMA engines at once), so that it's always safe to do
 * full cacheline flushes even if that affects memory outside the range of a
 * given DMA operation that doesn't involve the full allocated buffer.  If we're
 * mapping an mbuf, that follows the same rules as a buffer we allocated.
 */
static bool
cacheline_bounce(bus_dma_tag_t dmat, bus_dmamap_t map, bus_addr_t paddr,
    bus_size_t size)
{

#define	DMAMAP_CACHELINE_FLAGS						\
    (DMAMAP_FROM_DMAMEM | DMAMAP_COHERENT | DMAMAP_MBUF)
	if ((dmat->bounce_flags & BF_COHERENT) != 0)
		return (false);
	if (map != NULL && (map->flags & DMAMAP_CACHELINE_FLAGS) != 0)
		return (false);
	return (((paddr | size) & (dcache_line_size - 1)) != 0);
#undef DMAMAP_CACHELINE_FLAGS
}

/*
 * Return true if the given address does not fall on the alignment boundary.
 */
static bool
alignment_bounce(bus_dma_tag_t dmat, bus_addr_t addr)
{

	return (!vm_addr_align_ok(addr, dmat->common.alignment));
}

static bool
might_bounce(bus_dma_tag_t dmat, bus_dmamap_t map, bus_addr_t paddr,
    bus_size_t size)
{

	/* Memory allocated by bounce_bus_dmamem_alloc won't bounce */
	if (map && (map->flags & DMAMAP_FROM_DMAMEM) != 0)
		return (false);

	/* Bounce if accessing secure memory */
	if (in_realm() && !(paddr & prot_ns_shared_pa))
		return (true);

	if ((dmat->bounce_flags & BF_COULD_BOUNCE) != 0)
		return (true);

	if (cacheline_bounce(dmat, map, paddr, size))
		return (true);

	if (alignment_bounce(dmat, paddr))
		return (true);

	return (false);
}

static bool
must_bounce(bus_dma_tag_t dmat, bus_dmamap_t map, bus_addr_t paddr,
    bus_size_t size)
{

	if (dmat->ram != NULL && size != 0 &&
	    (paddr > dmat_lowaddr(dmat) ||
	    size - 1 > dmat_lowaddr(dmat) - paddr))
		return (true);
	if (cacheline_bounce(dmat, map, paddr, size))
		return (true);

	if ((dmat->bounce_flags & BF_COULD_BOUNCE) != 0 &&
	    addr_needs_bounce(dmat, paddr))
		return (true);

	/* Bounce if accessing secure memory */
	if (in_realm() && !(paddr & prot_ns_shared_pa))
		return (true);

	return (false);
}

static int
ram_ref_acquire(u_int *refs)
{
	u_int old;

	do {
		old = atomic_load_acq_int(refs);
		if (old == UINT_MAX)
			return (EOVERFLOW);
	} while (!atomic_cmpset_int(refs, old, old + 1));
	return (0);
}

/*
 * Allocate a device specific dma_tag.
 */
static int
bounce_bus_dma_tag_create_internal(bus_dma_tag_t parent,
    const struct bcm2712_dma_policy *policy, bool new_domain,
    bus_size_t alignment,
    bus_addr_t boundary, bus_addr_t lowaddr, bus_addr_t highaddr,
    bus_size_t maxsize, int nsegments, bus_size_t maxsegsz, int flags,
    bus_dma_lock_t *lockfunc, void *lockfuncarg, bus_dma_tag_t *dmat)
{
	struct bus_dma_ram_context *ram;
	struct bcm2712_dma_ram view;
	bus_dma_tag_t newtag;
	bus_addr_t parent_limit;
	int error;

	*dmat = NULL;
	ram = NULL;
	if (policy != NULL) {
		if (in_realm() || parent == NULL ||
		    parent->common.impl != &bus_dma_bounce_impl ||
		    parent->common.highaddr != BUS_SPACE_MAXADDR ||
		    highaddr != BUS_SPACE_MAXADDR || alignment == 0 ||
		    alignment > PAGE_SIZE || (alignment & (alignment - 1)) != 0 ||
		    parent->common.alignment == 0 ||
		    parent->common.alignment > PAGE_SIZE ||
		    (parent->common.alignment & (parent->common.alignment - 1)) != 0 ||
		    (boundary != 0 && (boundary > (UINT64_C(1) << 36) ||
		    (boundary & (boundary - 1)) != 0)) ||
		    (parent->common.boundary != 0 &&
		    (parent->common.boundary > (UINT64_C(1) << 36) ||
		    (parent->common.boundary & (parent->common.boundary - 1)) != 0)) ||
		    nsegments <= 0 ||
		    nsegments > 65535 || maxsize == 0 ||
		    maxsize > (bus_size_t)(INT_MAX - 3) * PAGE_SIZE ||
		    maxsize > BUS_SPACE_MAXSIZE - 2 * PAGE_SIZE ||
		    maxsegsz == 0 || (lowaddr & PAGE_MASK) != PAGE_MASK ||
		    (lowaddr != BUS_SPACE_MAXADDR &&
		    (lowaddr & (lowaddr + 1)) != 0) ||
		    (flags & ~(BUS_DMA_ALLOCNOW | BUS_DMA_COHERENT |
		    BUS_DMA_KEEP_PG_OFFSET)) != 0)
			return (EOPNOTSUPP);
		alignment = MAX(alignment, parent->common.alignment);
		if (boundary == 0)
			boundary = parent->common.boundary;
		else if (parent->common.boundary != 0)
			boundary = MIN(boundary, parent->common.boundary);
		parent_limit = dmat_lowaddr(parent);
		/* Same-domain children inherit their parent's device mask. */
		if (!new_domain)
			lowaddr = MIN(lowaddr, parent->common.lowaddr);
		error = bcm2712_dma_ram_select(policy, lowaddr, parent_limit,
		    alignment, boundary, PAGE_SIZE, &view);
		if (error != 0)
			return (error);
		error = ram_ref_acquire(&parent->ram_children);
		if (error != 0)
			return (error);
		if (new_domain) {
			ram = malloc(sizeof(*ram), M_DEVBUF, M_NOWAIT);
			if (ram == NULL) {
				atomic_subtract_int(&parent->ram_children, 1);
				return (ENOMEM);
			}
			ram->policy = *policy;
			ram->refs = 1;
		} else {
			ram = parent->ram;
			error = ram_ref_acquire(&ram->refs);
			if (error != 0) {
				atomic_subtract_int(&parent->ram_children, 1);
				return (error);
			}
		}
	}
	error = common_bus_dma_tag_create(parent != NULL && !new_domain ? &parent->common :
	    NULL, alignment, boundary, lowaddr, highaddr, maxsize, nsegments,
	    maxsegsz, flags, lockfunc, lockfuncarg,
	    sizeof (struct bus_dma_tag), (void **)&newtag);
	if (error != 0) {
		if (ram != NULL) {
			if (atomic_fetchadd_int(&ram->refs, -1) == 1)
				free(ram, M_DEVBUF);
			atomic_subtract_int(&parent->ram_children, 1);
		}
		return (error);
	}
	if (ram != NULL) {
		newtag->ram = ram;
		newtag->ram_view = view;
		newtag->ram_parent = parent;
		newtag->common.domain = vm_phys_domain_match(parent->common.domain,
		    0, view.physical_limit);
		newtag->bounce_flags |= BF_FORCE_MAP;
	}
	newtag->committed_domain = newtag->common.domain;

	newtag->common.impl = &bus_dma_bounce_impl;
	newtag->map_count = 0;
	newtag->segments = NULL;

#ifdef KMSAN
	/* KMSAN stores the memory descriptor in the map. */
	newtag->bounce_flags |= BF_FORCE_MAP;
#endif

	if ((flags & BUS_DMA_COHERENT) != 0) {
		newtag->bounce_flags |= BF_COHERENT;
	}

	if (parent != NULL) {
		if ((parent->bounce_flags & BF_COULD_BOUNCE) != 0)
			newtag->bounce_flags |= BF_COULD_BOUNCE;

		/* Copy some flags from the parent */
		newtag->bounce_flags |= parent->bounce_flags & BF_COHERENT;
	}

	if ((newtag->bounce_flags & BF_COHERENT) != 0) {
		newtag->alloc_alignment = newtag->common.alignment;
		newtag->alloc_size = newtag->common.maxsize;
	} else {
		/*
		 * Ensure the buffer is aligned to a cacheline when allocating
		 * a non-coherent buffer. This is so we don't have any data
		 * that another CPU may be accessing around DMA buffer
		 * causing the cache to become dirty.
		 */
		newtag->alloc_alignment = MAX(newtag->common.alignment,
		    dcache_line_size);
		newtag->alloc_size = roundup2(newtag->common.maxsize,
		    dcache_line_size);
	}

	if (dmat_lowaddr(newtag) < ptoa((vm_paddr_t)Maxmem) ||
	    newtag->common.alignment > 1)
		newtag->bounce_flags |= BF_COULD_BOUNCE;

	if ((flags & BUS_DMA_ALLOCNOW) != 0)
		error = bounce_bus_dma_zone_setup(newtag);
	else
		error = 0;

	if (error != 0) {
		if (ram != NULL) {
			if (atomic_fetchadd_int(&ram->refs, -1) == 1)
				free(ram, M_DEVBUF);
			atomic_subtract_int(&parent->ram_children, 1);
		}
		free(newtag, M_DEVBUF);
		newtag = NULL;
	} else {
		*dmat = newtag;
	}
	CTR4(KTR_BUSDMA, "%s returned tag %p tag flags 0x%x error %d",
	    __func__, newtag, (newtag != NULL ? newtag->common.flags : 0),
	    error);
	return (error);
}

static int
bounce_bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment,
    bus_addr_t boundary, bus_addr_t lowaddr, bus_addr_t highaddr,
    bus_size_t maxsize, int nsegments, bus_size_t maxsegsz, int flags,
    bus_dma_lock_t *lockfunc, void *lockfuncarg, bus_dma_tag_t *dmat)
{
	return (bounce_bus_dma_tag_create_internal(parent,
	    parent != NULL && parent->ram != NULL ? &parent->ram->policy : NULL,
	    false, alignment, boundary, lowaddr, highaddr, maxsize, nsegments,
	    maxsegsz, flags, lockfunc, lockfuncarg, dmat));
}

int
bus_dma_tag_create_bcm2712_ram(bus_dma_tag_t parent,
    const struct bcm2712_dma_policy *policy, bus_size_t alignment,
    bus_addr_t boundary, bus_addr_t lowaddr, bus_addr_t highaddr,
    bus_size_t maxsize, int nsegments, bus_size_t maxsegsz, int flags,
    bus_dma_lock_t *lockfunc, void *lockfuncarg, bus_dma_tag_t *dmat)
{
	if (dmat == NULL)
		return (EINVAL);
	*dmat = NULL;
	/* Check the common prefix before inspecting bounce-private fields. */
	if (policy == NULL || parent == NULL ||
	    ((struct bus_dma_tag_common *)parent)->impl != &bus_dma_bounce_impl)
		return (EOPNOTSUPP);
	return (bounce_bus_dma_tag_create_internal(parent, policy, true,
	    alignment, boundary, lowaddr, highaddr, maxsize, nsegments,
	    maxsegsz, flags, lockfunc, lockfuncarg, dmat));
}

bus_addr_t
bus_dma_bounce_domain_limit(bus_dma_tag_t dmat)
{
	return (dmat_lowaddr(dmat));
}

bool
bus_dma_bounce_ram_translated(bus_dma_tag_t dmat)
{
	return (dmat->ram != NULL);
}

static int
bounce_bus_dma_tag_destroy(bus_dma_tag_t dmat)
{
	int error = 0;

	if (dmat != NULL) {
		if (dmat->map_count != 0 ||
		    atomic_load_acq_int(&dmat->ram_children) != 0) {
			error = EBUSY;
			goto out;
		}
		if (dmat->segments != NULL)
			free(dmat->segments, M_DEVBUF);
		if (dmat->ram != NULL) {
			if (atomic_fetchadd_int(&dmat->ram->refs, -1) == 1)
				free(dmat->ram, M_DEVBUF);
			atomic_subtract_int(&dmat->ram_parent->ram_children, 1);
		}
		free(dmat, M_DEVBUF);
	}
out:
	CTR3(KTR_BUSDMA, "%s tag %p error %d", __func__, dmat, error);
	return (error);
}

/*
 * Update the domain for the tag.  We may need to reallocate the zone and
 * bounce pages.
 */
static int
bounce_bus_dma_tag_set_domain(bus_dma_tag_t dmat)
{
	struct bounce_zone *oldzone;
	int error, oldflags;

	if (dmat->ram != NULL || atomic_load_acq_int(&dmat->ram_children) != 0) {
		if (dmat->map_count != 0 ||
		    atomic_load_acq_int(&dmat->ram_children) != 0) {
			dmat->common.domain = dmat->committed_domain;
			return (EBUSY);
		}
		dmat->common.domain = vm_phys_domain_match(dmat->common.domain,
		    0, dmat_lowaddr(dmat));
		oldzone = dmat->bounce_zone;
		oldflags = dmat->bounce_flags;
		dmat->bounce_zone = NULL;
		dmat->bounce_flags &= ~BF_MIN_ALLOC_COMP;
		error = oldzone != NULL ? bounce_bus_dma_zone_setup(dmat) : 0;
		if (error != 0) {
			dmat->common.domain = dmat->committed_domain;
			dmat->bounce_zone = oldzone;
			dmat->bounce_flags = oldflags;
		} else
			dmat->committed_domain = dmat->common.domain;
		return (error);
	}
	KASSERT(dmat->map_count == 0,
	    ("bounce_bus_dma_tag_set_domain: Domain set after use.\n"));
	if ((dmat->bounce_flags & BF_COULD_BOUNCE) == 0 ||
	    dmat->bounce_zone == NULL) {
		dmat->committed_domain = dmat->common.domain;
		return (0);
	}
	dmat->bounce_flags &= ~BF_MIN_ALLOC_COMP;
	error = bounce_bus_dma_zone_setup(dmat);
	if (error == 0)
		dmat->committed_domain = dmat->common.domain;
	return (error);
}

static bool
bounce_bus_dma_id_mapped(bus_dma_tag_t dmat, vm_paddr_t buf, bus_size_t buflen)
{

	if (dmat->ram != NULL && dmat->ram_view.offset != 0)
		return (false);
	if (!might_bounce(dmat, NULL, buf, buflen))
		return (true);
	return (!_bus_dmamap_pagesneeded(dmat, NULL, buf, buflen, NULL));
}

static bus_dmamap_t
alloc_dmamap(bus_dma_tag_t dmat, int flags)
{
	u_long mapsize;
	bus_dmamap_t map;

	mapsize = sizeof(*map);
	mapsize += sizeof(struct sync_list) * dmat->common.nsegments;
	map = malloc_domainset(mapsize, M_DEVBUF,
	    DOMAINSET_PREF(dmat->common.domain), flags | M_ZERO);
	if (map == NULL)
		return (NULL);

	/* Initialize the new map */
	STAILQ_INIT(&map->bpages);
	if (dmat->ram != NULL)
		map->dmat = dmat;

	return (map);
}

/*
 * Allocate a handle for mapping from kva/uva/physical
 * address space into bus device space.
 */
static int
bounce_bus_dmamap_create(bus_dma_tag_t dmat, int flags, bus_dmamap_t *mapp)
{
	struct bounce_zone *bz;
	int error, maxpages, pages;

	error = 0;
	if (dmat->ram != NULL)
		*mapp = NULL;

	if (dmat->segments == NULL) {
		dmat->segments = mallocarray_domainset(dmat->common.nsegments,
		    sizeof(bus_dma_segment_t), M_DEVBUF,
		    DOMAINSET_PREF(dmat->common.domain), M_NOWAIT);
		if (dmat->segments == NULL) {
			CTR3(KTR_BUSDMA, "%s: tag %p error %d",
			    __func__, dmat, ENOMEM);
			return (ENOMEM);
		}
	}

	if ((dmat->bounce_flags &
	    (BF_COULD_BOUNCE | BF_COHERENT | BF_FORCE_MAP)) == BF_COHERENT &&
	    !in_realm()) {
		*mapp = NULL;
		goto out;
	}
	*mapp = alloc_dmamap(dmat, M_NOWAIT);
	if (*mapp == NULL) {
		CTR3(KTR_BUSDMA, "%s: tag %p error %d",
		    __func__, dmat, ENOMEM);
		return (ENOMEM);
	}

	/*
	 * Bouncing might be required if the driver asks for an active
	 * exclusion region, a data alignment that is stricter than 1, and/or
	 * an active address boundary.
	 */
	if (dmat->bounce_zone == NULL) {
		if ((error = alloc_bounce_zone(dmat)) != 0) {
			free(*mapp, M_DEVBUF);
			if (dmat->ram != NULL)
				*mapp = NULL;
			return (error);
		}
	}
	bz = dmat->bounce_zone;

	/*
	 * Attempt to add pages to our pool on a per-instance basis up to a sane
	 * limit. Even if the tag isn't subject of bouncing due to alignment
	 * and boundary constraints, it could still auto-bounce due to
	 * cacheline alignment, which requires at most two bounce pages.
	 */
	if (dmat->ram != NULL || dmat->common.alignment > 1)
		maxpages = MAX_BPAGES;
	else
		maxpages = MIN(MAX_BPAGES, Maxmem -
		    atop(dmat_lowaddr(dmat)));
	if ((dmat->bounce_flags & BF_MIN_ALLOC_COMP) == 0 ||
	    (bz->map_count > 0 && bz->total_bpages < maxpages)) {
		pages = atop(roundup2(dmat->common.maxsize, PAGE_SIZE)) + 1;
		pages = MIN(maxpages - bz->total_bpages, pages);
		pages = MAX(pages, 2);
		if (alloc_bounce_pages(dmat, pages) < pages)
			error = ENOMEM;
		if ((dmat->bounce_flags & BF_MIN_ALLOC_COMP) == 0) {
			if (error == 0) {
				dmat->bounce_flags |= BF_MIN_ALLOC_COMP;
			}
		} else
			error = 0;
	}
	if (error == 0 || dmat->ram == NULL)
		bz->map_count++;

out:
	if (error == 0) {
		dmat->map_count++;
		if (*mapp != NULL &&
		    (dmat->bounce_flags & BF_COHERENT) != 0)
			(*mapp)->flags |= DMAMAP_COHERENT;
	} else {
		free(*mapp, M_DEVBUF);
		if (dmat->ram != NULL)
			*mapp = NULL;
	}
	CTR4(KTR_BUSDMA, "%s: tag %p tag flags 0x%x error %d",
	    __func__, dmat, dmat->common.flags, error);
	return (error);
}

/*
 * Destroy a handle for mapping from kva/uva/physical
 * address space into bus device space.
 */
static int
bounce_bus_dmamap_destroy(bus_dma_tag_t dmat, bus_dmamap_t map)
{

	if (dmat->ram != NULL && (map == NULL || map->dmat != dmat))
		return (EINVAL);
	if (map != NULL && map != &nobounce_dmamap) {
		if ((map->flags & DMAMAP_FROM_DMAMEM) != 0)
			panic("bounce_bus_dmamap_destroy: Invalid map freed\n");
		if (STAILQ_FIRST(&map->bpages) != NULL ||
		    map->sync_count != 0 || (dmat->ram != NULL &&
		    ((atomic_load_acq_int(&map->flags) &
		    (DMAMAP_PENDING | DMAMAP_LOADED)) != 0 ||
		    map->pagesreserved != 0))) {
			CTR3(KTR_BUSDMA, "%s: tag %p error %d", __func__,
			    dmat, EBUSY);
			return (EBUSY);
		}
		if (dmat->bounce_zone != NULL)
			dmat->bounce_zone->map_count--;
		free(map, M_DEVBUF);
	}
	dmat->map_count--;
	CTR2(KTR_BUSDMA, "%s: tag %p error 0", __func__, dmat);
	return (0);
}

/*
 * Allocate a piece of memory that can be efficiently mapped into
 * bus device space based on the constraints lited in the dma tag.
 * A dmamap to for use with dmamap_load is also allocated.
 */
static int
bounce_bus_dmamem_alloc(bus_dma_tag_t dmat, void** vaddr, int flags,
    bus_dmamap_t *mapp)
{
	struct busdma_bufzone *bufzone;
	vm_memattr_t attr;
	int mflags;

	if (dmat->ram != NULL) {
		*vaddr = NULL;
		*mapp = NULL;
	}
	if (flags & BUS_DMA_NOWAIT)
		mflags = M_NOWAIT;
	else
		mflags = M_WAITOK;

	if (dmat->segments == NULL) {
		dmat->segments = mallocarray_domainset(dmat->common.nsegments,
		    sizeof(bus_dma_segment_t), M_DEVBUF,
		    DOMAINSET_PREF(dmat->common.domain), mflags);
		if (dmat->segments == NULL) {
			CTR4(KTR_BUSDMA, "%s: tag %p tag flags 0x%x error %d",
			    __func__, dmat, dmat->common.flags, ENOMEM);
			return (ENOMEM);
		}
	}
	if (flags & BUS_DMA_ZERO)
		mflags |= M_ZERO;
	if (flags & BUS_DMA_NOCACHE)
		attr = VM_MEMATTR_UNCACHEABLE;
	else if ((flags & BUS_DMA_COHERENT) != 0 &&
	    (dmat->bounce_flags & BF_COHERENT) == 0)
		/*
		 * If we have a non-coherent tag, and are trying to allocate
		 * a coherent block of memory it needs to be uncached.
		 */
		attr = VM_MEMATTR_UNCACHEABLE;
	else
		attr = VM_MEMATTR_DEFAULT;

	if (in_realm())
		mflags |= M_UNPROTECTED;

	/*
	 * Create the map, but don't set the could bounce flag as
	 * this allocation should never bounce;
	 */
	*mapp = alloc_dmamap(dmat, mflags);
	if (*mapp == NULL) {
		CTR4(KTR_BUSDMA, "%s: tag %p tag flags 0x%x error %d",
		    __func__, dmat, dmat->common.flags, ENOMEM);
		return (ENOMEM);
	}

	/*
	 * Mark the map as coherent if we used uncacheable memory or the
	 * tag was already marked as coherent.
	 */
	if (attr == VM_MEMATTR_UNCACHEABLE ||
	    (dmat->bounce_flags & BF_COHERENT) != 0)
		(*mapp)->flags |= DMAMAP_COHERENT;

	(*mapp)->flags |= DMAMAP_FROM_DMAMEM;

	/*
	 * Allocate the buffer from the malloc(9) allocator if...
	 *  - It's small enough to fit into a single page.
	 *  - Its alignment requirement is also smaller than the page size.
	 *  - The low address requirement is fulfilled.
	 *  - Default cache attributes are requested (WB).
	 * else allocate non-contiguous pages if...
	 *  - The page count that could get allocated doesn't exceed
	 *    nsegments also when the maximum segment size is less
	 *    than PAGE_SIZE.
	 *  - The alignment constraint isn't larger than a page boundary.
	 *  - There are no boundary-crossing constraints.
	 * else allocate a block of contiguous pages because one or more of the
	 * constraints is something that only the contig allocator can fulfill.
	 *
	 * NOTE: The (dmat->common.alignment <= dmat->maxsize) check
	 * below is just a quick hack. The exact alignment guarantees
	 * of malloc(9) need to be nailed down, and the code below
	 * should be rewritten to take that into account.
	 *
	 * In the meantime warn the user if malloc gets it wrong.
	 */

	bufzone = busdma_bufalloc_findzone(nonsecure_allocator,
	    dmat->alloc_size);

	if (bufzone &&
	    dmat->alloc_size <= PAGE_SIZE &&
	    dmat->alloc_alignment <= bufzone->size &&
	    dmat_lowaddr(dmat) >= ptoa((vm_paddr_t)Maxmem) &&
	    attr == VM_MEMATTR_DEFAULT) {
		*vaddr = uma_zalloc(bufzone->umazone, mflags);
	} else if (dmat->common.nsegments >=
	    howmany(dmat->alloc_size, MIN(dmat->common.maxsegsz, PAGE_SIZE)) &&
	    dmat->alloc_alignment <= PAGE_SIZE &&
	    (dmat->common.boundary % PAGE_SIZE) == 0) {
		/* Page-based multi-segment allocations allowed */
		*vaddr = kmem_alloc_attr_domainset(
		    DOMAINSET_PREF(dmat->common.domain), dmat->alloc_size,
		    mflags, 0ul, dmat_lowaddr(dmat), attr);
		dmat->bounce_flags |= BF_KMEM_ALLOC_PAGES;
		(*mapp)->ram_alloc_flags = BF_KMEM_ALLOC_PAGES;
	} else {
		*vaddr = kmem_alloc_contig_domainset(
		    DOMAINSET_PREF(dmat->common.domain), dmat->alloc_size,
		    mflags, 0ul, dmat_lowaddr(dmat),
		    dmat->alloc_alignment != 0 ? dmat->alloc_alignment : 1ul,
		    dmat->common.boundary, attr);
		dmat->bounce_flags |= BF_KMEM_ALLOC_CONTIG;
		(*mapp)->ram_alloc_flags = BF_KMEM_ALLOC_CONTIG;
	}
	if (*vaddr == NULL) {
		CTR4(KTR_BUSDMA, "%s: tag %p tag flags 0x%x error %d",
		    __func__, dmat, dmat->common.flags, ENOMEM);
		free(*mapp, M_DEVBUF);
		if (dmat->ram != NULL)
			*mapp = NULL;
		return (ENOMEM);
	} else if (!vm_addr_align_ok(vtophys(*vaddr), dmat->alloc_alignment)) {
		printf("bus_dmamem_alloc failed to align memory properly.\n");
	}
	dmat->map_count++;
	CTR4(KTR_BUSDMA, "%s: tag %p tag flags 0x%x error %d",
	    __func__, dmat, dmat->common.flags, 0);
	return (0);
}

/*
 * Free a piece of memory and it's allociated dmamap, that was allocated
 * via bus_dmamem_alloc.
 */
static void
bounce_bus_dmamem_free(bus_dma_tag_t dmat, void *vaddr, bus_dmamap_t map)
{

	/*
	 * Check the map came from bounce_bus_dmamem_alloc, so the map
	 * should be NULL and the BF_KMEM_ALLOC flag cleared if malloc()
	 * was used and set if kmem_alloc_contig() was used.
	 */
	if ((map->flags & DMAMAP_FROM_DMAMEM) == 0)
		panic("bus_dmamem_free: Invalid map freed\n");
	if (dmat->ram != NULL && (map->dmat != dmat ||
	    (atomic_load_acq_int(&map->flags) &
	    (DMAMAP_PENDING | DMAMAP_LOADED)) != 0 ||
	    !STAILQ_EMPTY(&map->bpages) || map->pagesreserved != 0)) {
		printf("busdma: translated memory still owned; unload and retry free\n");
		return;
	}
	if ((dmat->ram != NULL ? map->ram_alloc_flags :
	    dmat->bounce_flags & BF_KMEM_ALLOC) == 0)
		free(vaddr, M_DEVBUF);
	else
		kmem_free(vaddr, dmat->alloc_size);
	free(map, M_DEVBUF);
	dmat->map_count--;
	CTR3(KTR_BUSDMA, "%s: tag %p flags 0x%x", __func__, dmat,
	    dmat->bounce_flags);
}

static bool
_bus_dmamap_pagesneeded(bus_dma_tag_t dmat, bus_dmamap_t map, vm_paddr_t buf,
    bus_size_t buflen, int *pagesneeded)
{
	bus_addr_t curaddr;
	bus_size_t sgsize;
	int count;

	/*
	 * Count the number of bounce pages needed in order to
	 * complete this transfer
	 */
	count = 0;
	curaddr = buf;
	while (buflen != 0) {
		sgsize = buflen;
		if (must_bounce(dmat, map, curaddr, sgsize)) {
			sgsize = MIN(sgsize,
			    PAGE_SIZE - (curaddr & PAGE_MASK));
			if (pagesneeded == NULL)
				return (true);
			count++;
		}
		curaddr += sgsize;
		buflen -= sgsize;
	}

	if (pagesneeded != NULL)
		*pagesneeded = count;
	return (count != 0);
}

static void
_bus_dmamap_count_phys(bus_dma_tag_t dmat, bus_dmamap_t map, vm_paddr_t buf,
    bus_size_t buflen, int flags)
{

	if (map->pagesneeded == 0) {
		_bus_dmamap_pagesneeded(dmat, map, buf, buflen,
		    &map->pagesneeded);
		CTR1(KTR_BUSDMA, "pagesneeded= %d\n", map->pagesneeded);
	}
}

static void
_bus_dmamap_count_pages(bus_dma_tag_t dmat, bus_dmamap_t map, pmap_t pmap,
    void *buf, bus_size_t buflen, int flags)
{
	vm_offset_t vaddr;
	vm_offset_t vendaddr;
	bus_addr_t paddr;
	bus_size_t sg_len;

	if (map->pagesneeded == 0) {
		CTR4(KTR_BUSDMA, "lowaddr= %d Maxmem= %d, boundary= %d, "
		    "alignment= %d", dmat->common.lowaddr,
		    ptoa((vm_paddr_t)Maxmem),
		    dmat->common.boundary, dmat->common.alignment);
		CTR2(KTR_BUSDMA, "map= %p, pagesneeded= %d", map,
		    map->pagesneeded);
		/*
		 * Count the number of bounce pages
		 * needed in order to complete this transfer
		 */
		vaddr = (vm_offset_t)buf;
		vendaddr = (vm_offset_t)buf + buflen;

		while (vaddr < vendaddr) {
			sg_len = MIN(vendaddr - vaddr,
			    PAGE_SIZE - ((vm_offset_t)vaddr & PAGE_MASK));
			if (pmap == kernel_pmap)
				paddr = pmap_kextract(vaddr);
			else
				paddr = pmap_extract(pmap, vaddr);
			if (must_bounce(dmat, map, paddr, sg_len) != 0) {
				sg_len = roundup2(sg_len,
				    dmat->common.alignment);
				map->pagesneeded++;
			}
			vaddr += sg_len;
		}
		CTR1(KTR_BUSDMA, "pagesneeded= %d\n", map->pagesneeded);
	}
}

/* Check once per MI chunk.  WAITOK preparation visits exactly the same
 * MI traversal with COUNTING set; it cannot allocate pages, publish segments,
 * perform cache operations or invoke a driver callback.  All bounce pages
 * are then reserved together before the first real chunk is mapped.
 */
static int
ram_load_check(bus_dma_tag_t dmat, bus_dmamap_t map, bus_size_t length,
    int seg)
{
	if (map == NULL || map->dmat != dmat)
		return (EINVAL);
	if (ram_map_busy(map) || (map->flags & DMAMAP_LOADED) != 0)
		return (EBUSY);
	if ((map->flags & DMAMAP_PREPARED) != 0)
		return (map->ram_error);
	if (seg == -1 && (map->flags & DMAMAP_COUNTING) == 0)
		map->ram_bytes = 0;
	if (length > dmat->common.maxsize - map->ram_bytes)
		return (EFBIG);
	map->ram_bytes += length;
	return (0);
}

/* A RAM tag cannot turn a peer/MMIO source into ordinary bounced RAM.
 * As with the ordinary API, callers must keep their virtual mappings/pages
 * stable for the lifetime of a load.  Every physical source must have a
 * managed RAM page, independently of the destination bounce preimage.
 */
static int
ram_source_check(bus_dma_tag_t dmat, vm_paddr_t physical, bus_size_t length)
{
	const struct bcm2712_dma_range *range;
	vm_page_t page;
	bus_size_t chunk;
	unsigned i;

	if (length == 0)
		return (0);
	if (length - 1 > BUS_SPACE_MAXADDR - physical)
		return (EINVAL);
	for (i = 0; i < dmat->ram->policy.count; i++) {
		range = &dmat->ram->policy.ranges[i];
		if (range->role == BCM2712_DMA_PEER &&
		    physical <= range->cpu_base + range->length - 1 &&
		    range->cpu_base <= physical + length - 1)
			return (ENXIO);
	}
	while (length != 0) {
		page = vm_phys_paddr_to_vm_page(physical);
		if (page == NULL || VM_PAGE_TO_PHYS(page) != trunc_page(physical))
			return (ENXIO);
		chunk = MIN(length, PAGE_SIZE - (physical & PAGE_MASK));
		length -= chunk;
		if (length != 0)
			physical += chunk;
	}
	return (0);
}

static int
ram_count_phys(bus_dma_tag_t dmat, bus_dmamap_t map, vm_paddr_t physical,
    bus_size_t length)
{
	bus_size_t chunk;
	int error;

	error = ram_source_check(dmat, physical, length);
	if (error != 0)
		return (error);
	while (length != 0) {
		chunk = length;
		if (must_bounce(dmat, map, physical, chunk)) {
			chunk = MIN(chunk, PAGE_SIZE - (physical & PAGE_MASK));
			if (map->pagesneeded == INT_MAX)
				return (EFBIG);
			map->pagesneeded++;
		}
		length -= chunk;
		if (length != 0)
			physical += chunk;
	}
	return (0);
}

static int
ram_count_buffer(bus_dma_tag_t dmat, bus_dmamap_t map, void *buffer,
    bus_size_t length, pmap_t pmap)
{
	vm_offset_t virtual;
	vm_paddr_t physical;
	bus_size_t chunk;
	int error;

	virtual = (vm_offset_t)buffer;
	if (length != 0 && length - 1 > UINTPTR_MAX - virtual)
		return (EINVAL);
	while (length != 0) {
		physical = pmap == kernel_pmap ? pmap_kextract(virtual) :
		    pmap_extract(pmap, virtual);
		chunk = MIN(length, PAGE_SIZE - (physical & PAGE_MASK));
		error = ram_count_phys(dmat, map, physical, chunk);
		if (error != 0)
			return (error);
		length -= chunk;
		if (length != 0)
			virtual += chunk;
	}
	return (0);
}

static int
ram_validate_buffer(bus_dma_tag_t dmat, void *buffer, bus_size_t length,
    pmap_t pmap)
{
	vm_offset_t virtual;
	vm_paddr_t physical;
	bus_size_t chunk;
	int error;

	virtual = (vm_offset_t)buffer;
	if (length != 0 && length - 1 > UINTPTR_MAX - virtual)
		return (EINVAL);
	while (length != 0) {
		physical = pmap == kernel_pmap ? pmap_kextract(virtual) :
		    pmap_extract(pmap, virtual);
		chunk = MIN(length, PAGE_SIZE - (physical & PAGE_MASK));
		error = ram_source_check(dmat, physical, chunk);
		if (error != 0)
			return (error);
		length -= chunk;
		if (length != 0)
			virtual += chunk;
	}
	return (0);
}

/*
 * Utility function to load a physical buffer.  segp contains
 * the starting segment on entrace, and the ending segment on exit.
 */
static int
bounce_bus_dmamap_load_phys(bus_dma_tag_t dmat, bus_dmamap_t map,
    vm_paddr_t buf, bus_size_t buflen, int flags, bus_dma_segment_t *segs,
    int *segp)
{
	struct sync_list *sl;
	bus_size_t sgsize;
	bus_addr_t curaddr, sl_end;
	int error;

	if (dmat->ram != NULL) {
		error = ram_load_check(dmat, map, buflen, *segp);
		if (error != 0)
			return (error);
		error = ram_source_check(dmat, buf, buflen);
		if (error != 0)
			return (error);
		if ((map->flags & DMAMAP_COUNTING) != 0)
			return (ram_count_phys(dmat, map, buf, buflen));
	}
	if (segs == NULL)
		segs = dmat->segments;

	if (map == NULL) {
		KASSERT((dmat->bounce_flags &
		    (BF_COULD_BOUNCE | BF_COHERENT | BF_FORCE_MAP)) ==
		    BF_COHERENT && !in_realm(),
		    ("%s: invalid NULL map for tag %p", __func__, dmat));
		map = &nobounce_dmamap;
		if (!bounce_bus_dmamap_addsegs(dmat, map, buf, buflen, segs, segp)) {
			bus_dmamap_unload(dmat, map);
			return (EFBIG); /* XXX better return value here? */
		}
		return (0);
	}
	sl = map->sync_count != 0 ?
	    map->slist + map->sync_count - 1 : NULL;

	if (might_bounce(dmat, map, (bus_addr_t)buf, buflen) ||
	    (dmat->ram != NULL && (map->flags & DMAMAP_PREPARED) != 0)) {
		if (dmat->ram == NULL || (map->flags & DMAMAP_PREPARED) == 0)
			_bus_dmamap_count_phys(dmat, map, buf, buflen, flags);
		if (map->pagesneeded != 0) {
			error = _bus_dmamap_reserve_pages(dmat, map, flags);
			if (error)
				return (error);
		}
	}

	sl_end = 0;

	while (buflen > 0) {
		curaddr = buf;
		sgsize = buflen;
		if (map->pagesneeded != 0 &&
		    must_bounce(dmat, map, curaddr, sgsize)) {
			/*
			 * The attempt to split a physically continuous buffer
			 * seems very controversial, it's unclear whether we
			 * can do this in all cases. Also, memory for bounced
			 * buffers is allocated as pages, so we cannot
			 * guarantee multipage alignment.
			 */
			KASSERT(dmat->common.alignment <= PAGE_SIZE,
			    ("bounced buffer cannot have alignment bigger "
			    "than PAGE_SIZE: %lu", dmat->common.alignment));
			sgsize = MIN(sgsize, PAGE_SIZE - (curaddr & PAGE_MASK));
			curaddr = add_bounce_page(dmat, map, 0, curaddr,
			    sgsize);
		} else if ((map->flags & DMAMAP_COHERENT) == 0) {
			if (map->sync_count > 0)
				sl_end = sl->paddr + sl->datacount;

			if (map->sync_count == 0 || curaddr != sl_end) {
				if (++map->sync_count > dmat->common.nsegments)
					break;
				sl = map->slist + map->sync_count - 1;
				sl->vaddr = NULL;
				sl->paddr = curaddr;
				sl->pages = PHYS_TO_VM_PAGE(curaddr);
				KASSERT(sl->pages != NULL,
				    ("%s: page at PA:0x%08lx is not in "
				    "vm_page_array", __func__, curaddr));
				sl->datacount = sgsize;
			} else
				sl->datacount += sgsize;
		}
		if (!bounce_bus_dmamap_addsegs(dmat, map, curaddr, sgsize, segs,
		    segp))
			break;
		buf += sgsize;
		buflen -= sgsize;
	}

	/*
	 * Did we fit?
	 */
	if (buflen != 0) {
		bus_dmamap_unload(dmat, map);
		return (EFBIG); /* XXX better return value here? */
	}
	return (0);
}

/*
 * Utility function to load a linear buffer.  segp contains
 * the starting segment on entrace, and the ending segment on exit.
 */
static int
bounce_bus_dmamap_load_buffer(bus_dma_tag_t dmat, bus_dmamap_t map, void *buf,
    bus_size_t buflen, pmap_t pmap, int flags, bus_dma_segment_t *segs,
    int *segp)
{
	struct sync_list *sl;
	bus_size_t sgsize;
	bus_addr_t curaddr, sl_pend;
	char *kvaddr, *vaddr, *sl_vend;
	int error;

	if (dmat->ram != NULL) {
		error = ram_load_check(dmat, map, buflen, *segp);
		if (error != 0)
			return (error);
		error = ram_validate_buffer(dmat, buf, buflen, pmap);
		if (error != 0)
			return (error);
		if ((flags & BUS_DMA_LOAD_MBUF) != 0)
			map->flags |= DMAMAP_MBUF;
		if ((map->flags & DMAMAP_COUNTING) != 0)
			return (ram_count_buffer(dmat, map, buf, buflen, pmap));
	}
	if (map == NULL) {
		KASSERT((dmat->bounce_flags &
		    (BF_COULD_BOUNCE | BF_COHERENT | BF_FORCE_MAP)) ==
		    BF_COHERENT && !in_realm(),
		    ("%s: invalid NULL map for tag %p", __func__, dmat));
		map = &nobounce_dmamap;
		sl = NULL;
	} else {
		sl = map->sync_count != 0 ?
	    map->slist + map->sync_count - 1 : NULL;
	}

	KASSERT((map->flags & DMAMAP_FROM_DMAMEM) != 0 ||
	    dmat->common.alignment <= PAGE_SIZE,
	    ("loading user buffer with alignment bigger than PAGE_SIZE is not "
	    "supported"));

	if (segs == NULL)
		segs = dmat->segments;

	if ((flags & BUS_DMA_LOAD_MBUF) != 0 && map != &nobounce_dmamap)
		map->flags |= DMAMAP_MBUF;

	if (might_bounce(dmat, map, (bus_addr_t)buf, buflen) ||
	    (dmat->ram != NULL && (map->flags & DMAMAP_PREPARED) != 0)) {
		if (dmat->ram == NULL || (map->flags & DMAMAP_PREPARED) == 0)
			_bus_dmamap_count_pages(dmat, map, pmap, buf, buflen, flags);
		if (map->pagesneeded != 0) {
			error = _bus_dmamap_reserve_pages(dmat, map, flags);
			if (error)
				return (error);
		}
	}

	/*
	 * XXX Optimally we should parse input buffer for physically
	 * continuous segments first and then pass these segment into
	 * load loop.
	 */
	vaddr = buf;
	sl_pend = 0;
	sl_vend = NULL;

	while (buflen > 0) {
		/*
		 * Get the physical address for this segment.
		 */
		if (__predict_true(pmap == kernel_pmap)) {
			curaddr = pmap_kextract((vm_offset_t)vaddr);
			kvaddr = vaddr;
		} else {
			curaddr = pmap_extract(pmap, (vm_offset_t)vaddr);
			kvaddr = NULL;
		}

		/*
		 * Compute the segment size, and adjust counts.
		 */
		sgsize = buflen;
		if ((map->flags & DMAMAP_FROM_DMAMEM) == 0 ||
		    ((dmat->ram != NULL ? map->ram_alloc_flags : dmat->bounce_flags) &
		    BF_KMEM_ALLOC_CONTIG) == 0)
			sgsize = MIN(sgsize, PAGE_SIZE - (curaddr & PAGE_MASK));

		if (map->pagesneeded != 0 &&
		    must_bounce(dmat, map, curaddr, sgsize)) {
			/* See comment in bounce_bus_dmamap_load_phys */
			KASSERT(dmat->common.alignment <= PAGE_SIZE,
			    ("bounced buffer cannot have alignment bigger "
			    "than PAGE_SIZE: %lu", dmat->common.alignment));
			curaddr = add_bounce_page(dmat, map, kvaddr, curaddr,
			    sgsize);
		} else if ((map->flags & DMAMAP_COHERENT) == 0) {
			if (map->sync_count > 0) {
				sl_pend = sl->paddr + sl->datacount;
				sl_vend = sl->vaddr + sl->datacount;
			}

			if (map->sync_count == 0 ||
			    (kvaddr != 0 && kvaddr != sl_vend) ||
			    (curaddr != sl_pend)) {
				if (++map->sync_count > dmat->common.nsegments)
					break;
				sl = map->slist + map->sync_count - 1;
				sl->vaddr = kvaddr;
				sl->paddr = curaddr;
				if (kvaddr != NULL) {
					sl->pages = NULL;
				} else {
					sl->pages = PHYS_TO_VM_PAGE(curaddr);
					KASSERT(sl->pages != NULL,
					    ("%s: page at PA:0x%08lx is not "
					    "in vm_page_array", __func__,
					    curaddr));
				}
				sl->datacount = sgsize;
			} else
				sl->datacount += sgsize;
		}
		if (!bounce_bus_dmamap_addsegs(dmat, map, curaddr, sgsize, segs,
		    segp))
			break;
		vaddr += sgsize;
		buflen -= MIN(sgsize, buflen); /* avoid underflow */
	}

	/*
	 * Did we fit?
	 */
	if (buflen != 0) {
		bus_dmamap_unload(dmat, map);
		return (EFBIG); /* XXX better return value here? */
	}
	return (0);
}

static void
ram_counted(void *arg, bus_dma_segment_t *segs __unused,
    int nsegs __unused, int error)
{
	((bus_dmamap_t)arg)->ram_error = error;
}

static void
bounce_bus_dmamap_waitok(bus_dma_tag_t dmat, bus_dmamap_t map,
    struct memdesc *mem, bus_dmamap_callback_t *callback, void *callback_arg)
{

	if (dmat->ram != NULL) {
		if (map->dmat != dmat || ram_map_busy(map) ||
		    (map->flags & DMAMAP_LOADED) != 0)
			return;
		if ((map->flags & DMAMAP_REPLAY) != 0)
			return;
		map->pagesneeded = 0;
		map->ram_bytes = 0;
		map->ram_error = 0;
		map->flags |= DMAMAP_COUNTING;
		(void)bus_dmamap_load_mem(dmat, map, mem, ram_counted, map,
		    BUS_DMA_NOWAIT);
		map->flags &= ~DMAMAP_COUNTING;
		map->flags |= DMAMAP_PREPARED;
	}
	map->mem = *mem;
	map->dmat = dmat;
	map->callback = callback;
	map->callback_arg = callback_arg;
}

static bus_dma_segment_t *
bounce_bus_dmamap_complete(bus_dma_tag_t dmat, bus_dmamap_t map,
    bus_dma_segment_t *segs, int nsegs, int error)
{

	if (dmat->ram != NULL && map != NULL && map->dmat == dmat &&
	    (map->flags & DMAMAP_COUNTING) == 0 && error != EBUSY) {
		if (error != 0)
			bus_dmamap_unload(dmat, map);
		else
			map->flags |= DMAMAP_LOADED;
		map->flags &= ~(DMAMAP_PREPARED | DMAMAP_REPLAY);
		/* The deferred worker saves its unlock callback before load,
		 * and never dereferences this map/tag after the user callback.
		 * Thus callback-time unload/destroy and post-unlock cleanup are
		 * both permitted; no externally visible post-unlock busy gap. */
		atomic_clear_int(&map->flags, DMAMAP_PENDING);
	}
	if (segs == NULL)
		segs = dmat->segments;
	return (segs);
}

/*
 * Release the mapping held by map.
 */
static void
bounce_bus_dmamap_unload(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	if (map == NULL || map == &nobounce_dmamap)
		return;

	if (dmat->ram != NULL) {
		if (map->dmat != dmat || ram_map_busy(map)) {
			printf("busdma: translated map pending; retry unload after callback\n");
			return;
		}
		/* A failed multi-chunk NOWAIT load or preflight may leave
		 * a reservation without any active bounce page. */
		mtx_lock(&bounce_lock);
		if (map->pagesreserved != 0) {
			struct bounce_zone *bz;
			bus_dmamap_t waiting;

			bz = dmat->bounce_zone;
			bz->reserved_bpages -= map->pagesreserved;
			bz->free_bpages += map->pagesreserved;
			map->pagesreserved = 0;
			while ((waiting = STAILQ_FIRST(
			    &bz->bounce_map_waitinglist)) != NULL) {
				if (reserve_bounce_pages(waiting->dmat, waiting, 1) != 0)
					break;
				STAILQ_REMOVE_HEAD(&bz->bounce_map_waitinglist, links);
				STAILQ_INSERT_TAIL(&bounce_map_callbacklist, waiting, links);
				bz->total_deferred++;
				wakeup(&bounce_map_callbacklist);
			}
		}
		map->pagesneeded = 0;
		mtx_unlock(&bounce_lock);
		map->ram_bytes = 0;
		map->flags &= ~(DMAMAP_LOADED | DMAMAP_PREPARED);
	}
	free_bounce_pages(dmat, map);
	map->sync_count = 0;
	map->flags &= ~DMAMAP_MBUF;
}

static void
dma_preread_safe(char *va, vm_size_t size)
{
	/*
	 * Write back any partial cachelines immediately before and
	 * after the DMA region.
	 */
	if (!__is_aligned(va, dcache_line_size))
		cpu_dcache_wb_range(va, 1);
	if (!__is_aligned(va + size, dcache_line_size))
		cpu_dcache_wb_range(va + size, 1);

	cpu_dcache_inv_range(va, size);
}

static void
dma_dcache_sync(struct sync_list *sl, bus_dmasync_op_t op)
{
	uint32_t len, offset;
	vm_page_t m;
	vm_paddr_t pa;
	char *va, *tempva;
	bus_size_t size;

	offset = sl->paddr & PAGE_MASK;
	m = sl->pages;
	size = sl->datacount;
	pa = sl->paddr;

	for ( ; size != 0; size -= len, pa += len, offset = 0, ++m) {
		tempva = NULL;
		if (sl->vaddr == NULL) {
			len = min(PAGE_SIZE - offset, size);
			tempva = pmap_quick_enter_page(m);
			va = tempva + offset;
			KASSERT(pa == (VM_PAGE_TO_PHYS(m) | offset),
			    ("unexpected vm_page_t phys: 0x%16lx != 0x%16lx",
			    VM_PAGE_TO_PHYS(m) | offset, pa));
		} else {
			len = sl->datacount;
			va = sl->vaddr;
		}

		switch (op) {
		case BUS_DMASYNC_PREWRITE:
		case BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD:
			cpu_dcache_wb_range((void *)va, len);
			break;
		case BUS_DMASYNC_PREREAD:
			/*
			 * An mbuf may start in the middle of a cacheline. There
			 * will be no cpu writes to the beginning of that line
			 * (which contains the mbuf header) while dma is in
			 * progress.  Handle that case by doing a writeback of
			 * just the first cacheline before invalidating the
			 * overall buffer.  Any mbuf in a chain may have this
			 * misalignment.  Buffers which are not mbufs bounce if
			 * they are not aligned to a cacheline.
			 */
			dma_preread_safe((void *)va, len);
			break;
		case BUS_DMASYNC_POSTREAD:
		case BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE:
			cpu_dcache_inv_range((void *)va, len);
			break;
		default:
			panic("unsupported combination of sync operations: "
                              "0x%08x\n", op);
		}

		if (tempva != NULL)
			pmap_quick_remove_page(tempva);
	}
}

static void
bounce_bus_dmamap_sync(bus_dma_tag_t dmat, bus_dmamap_t map,
    bus_dmasync_op_t op)
{
	struct bounce_page *bpage;
	struct sync_list *sl, *end;
	char *datavaddr, *tempvaddr;

	if (dmat->ram != NULL && (map == NULL || map->dmat != dmat ||
	    ram_map_busy(map)))
		return;
	if (op == BUS_DMASYNC_POSTWRITE)
		return;

	if ((op & BUS_DMASYNC_POSTREAD) != 0) {
		/*
		 * Wait for any DMA operations to complete before the bcopy.
		 */
		dmb(oshld);
	}
	if (map == NULL)
		goto out;

	if ((bpage = STAILQ_FIRST(&map->bpages)) != NULL) {
		CTR4(KTR_BUSDMA, "%s: tag %p tag flags 0x%x op 0x%x "
		    "performing bounce", __func__, dmat, dmat->common.flags,
		    op);

		if ((op & BUS_DMASYNC_PREWRITE) != 0) {
			while (bpage != NULL) {
				tempvaddr = NULL;
				datavaddr = bpage->datavaddr;
				if (datavaddr == NULL) {
					tempvaddr = pmap_quick_enter_page(
					    bpage->datapage);
					datavaddr = tempvaddr + bpage->dataoffs;
				}

				bcopy(datavaddr, bpage->vaddr, bpage->datacount);
				if (tempvaddr != NULL)
					pmap_quick_remove_page(tempvaddr);
				if ((map->flags & DMAMAP_COHERENT) == 0)
					cpu_dcache_wb_range(bpage->vaddr,
					    bpage->datacount);
				bpage = STAILQ_NEXT(bpage, links);
			}
			dmat->bounce_zone->total_bounced++;
		} else if ((op & BUS_DMASYNC_PREREAD) != 0) {
			while (bpage != NULL) {
				if ((map->flags & DMAMAP_COHERENT) == 0)
					cpu_dcache_wbinv_range(bpage->vaddr,
					    bpage->datacount);
				bpage = STAILQ_NEXT(bpage, links);
			}
		}

		if ((op & BUS_DMASYNC_POSTREAD) != 0) {
			while (bpage != NULL) {
				if ((map->flags & DMAMAP_COHERENT) == 0)
					cpu_dcache_inv_range(bpage->vaddr,
					    bpage->datacount);
				tempvaddr = NULL;
				datavaddr = bpage->datavaddr;
				if (datavaddr == NULL) {
					tempvaddr = pmap_quick_enter_page(
					    bpage->datapage);
					datavaddr = tempvaddr + bpage->dataoffs;
				}

				bcopy(bpage->vaddr, datavaddr, bpage->datacount);

				if (tempvaddr != NULL)
					pmap_quick_remove_page(tempvaddr);
				bpage = STAILQ_NEXT(bpage, links);
			}
			dmat->bounce_zone->total_bounced++;
		}
	}

	/*
	 * Cache maintenance for normal (non-COHERENT non-bounce) buffers.
	 */
	if (map->sync_count != 0) {
		sl = &map->slist[0];
		end = &map->slist[map->sync_count];
		CTR3(KTR_BUSDMA, "%s: tag %p op 0x%x "
		    "performing sync", __func__, dmat, op);

		for ( ; sl != end; ++sl)
			dma_dcache_sync(sl, op);
	}

out:
	/*
	 * If the device is cache-coherent then the cache operation will
	 * have performed a dsb instruction so we don't need another
	 * barrier here.
	 */
	if ((map == NULL || (map->flags & DMAMAP_COHERENT) != 0) &&
	    (op & (BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE)) != 0) {
		/*
		 * Wait for the bcopy to complete before any DMA operations.
		 */
		dmb(oshst);
	}

	if (map != NULL)
		kmsan_bus_dmamap_sync(&map->kmsan_mem, op);
}

#ifdef KMSAN
static void
bounce_bus_dmamap_load_kmsan(bus_dmamap_t map, struct memdesc *mem)
{
	if (map == NULL)
		return;
	if (map->dmat != NULL && map->dmat->ram != NULL &&
	    ((map->flags & (DMAMAP_COUNTING | DMAMAP_LOADED)) != 0 ||
	    ram_map_busy(map)))
		return;
	memcpy(&map->kmsan_mem, mem, sizeof(map->kmsan_mem));
}
#endif

struct bus_dma_impl bus_dma_bounce_impl = {
	.tag_create = bounce_bus_dma_tag_create,
	.tag_destroy = bounce_bus_dma_tag_destroy,
	.tag_set_domain = bounce_bus_dma_tag_set_domain,
	.id_mapped = bounce_bus_dma_id_mapped,
	.map_create = bounce_bus_dmamap_create,
	.map_destroy = bounce_bus_dmamap_destroy,
	.mem_alloc = bounce_bus_dmamem_alloc,
	.mem_free = bounce_bus_dmamem_free,
	.load_phys = bounce_bus_dmamap_load_phys,
	.load_buffer = bounce_bus_dmamap_load_buffer,
	.load_ma = bus_dmamap_load_ma_triv,
	.map_waitok = bounce_bus_dmamap_waitok,
	.map_complete = bounce_bus_dmamap_complete,
	.map_unload = bounce_bus_dmamap_unload,
	.map_sync = bounce_bus_dmamap_sync,
#ifdef KMSAN
	.load_kmsan = bounce_bus_dmamap_load_kmsan,
#endif
};

static void *
nonsecure_alloc(uma_zone_t zone, vm_size_t size, int domain, uint8_t *pflag,
    int wait)
{
	void *p;	/* Returned page */

	*pflag = UMA_SLAB_KERNEL;
	p = kmem_malloc_domainset(DOMAINSET_FIXED(domain), size,
	    wait | M_UNPROTECTED);

	return (p);
}

static void
nonsecure_free(void *mem, vm_size_t size, uint8_t flags)
{
	KASSERT((flags & UMA_SLAB_KERNEL) != 0,
	    ("UMA: page_free used with invalid flags %x", flags));

	kmem_free(mem, size);
}

static void
busdma_bounce_init(void *dummy)
{
	if (in_realm())
		nonsecure_allocator = busdma_bufalloc_create("nonsecure",
		    dcache_line_size,		/* minimum_alignment */
		    nonsecure_alloc,		/* uma_alloc func */
		    nonsecure_free,		/* uma_free func */
		    UMA_ZONE_NOTOUCH);		/* uma_zcreate_flags */
	else
		nonsecure_allocator = busdma_bufalloc_create("nonsecure",
		    dcache_line_size,		/* minimum_alignment */
		    NULL,			/* uma_alloc func */
		    NULL,			/* uma_free func */
		    UMA_ZONE_NOTOUCH);		/* uma_zcreate_flags */
}

SYSINIT(busdma, SI_SUB_KMEM + 1, SI_ORDER_FIRST, busdma_bounce_init, NULL);
