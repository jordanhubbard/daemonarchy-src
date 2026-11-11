
#ifndef _MACHINE_BUS_DMA_H_
#define	_MACHINE_BUS_DMA_H_

#define BUS_DMA_TEMPLATE_MD_FIELDS unsigned arm64_requires_translation;
#define BUS_DMA_TEMPLATE_MD_INIT(t) ((t)->arm64_requires_translation = 0)
#define BUS_DMA_TEMPLATE_MD_CHECK(t) ((t)->arm64_requires_translation != 0)
#define BUS_DMA_MEMDESC_UNSUPPORTED(dmat) \
    (((struct bus_dma_tag_common *)(dmat))->impl == &bus_dma_bounce_impl && \
    bus_dma_bounce_ram_translated(dmat) ? EINVAL : 0)

#define WANT_INLINE_DMAMAP
#include <sys/bus_dma.h>

/* Use existing ARM64 padding, preserving legacy module stack ABI. */
_Static_assert(sizeof(bus_dma_template_t) == 96, "ARM64 DMA template ABI");
_Static_assert(__builtin_offsetof(bus_dma_template_t,
    arm64_requires_translation) == 52, "ARM64 DMA template marker");
_Static_assert(__builtin_offsetof(bus_dma_template_t, maxsegsize) == 56,
    "ARM64 DMA template maxsegsize ABI");
_Static_assert(__builtin_offsetof(bus_dma_template_t, name) == 88,
    "ARM64 DMA template name ABI");

#include <machine/bus_dma_impl.h>

/* Opt-in BCM2712 affine RAM translation.  The parent remains owned until
 * all translated descendants are destroyed.  No device is published here.
 * lowaddr must be a page-ended contiguous mask; highaddr must be MAXADDR.
 * Ordinary bus_dma_tag_create children inherit the selected device domain.
 * Passing an already translated parent with a new composed policy changes
 * domain explicitly, retaining its physical constraints and lifetime.
 */
struct bcm2712_dma_policy;
int bus_dma_tag_create_bcm2712_ram(bus_dma_tag_t,
    const struct bcm2712_dma_policy *, bus_size_t, bus_addr_t, bus_addr_t,
    bus_addr_t, bus_size_t, int, bus_size_t, int, bus_dma_lock_t *, void *,
    bus_dma_tag_t *);

/*
 * Is DMA address 1:1 mapping of physical address
 */
static inline bool
bus_dma_id_mapped(bus_dma_tag_t dmat, vm_paddr_t buf, bus_size_t buflen)
{
	struct bus_dma_tag_common *tc;

	tc = (struct bus_dma_tag_common *)dmat;
	return (tc->impl->id_mapped(dmat, buf, buflen));
}

/*
 * Allocate a handle for mapping from kva/uva/physical
 * address space into bus device space.
 */
static inline int
bus_dmamap_create(bus_dma_tag_t dmat, int flags, bus_dmamap_t *mapp)
{
	struct bus_dma_tag_common *tc;

	tc = (struct bus_dma_tag_common *)dmat;
	return (tc->impl->map_create(dmat, flags, mapp));
}

/*
 * Destroy a handle for mapping from kva/uva/physical
 * address space into bus device space.
 */
static inline int
bus_dmamap_destroy(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	struct bus_dma_tag_common *tc;

	tc = (struct bus_dma_tag_common *)dmat;
	return (tc->impl->map_destroy(dmat, map));
}

/*
 * Allocate a piece of memory that can be efficiently mapped into
 * bus device space based on the constraints listed in the dma tag.
 * A dmamap to for use with dmamap_load is also allocated.
 */
static inline int
bus_dmamem_alloc(bus_dma_tag_t dmat, void** vaddr, int flags,
    bus_dmamap_t *mapp)
{
	struct bus_dma_tag_common *tc;

	tc = (struct bus_dma_tag_common *)dmat;
	return (tc->impl->mem_alloc(dmat, vaddr, flags, mapp));
}

/*
 * Free a piece of memory and it's allociated dmamap, that was allocated
 * via bus_dmamem_alloc.
 */
static inline void
bus_dmamem_free(bus_dma_tag_t dmat, void *vaddr, bus_dmamap_t map)
{
	struct bus_dma_tag_common *tc;

	tc = (struct bus_dma_tag_common *)dmat;
	tc->impl->mem_free(dmat, vaddr, map);
}

/*
 * Release the mapping held by map.
 */
static inline void
bus_dmamap_unload(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	struct bus_dma_tag_common *tc;

	if (map != NULL) {
		tc = (struct bus_dma_tag_common *)dmat;
		tc->impl->map_unload(dmat, map);
	}
}

static inline void
bus_dmamap_sync(bus_dma_tag_t dmat, bus_dmamap_t map, bus_dmasync_op_t op)
{
	struct bus_dma_tag_common *tc;

	tc = (struct bus_dma_tag_common *)dmat;
	tc->impl->map_sync(dmat, map, op);
}

static inline int
_bus_dmamap_load_phys(bus_dma_tag_t dmat, bus_dmamap_t map, vm_paddr_t buf,
    bus_size_t buflen, int flags, bus_dma_segment_t *segs, int *segp)
{
	struct bus_dma_tag_common *tc;

	tc = (struct bus_dma_tag_common *)dmat;
	return (tc->impl->load_phys(dmat, map, buf, buflen, flags, segs,
	    segp));
}

static inline int
_bus_dmamap_load_ma(bus_dma_tag_t dmat, bus_dmamap_t map, struct vm_page **ma,
    bus_size_t tlen, int ma_offs, int flags, bus_dma_segment_t *segs,
    int *segp)
{
	struct bus_dma_tag_common *tc;

	tc = (struct bus_dma_tag_common *)dmat;
	return (tc->impl->load_ma(dmat, map, ma, tlen, ma_offs, flags,
	    segs, segp));
}

static inline int
_bus_dmamap_load_buffer(bus_dma_tag_t dmat, bus_dmamap_t map, void *buf,
    bus_size_t buflen, struct pmap *pmap, int flags, bus_dma_segment_t *segs,
    int *segp)
{
	struct bus_dma_tag_common *tc;

	tc = (struct bus_dma_tag_common *)dmat;
	return (tc->impl->load_buffer(dmat, map, buf, buflen, pmap, flags, segs,
	    segp));
}

static inline void
_bus_dmamap_waitok(bus_dma_tag_t dmat, bus_dmamap_t map,
    struct memdesc *mem, bus_dmamap_callback_t *callback, void *callback_arg)
{
	struct bus_dma_tag_common *tc;

	if (map != NULL) {
		tc = (struct bus_dma_tag_common *)dmat;
		tc->impl->map_waitok(dmat, map, mem, callback, callback_arg);
	}
}

static inline bus_dma_segment_t *
_bus_dmamap_complete(bus_dma_tag_t dmat, bus_dmamap_t map,
    bus_dma_segment_t *segs, int nsegs, int error)
{
	struct bus_dma_tag_common *tc;

	tc = (struct bus_dma_tag_common *)dmat;
	return (tc->impl->map_complete(dmat, map, segs, nsegs, error));
}

#ifdef KMSAN
static inline void
_bus_dmamap_load_kmsan(bus_dma_tag_t dmat, bus_dmamap_t map,
    struct memdesc *mem)
{
	struct bus_dma_tag_common *tc;

	tc = (struct bus_dma_tag_common *)dmat;
	return (tc->impl->load_kmsan(map, mem));
}
#endif

#endif /* !_MACHINE_BUS_DMA_H_ */
