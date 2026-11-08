/* SPDX-License-Identifier: BSD-2-Clause */
/* Primitive dispatch and fault injection; mapping policy is production code. */
static int alloc_bounce_zone(bus_dma_tag_t d)
{
	if (fail_zone)
		return ENOMEM;
	for (struct bounce_zone *z = STAILQ_FIRST(&bounce_zone_list); z;
	     z = STAILQ_NEXT(z, links))
		if (z->lowaddr <= dmat_lowaddr(d) &&
		    z->alignment >= d->common.alignment &&
		    z->domain == d->common.domain) {
			d->bounce_zone = z;
			return 0;
		}
	struct bounce_zone *z = host_alloc(sizeof(*z), M_ZERO);
	if (!z)
		return ENOMEM;
	STAILQ_INIT(&z->bounce_page_list);
	STAILQ_INIT(&z->bounce_map_waitinglist);
	z->lowaddr = dmat_lowaddr(d);
	z->alignment = MAX(d->common.alignment, PAGE_SIZE);
	z->domain = d->common.domain;
	STAILQ_INSERT_TAIL(&bounce_zone_list, z, links);
	d->bounce_zone = z;
	return 0;
}
static int alloc_bounce_pages(bus_dma_tag_t d, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++) {
		struct bounce_page *p = host_alloc(sizeof(*p), M_ZERO);
		if (!p)
			break;
		p->vaddr =
		    host_kmem(PAGE_SIZE, 0, 0, dmat_lowaddr(d), PAGE_SIZE);
		if (!p->vaddr) {
			host_free(p);
			break;
		}
		p->busaddr = vtophys(p->vaddr);
		STAILQ_INSERT_TAIL(&d->bounce_zone->bounce_page_list, p, links);
		d->bounce_zone->total_bpages++;
		d->bounce_zone->free_bpages++;
	}
	return i;
}
static int bus_dma_tag_create(bus_dma_tag_t p, bus_size_t a, bus_addr_t b,
			      bus_addr_t l, bus_addr_t h, void *f, void *fa,
			      bus_size_t ms, int ns, bus_size_t ss, int fl,
			      bus_dma_lock_t *lk, void *arg, bus_dma_tag_t *out)
{
	if (f || fa)
		return EINVAL;
	return bounce_bus_dma_tag_create(p, a, b, l, h, ms, ns, ss, fl, lk, arg,
					 out);
}
static void bus_dmamap_unload(bus_dma_tag_t d, bus_dmamap_t m)
{
	bounce_bus_dmamap_unload(d, m);
}
static int _bus_dmamap_load_buffer(bus_dma_tag_t d, bus_dmamap_t m, void *b,
				   bus_size_t n, pmap_t p, int f,
				   bus_dma_segment_t *s, int *i)
{
	return bounce_bus_dmamap_load_buffer(d, m, b, n, p, f, s, i);
}
static int _bus_dmamap_load_phys(bus_dma_tag_t d, bus_dmamap_t m, vm_paddr_t b,
				 bus_size_t n, int f, bus_dma_segment_t *s,
				 int *i)
{
	return bounce_bus_dmamap_load_phys(d, m, b, n, f, s, i);
}
static int _bus_dmamap_load_ma(bus_dma_tag_t d, bus_dmamap_t m, vm_page_t *p,
			       bus_size_t n, int off, int f,
			       bus_dma_segment_t *s, int *i)
{
	return bus_dmamap_load_ma_triv(d, m, p, n, off, f, s, i);
}
static void _bus_dmamap_waitok(bus_dma_tag_t d, bus_dmamap_t m,
			       struct memdesc *mem, bus_dmamap_callback_t *c,
			       void *a)
{
	bounce_bus_dmamap_waitok(d, m, mem, c, a);
}
static void _bus_dmamap_load_kmsan(bus_dma_tag_t d, bus_dmamap_t m,
				   struct memdesc *mem)
{
	(void)d;
	bounce_bus_dmamap_load_kmsan(m, mem);
}
static bus_dma_segment_t *_bus_dmamap_complete(bus_dma_tag_t d, bus_dmamap_t m,
					       bus_dma_segment_t *s, int n,
					       int e)
{
	return bounce_bus_dmamap_complete(d, m, s, n, e);
}
struct bus_dma_impl bus_dma_bounce_impl = {.tag_set_domain =
					       bounce_bus_dma_tag_set_domain};
/* INSERT_REPLAY */
static int callbacks, callback_error, callback_segments;
static bus_dma_segment_t last_segments[128];
static void record(void *arg, bus_dma_segment_t *s, int n, int e)
{
	(void)arg;
	callbacks++;
	callback_error = e;
	callback_segments = n;
	if (n)
		memcpy(last_segments, s, n * sizeof(*s));
}
static void lock_driver(void *arg, int op)
{
	struct mtx *m = arg;
	if (op == BUS_DMA_LOCK)
		mtx_lock(m);
	else
		mtx_unlock(m);
}
static void reset_callback(void)
{
	callbacks = 0;
	callback_error = -1;
	callback_segments = -1;
}
static struct bcm2712_dma_policy policy(bool rp1)
{
	struct bcm2712_dma_policy p;
	struct bcm2712_dma_root_window rows[2] = {
	    {0, UINT64_C(1) << 36, UINT64_C(1) << 36, BCM2712_DMA_RAM,
	     0x43000000},
	    {0, 0, UINT64_C(1) << 36, BCM2712_DMA_RAM, 0x02000000}};
	assert(bcm2712_dma_policy_root(&p, rows, rp1 ? 2 : 1) == 0);
	return p;
}
static bus_dma_tag_t physical_parent(uint64_t limit)
{
	bus_dma_tag_t d;
	assert(bounce_bus_dma_tag_create(NULL, 1, 0, limit, UINT64_MAX, 65536,
					 64, 65536, 0, NULL, NULL, &d) == 0);
	return d;
}
static bus_dma_tag_t translated(bus_dma_tag_t p, bool rp1, uint64_t mask,
				bool coherent, struct mtx *lock)
{
	bus_dma_tag_t d;
	struct bcm2712_dma_policy pol = policy(rp1);
	assert(bus_dma_tag_create_bcm2712_ram(p, &pol, 1, 0, mask, UINT64_MAX,
					      65536, 64, 65536,
					      coherent ? BUS_DMA_COHERENT : 0,
					      lock_driver, lock, &d) == 0);
	return d;
}

static void *buffer_at(uint64_t physical, size_t length)
{
	void *v = NULL;
	assert(posix_memalign(&v, PAGE_SIZE, length) == 0);
	memset(v, 0x5a, length);
	host_register(v, length, physical);
	return v;
}
static void release_map(bus_dma_tag_t d, bus_dmamap_t m)
{
	bounce_bus_dmamap_unload(d, m);
	assert(bounce_bus_dmamap_destroy(d, m) == 0);
}
static void load_mem(bus_dma_tag_t d, bus_dmamap_t m, struct memdesc mem,
		     int flags)
{
	reset_callback();
	assert(bus_dmamap_load_mem(d, m, &mem, record, NULL, flags) == 0);
	assert(callbacks == 1 && callback_error == 0 && callback_segments > 0);
}
static void test_constructor(void)
{
	struct mtx lock = {0};
	struct bcm2712_dma_policy pol = policy(false);
	bus_dma_tag_t out, p = physical_parent(UINT64_MAX),
			   root = translated(p, false, UINT64_MAX, true, &lock);
	/* The helper itself rejects a high-partial, noncontiguous mask. */
	struct bcm2712_dma_ram poison, saved;
	memset(&poison, 0xa5, sizeof(poison));
	saved = poison;
	struct bcm2712_dma_policy aliases = policy(true);
	assert(bcm2712_dma_ram_select(
		   &aliases, (UINT64_C(1) << 36) + (UINT64_C(1) << 30) - 1,
		   UINT64_MAX, 1, 0, PAGE_SIZE, &poison) == EINVAL);
	assert(memcmp(&poison, &saved, sizeof(poison)) == 0);

	assert(root->ram_view.offset == (UINT64_C(1) << 36));
	assert(root->ram_view.physical_limit == (UINT64_C(1) << 36) - 1);
	assert(bounce_bus_dma_tag_destroy(p) == EBUSY);
	struct bcm2712_dma_policy rp = policy(true);
	bus_dma_tag_t rp1 = translated(root, true, UINT32_MAX, true, &lock),
		      child;
	assert(rp1->ram_view.offset == 0 &&
	       rp1->ram_view.physical_limit == UINT32_MAX);
	assert(bounce_bus_dma_tag_create(rp1, 64, 4096, UINT32_MAX, UINT64_MAX,
					 4096, 4, 4096, 0, lock_driver, &lock,
					 &child) == 0);
	assert(child->ram == rp1->ram && child->ram->refs == 2 &&
	       child->ram_view.offset == 0);
	assert(bounce_bus_dma_tag_destroy(rp1) == EBUSY);
	assert(bounce_bus_dma_tag_destroy(root) == EBUSY);
	assert(bounce_bus_dma_tag_create(root, 1, 0, UINT32_MAX, UINT64_MAX,
					 4096, 4, 4096, 0, NULL, NULL,
					 &out) == EFBIG &&
	       out == NULL);
	assert(bus_dma_tag_create_bcm2712_ram(root, &rp, 1, 0, 0xfffff001,
					      UINT64_MAX, 4096, 4, 4096, 0,
					      NULL, NULL, &out) == EOPNOTSUPP &&
	       out == NULL);
	assert(bus_dma_tag_create_bcm2712_ram(root, &rp, 1, 0, UINT32_MAX,
					      UINT32_MAX, 4096, 4, 4096, 0,
					      NULL, NULL, &out) == EOPNOTSUPP &&
	       out == NULL);
	assert(bus_dma_tag_create_bcm2712_ram(root, &rp, 8192, 0, UINT64_MAX,
					      UINT64_MAX, 4096, 4, 4096, 0,
					      NULL, NULL, &out) == EOPNOTSUPP &&
	       out == NULL);
	assert(bus_dma_tag_create_bcm2712_ram(
		   root, &rp, 1, UINT64_C(1) << 37, UINT64_MAX, UINT64_MAX,
		   4096, 4, 4096, 0, NULL, NULL, &out) == EOPNOTSUPP &&
	       out == NULL);
	realm = 1;
	assert(bus_dma_tag_create_bcm2712_ram(root, &rp, 1, 0, UINT64_MAX,
					      UINT64_MAX, 4096, 4, 4096, 0,
					      NULL, NULL, &out) == EOPNOTSUPP &&
	       out == NULL);
	realm = 0;
	fail_alloc = 0;
	assert(bus_dma_tag_create_bcm2712_ram(p, &pol, 1, 0, UINT64_MAX,
					      UINT64_MAX, 4096, 4, 4096, 0,
					      NULL, NULL, &out) == ENOMEM &&
	       out == NULL);
	fail_alloc = 1;
	assert(bus_dma_tag_create_bcm2712_ram(p, &pol, 1, 0, UINT64_MAX,
					      UINT64_MAX, 4096, 4, 4096, 0,
					      NULL, NULL, &out) == ENOMEM &&
	       out == NULL);
	fail_alloc = -1;
	assert(root->ram_children == 1 && p->ram_children == 1);
	assert(bounce_bus_dma_tag_destroy(child) == 0);
	assert(bounce_bus_dma_tag_destroy(rp1) == 0);
	assert(bounce_bus_dma_tag_destroy(root) == 0);
	assert(bounce_bus_dma_tag_destroy(p) == 0);
	puts("constructor/domain/alias/inheritance/fault controls PASS");
}
static void test_template(void)
{
	_Static_assert(sizeof(bus_dma_template_t) == 96, "unchanged ABI");
	_Static_assert(__builtin_offsetof(bus_dma_template_t,
					  arm64_requires_translation) == 52,
		       "padding");
	_Static_assert(__builtin_offsetof(bus_dma_template_t, maxsegsize) == 56,
		       "maxsegsize ABI");
	_Static_assert(__builtin_offsetof(bus_dma_template_t, name) == 88,
		       "name ABI");
	struct mtx lock = {0};
	bus_dma_tag_t p = physical_parent(UINT64_MAX),
		      d = translated(p, false, UINT64_MAX, true, &lock), out;
	bus_dma_template_t t;
	memset(&t, 0xa5, sizeof(t));
	bus_dma_template_clone(&t, d);
	assert(t.arm64_requires_translation == 1 && t.parent == NULL);
	assert(bounce_bus_dma_tag_destroy(d) == 0);
	assert(bounce_bus_dma_tag_destroy(p) == 0);
	bus_dma_param_t kv[] = {
	    {BD_PARAM_PARENT, .ptr = (void *)(uintptr_t)0xdeadbeef},
	    {BD_PARAM_FLAGS, .num = 0}};
	bus_dma_template_fill(&t, kv, 2);
	assert(t.arm64_requires_translation == 1);
	assert(bus_dma_template_tag(&t, &out) == EOPNOTSUPP && out == NULL);
	bus_dma_template_init(&t, NULL);
	assert(t.arm64_requires_translation == 0);
	p = physical_parent(UINT64_MAX);
	bus_dma_template_clone(&t, p);
	assert(t.arm64_requires_translation == 0);
	assert(bounce_bus_dma_tag_destroy(p) == 0);
	puts("by-value translated-template refusal/poison/legacy ABI controls "
	     "PASS");
}
static void test_load_forms(void)
{
	struct mtx lock = {0};
	bus_dma_tag_t p = physical_parent(UINT64_MAX),
		      d = translated(p, false, UINT64_MAX, false, &lock);
	bus_dmamap_t m;
	assert(bounce_bus_dmamap_create(d, 0, &m) == 0);
	void *v = buffer_at(UINT64_C(0x100000000), 3 * PAGE_SIZE);
	struct memdesc mem = {.md_type = MEMDESC_PADDR,
			      .md_len = PAGE_SIZE,
			      .u.md_paddr = UINT64_C(0x100000000)};
	load_mem(d, m, mem, BUS_DMA_NOWAIT);
	assert(last_segments[0].ds_addr == UINT64_C(0x1100000000));
	assert(m->slist[0].paddr == UINT64_C(0x100000000));
	assert(!bounce_bus_dma_id_mapped(d, mem.u.md_paddr, mem.md_len));
	int oldcache = cache_calls;
	bounce_bus_dmamap_sync(d, m, BUS_DMASYNC_PREWRITE);
	assert(cache_calls > oldcache);
	assert(bounce_bus_dmamap_destroy(d, m) == EBUSY);
	bounce_bus_dmamap_unload(d, m);
	mem = (struct memdesc){
	    .md_type = MEMDESC_VADDR, .md_len = 2 * PAGE_SIZE, .u.md_vaddr = v};
	load_mem(d, m, mem, BUS_DMA_WAITOK);
	assert(last_segments[0].ds_addr == UINT64_C(0x1100000000) &&
	       last_segments[0].ds_len == 2 * PAGE_SIZE);
	bounce_bus_dmamap_unload(d, m);
	bus_dma_segment_t list[2] = {{UINT64_C(0x100000000), PAGE_SIZE},
				     {UINT64_C(0x100002000), PAGE_SIZE}};
	mem = (struct memdesc){
	    .md_type = MEMDESC_PLIST, .md_nseg = 2, .u.md_list = list};
	load_mem(d, m, mem, BUS_DMA_WAITOK);
	assert(callback_segments == 2 &&
	       last_segments[1].ds_addr == UINT64_C(0x1100002000));
	bounce_bus_dmamap_unload(d, m);
	list[0].ds_addr = (uintptr_t)v;
	list[1].ds_addr = (uintptr_t)v + 2 * PAGE_SIZE;
	mem.md_type = MEMDESC_VLIST;
	load_mem(d, m, mem, BUS_DMA_WAITOK);
	assert(callback_segments == 2);
	bounce_bus_dmamap_unload(d, m);
	vm_page_t pages[2] = {host_phys_page(UINT64_C(0x100000000)),
			      host_phys_page(UINT64_C(0x100002000))};
	mem = (struct memdesc){.md_type = MEMDESC_VMPAGES,
			       .md_len = 2 * PAGE_SIZE,
			       .u.md_ma = pages};
	load_mem(d, m, mem, BUS_DMA_WAITOK);
	assert(callback_segments == 2);
	bounce_bus_dmamap_unload(d, m);
	struct iovec iov[2] = {{v, PAGE_SIZE},
			       {(char *)v + 2 * PAGE_SIZE, PAGE_SIZE}};
	struct uio uio = {
	    .uio_resid = 2 * PAGE_SIZE, .uio_iov = iov, .uio_iovcnt = 2};
	mem = (struct memdesc){.md_type = MEMDESC_UIO, .u.md_uio = &uio};
	load_mem(d, m, mem, BUS_DMA_WAITOK);
	assert(callback_segments == 2);
	bounce_bus_dmamap_unload(d, m);
	struct mbuf mb[2] = {
	    {.m_len = PAGE_SIZE, .m_next = &mb[1], .m_data = v},
	    {.m_len = PAGE_SIZE, .m_data = (char *)v + 2 * PAGE_SIZE}};
	mem = (struct memdesc){.md_type = MEMDESC_MBUF, .u.md_mbuf = &mb[0]};
	load_mem(d, m, mem, BUS_DMA_WAITOK);
	assert(callback_segments == 2);
	bounce_bus_dmamap_unload(d, m);
	struct mbuf epg = {
	    .m_len = 2 * PAGE_SIZE,
	    .m_flags = M_EXTPG,
	    .m_epg_npgs = 2,
	    .m_epg_last_len = PAGE_SIZE,
	    .m_epg_pa = {UINT64_C(0x100000000), UINT64_C(0x100002000)}};
	mem.u.md_mbuf = &epg;
	load_mem(d, m, mem, BUS_DMA_WAITOK);
	assert(callback_segments == 2);
	bounce_bus_dmamap_unload(d, m);
	struct mbuf partial = {.m_len = 40,
			       .m_flags = M_EXTPG,
			       .m_data = (char *)2,
			       .m_epg_hdrlen = 8,
			       .m_epg_hdr = v,
			       .m_epg_1st_off = 16,
			       .m_epg_npgs = 1,
			       .m_epg_last_len = 32,
			       .m_epg_trllen = 2,
			       .m_epg_trail = (char *)v + 2 * PAGE_SIZE,
			       .m_epg_pa = {UINT64_C(0x100000000)}};
	mem.u.md_mbuf = &partial;
	load_mem(d, m, mem, BUS_DMA_WAITOK);
	assert(callback_segments == 3 && last_segments[0].ds_len == 6 &&
	       last_segments[1].ds_len == 32 && last_segments[2].ds_len == 2);
	bounce_bus_dmamap_sync(d, m, BUS_DMASYNC_PREWRITE);
	bounce_bus_dmamap_unload(d, m);
	mem = (struct memdesc){.md_type = MEMDESC_PADDR,
			       .md_len = 65537,
			       .u.md_paddr = UINT64_C(0x100000000)};
	reset_callback();
	bus_dmamap_load_mem(d, m, &mem, record, NULL, BUS_DMA_WAITOK);
	assert(callbacks == 1 && callback_error == EFBIG &&
	       m->pagesneeded == 0 && m->pagesreserved == 0 &&
	       m->sync_count == 0);
	assert(bounce_bus_dmamap_destroy(d, m) == 0);
	assert(bounce_bus_dma_tag_destroy(d) == 0);
	assert(bounce_bus_dma_tag_destroy(p) == 0);
	puts("actual MI PADDR/VADDR/PLIST/VLIST/pages/UIO/mbuf/EPG +physical "
	     "sync controls PASS");
}
static void test_bounce(void)
{
	struct mtx lock = {0};
	bus_dma_tag_t p = physical_parent(0x3bffffff),
		      d = translated(p, false, UINT64_MAX, false, &lock);
	bus_dmamap_t m;
	assert(bounce_bus_dmamap_create(d, 0, &m) == 0);
	void *v = buffer_at(UINT64_C(0x100010000), PAGE_SIZE);
	struct memdesc mem = {
	    .md_type = MEMDESC_VADDR, .md_len = PAGE_SIZE, .u.md_vaddr = v};
	load_mem(d, m, mem, BUS_DMA_NOWAIT);
	struct bounce_page *bp = STAILQ_FIRST(&m->bpages);
	assert(bp && bp->busaddr <= 0x3bffffff &&
	       last_segments[0].ds_addr == bp->busaddr + (UINT64_C(1) << 36));
	bounce_bus_dmamap_sync(d, m, BUS_DMASYNC_PREWRITE);
	assert(memcmp(bp->vaddr, v, PAGE_SIZE) == 0);
	memset(bp->vaddr, 0xa6, PAGE_SIZE);
	bounce_bus_dmamap_sync(d, m, BUS_DMASYNC_POSTREAD);
	assert(((unsigned char *)v)[0] == 0xa6 &&
	       ((unsigned char *)v)[PAGE_SIZE - 1] == 0xa6);
	release_map(d, m);
	assert(bounce_bus_dma_tag_destroy(d) == 0);
	assert(bounce_bus_dma_tag_destroy(p) == 0);
	p = physical_parent(UINT64_MAX);
	d = translated(p, true, UINT32_MAX, true, &lock);
	assert(bounce_bus_dmamap_create(d, 0, &m) == 0);
	load_mem(d, m, mem, BUS_DMA_NOWAIT);
	bp = STAILQ_FIRST(&m->bpages);
	assert(bp && bp->busaddr <= UINT32_MAX &&
	       last_segments[0].ds_addr == bp->busaddr);
	release_map(d, m);
	assert(bounce_bus_dma_tag_destroy(d) == 0);
	assert(bounce_bus_dma_tag_destroy(p) == 0);
	puts("actual bounce PREWRITE/POSTREAD copies +RP1 low-alias32 controls "
	     "PASS");
}
static bus_dmamap_t callback_map;
static bus_dma_tag_t callback_tag;
static int callback_destroyed;
static void destroy_callback(void *arg, bus_dma_segment_t *s, int n, int error)
{
	record(arg, s, n, error);
	assert(error == 0);
	bounce_bus_dmamap_unload(callback_tag, callback_map);
	assert(bounce_bus_dmamap_destroy(callback_tag, callback_map) == 0);
	assert(bounce_bus_dma_tag_destroy(callback_tag) == 0);
	callback_destroyed++;
}
static void test_deferred(void)
{
	struct mtx lock = {0};
	bus_dma_tag_t p = physical_parent(0x3bffffff),
		      d = translated(p, false, UINT64_MAX, false, &lock);
	bus_dmamap_t m;
	assert(bounce_bus_dmamap_create(d, 0, &m) == 0);
	struct bounce_zone *z = d->bounce_zone;
	/* Inject another owner's pressure without changing the mapping
	 * algorithm. */
	struct bus_dmamap pressure = {0};
	STAILQ_INIT(&pressure.bpages);
	pressure.dmat = d;
	STAILQ_CONCAT(&pressure.bpages, &z->bounce_page_list);
	z->active_bpages += z->free_bpages;
	z->free_bpages = 0;
	struct bounce_page *available = STAILQ_FIRST(&pressure.bpages);
	STAILQ_REMOVE_HEAD(&pressure.bpages, links);
	STAILQ_INSERT_TAIL(&z->bounce_page_list, available, links);
	z->active_bpages--;
	z->free_bpages = 1;
	void *v = buffer_at(UINT64_C(0x100020000), 3 * PAGE_SIZE);
	bus_dma_segment_t list[2] = {{(uintptr_t)v, PAGE_SIZE},
				     {(uintptr_t)v + 2 * PAGE_SIZE, PAGE_SIZE}};
	struct memdesc mem = {
	    .md_type = MEMDESC_VLIST, .md_nseg = 2, .u.md_list = list};
	reset_callback();
	int rc = bus_dmamap_load_mem(d, m, &mem, record, NULL, BUS_DMA_NOWAIT);
	assert(rc == ENOMEM && callbacks == 1 && callback_error == ENOMEM &&
	       m->pagesreserved == 0 && STAILQ_EMPTY(&m->bpages));
	reset_callback();
	rc = bus_dmamap_load_mem(d, m, &mem, destroy_callback, NULL,
				 BUS_DMA_WAITOK);
	assert(rc == EINPROGRESS && callbacks == 0 && m->pagesneeded == 2 &&
	       m->pagesreserved == 1 && STAILQ_EMPTY(&m->bpages) &&
	       m->sync_count == 0 && (m->flags & DMAMAP_PENDING));
	assert(bounce_bus_dmamap_destroy(d, m) == EBUSY &&
	       bounce_bus_dma_tag_destroy(d) == EBUSY);
	struct memdesc saved = m->mem, kmsan = m->kmsan_mem;
	int oldcache = cache_calls;
	bounce_bus_dmamap_unload(d, m);
	bounce_bus_dmamap_sync(d, m, BUS_DMASYNC_PREWRITE);
	assert(cache_calls == oldcache);
	reset_callback();
	bus_dmamap_load_mem(d, m, &mem, record, (void *)1, BUS_DMA_WAITOK);
	assert(callbacks == 1 && callback_error == EBUSY &&
	       m->callback == destroy_callback &&
	       memcmp(&m->mem, &saved, sizeof(saved)) == 0 &&
	       memcmp(&m->kmsan_mem, &kmsan, sizeof(kmsan)) == 0);
	free_bounce_pages(d, &pressure);
	assert(STAILQ_FIRST(&bounce_map_callbacklist) == m &&
	       m->pagesreserved == 2);
	STAILQ_REMOVE_HEAD(&bounce_map_callbacklist, links);
	callback_map = m;
	callback_tag = d;
	reset_callback();
	replay_one(m);
	assert(callbacks == 1 && callback_error == 0 &&
	       callback_destroyed == 1 && !lock.held);
	assert(p->ram_children == 0);
	assert(bounce_bus_dma_tag_destroy(p) == 0);
	puts("whole-transfer deferred replay/exactly-once/callback destroy "
	     "+saved unlock controls PASS");
}

static void test_domain_allocations(void)
{
	struct mtx lock = {0};
	bus_dma_tag_t p = physical_parent(0x3bffffff),
		      d = translated(p, false, UINT64_MAX, true, &lock), c;
	bus_dmamap_t m;
	assert(bounce_bus_dma_tag_create(d, 1, 0, UINT64_MAX, UINT64_MAX,
					 PAGE_SIZE, 4, PAGE_SIZE, 0,
					 lock_driver, &lock, &c) == 0);
	assert(bus_dma_tag_set_domain(d, 1) == EBUSY && d->common.domain == 0);
	assert(bus_dma_tag_set_domain(p, 1) == EBUSY && p->common.domain == 0);
	assert(bounce_bus_dma_tag_destroy(c) == 0);
	assert(bounce_bus_dmamap_create(d, 0, &m) == 0);
	assert(bus_dma_tag_set_domain(d, 1) == EBUSY && d->common.domain == 0);
	release_map(d, m);
	struct bounce_zone *old = d->bounce_zone;
	int oldflags = d->bounce_flags;
	fail_zone = 1;
	assert(bus_dma_tag_set_domain(d, 1) == ENOMEM &&
	       d->common.domain == 0 && d->bounce_zone == old &&
	       d->bounce_flags == oldflags);
	fail_zone = 0;
	assert(bus_dma_tag_set_domain(d, 1) == 0 && d->common.domain == 1 &&
	       d->committed_domain == 1 && matched_limit == 0x3bffffff);
	assert(bounce_bus_dma_tag_destroy(d) == 0);
	assert(bounce_bus_dma_tag_destroy(p) == 0);
	p = physical_parent(UINT64_MAX);
	d = translated(p, false, UINT64_MAX, true, &lock);
	assert(bounce_bus_dma_tag_create(d, 64, 0, UINT64_MAX, UINT64_MAX,
					 PAGE_SIZE, 4, PAGE_SIZE, 0,
					 lock_driver, &lock, &c) == 0);
	void *v = (void *)1;
	m = (void *)1;
	fail_alloc = 0;
	assert(bounce_bus_dmamem_alloc(c, &v, BUS_DMA_NOWAIT, &m) == ENOMEM &&
	       m == NULL && v == NULL);
	fail_alloc = -1;
	assert(bounce_bus_dmamem_alloc(c, &v, BUS_DMA_NOWAIT | BUS_DMA_ZERO,
				       &m) == 0 &&
	       m->ram_alloc_flags == BF_KMEM_ALLOC_PAGES);
	struct memdesc mem = {
	    .md_type = MEMDESC_VADDR, .md_len = PAGE_SIZE, .u.md_vaddr = v};
	load_mem(c, m, mem, BUS_DMA_NOWAIT);
	assert(last_segments[0].ds_addr == vtophys(v) + (UINT64_C(1) << 36));
	assert(bounce_bus_dma_tag_destroy(c) == EBUSY);
	bounce_bus_dmamap_unload(c, m);
	int frees = kmem_frees;
	bounce_bus_dmamem_free(c, v, m);
	assert(kmem_frees == frees + 1);
	/* A subsequent UMA allocation must not inherit an earlier allocation's
	 * tag-global KMEM bit when choosing its free operation. */
	use_bufzone = 1;
	assert(bounce_bus_dmamem_alloc(c, &v, BUS_DMA_NOWAIT, &m) == 0 &&
	       m->ram_alloc_flags == 0);
	frees = kmem_frees;
	bounce_bus_dmamem_free(c, v, m);
	assert(kmem_frees == frees);
	use_bufzone = 0;
	assert(bounce_bus_dma_tag_destroy(c) == 0);
	assert(bounce_bus_dma_tag_destroy(d) == 0);
	assert(bounce_bus_dma_tag_destroy(p) == 0);
	puts("domain busy/physical selection/rollback +dmamem allocation/free "
	     "ownership controls PASS");
}

static void test_error_cleanup(void)
{
	struct mtx lock = {0};
	bus_dma_tag_t p = physical_parent(UINT64_MAX), d;
	struct bcm2712_dma_policy pol = policy(false);
	bus_dmamap_t m;
	assert(bus_dma_tag_create_bcm2712_ram(
		   p, &pol, 1, 0, UINT64_MAX, UINT64_MAX, 2 * PAGE_SIZE, 1,
		   PAGE_SIZE, 0, lock_driver, &lock, &d) == 0);
	assert(bounce_bus_dmamap_create(d, 0, &m) == 0);
	void *v = buffer_at(UINT64_C(0x100030000), 3 * PAGE_SIZE);
	bus_dma_segment_t list[2] = {{UINT64_C(0x100030000), PAGE_SIZE},
				     {UINT64_C(0x100032000), PAGE_SIZE}};
	struct memdesc mem = {
	    .md_type = MEMDESC_PLIST, .md_nseg = 2, .u.md_list = list};
	reset_callback();
	bus_dmamap_load_mem(d, m, &mem, record, NULL, BUS_DMA_NOWAIT);
	assert(callbacks == 1 && callback_error == EFBIG &&
	       m->sync_count == 0 && m->pagesreserved == 0 &&
	       STAILQ_EMPTY(&m->bpages));
	mem = (struct memdesc){
	    .md_type = MEMDESC_VADDR, .md_len = PAGE_SIZE, .u.md_vaddr = v};
	load_mem(d, m, mem, BUS_DMA_NOWAIT);
	bounce_bus_dmamap_unload(d, m);
	mem = (struct memdesc){.md_type = 99};
	reset_callback();
	bus_dmamap_load_mem(d, m, &mem, record, NULL, BUS_DMA_WAITOK);
	assert(callbacks == 1 && callback_error == EINVAL &&
	       m->sync_count == 0);
	mem = (struct memdesc){.md_type = MEMDESC_PADDR,
			       .md_len = PAGE_SIZE,
			       .u.md_paddr = 0xdead000};
	reset_callback();
	bus_dmamap_load_mem(d, m, &mem, record, NULL, BUS_DMA_NOWAIT);
	assert(callbacks == 1 && callback_error == ENXIO &&
	       STAILQ_EMPTY(&m->bpages));
	assert(bounce_bus_dmamap_destroy(d, m) == 0);
	assert(bounce_bus_dma_tag_destroy(d) == 0);
	assert(bounce_bus_dma_tag_destroy(p) == 0);
	puts("partial multi-chunk error rollback/unsupported memdesc/non-RAM "
	     "refusal controls PASS");
}

static void test_ref_and_pool_failures(void)
{
	struct mtx lock = {0};
	bus_dma_tag_t p = physical_parent(UINT64_MAX),
		      d = translated(p, false, UINT64_MAX, true, &lock), out;
	struct bcm2712_dma_policy pol = policy(false);
	bus_dmamap_t map = (void *)1;
	p->ram_children = UINT_MAX;
	assert(bus_dma_tag_create_bcm2712_ram(p, &pol, 1, 0, UINT64_MAX,
					      UINT64_MAX, 4096, 4, 4096, 0,
					      NULL, NULL, &out) == EOVERFLOW &&
	       out == NULL && p->ram_children == UINT_MAX);
	p->ram_children = 1;
	d->ram->refs = UINT_MAX;
	assert(bounce_bus_dma_tag_create(d, 1, 0, UINT64_MAX, UINT64_MAX, 4096,
					 4, 4096, 0, NULL, NULL,
					 &out) == EOVERFLOW &&
	       out == NULL && d->ram->refs == UINT_MAX && d->ram_children == 0);
	d->ram->refs = 1;
	fail_zone = 1;
	assert(bus_dma_tag_create_bcm2712_ram(
		   p, &pol, 1, 0, UINT64_MAX, UINT64_MAX, 4096, 4, 4096,
		   BUS_DMA_ALLOCNOW, NULL, NULL, &out) == ENOMEM &&
	       out == NULL && p->ram_children == 1);
	fail_zone = 0;
	assert(bus_dma_tag_set_domain(d, 7) == 0);
	fail_alloc = 5;
	assert(bounce_bus_dmamap_create(d, 0, &map) == ENOMEM && map == NULL &&
	       d->map_count == 0 && d->bounce_zone->map_count == 0 &&
	       d->bounce_zone->total_bpages == 1);
	fail_alloc = -1;
	assert(bounce_bus_dmamap_create(d, 0, &map) == 0 && d->map_count == 1 &&
	       d->bounce_zone->map_count == 1);
	(void)buffer_at(UINT64_C(0x1dead000), PAGE_SIZE);
	host_phys_page(UINT64_C(0x1dead000))->fictitious = true;
	struct memdesc mem = {.md_type = MEMDESC_PADDR,
			      .md_len = PAGE_SIZE,
			      .u.md_paddr = UINT64_C(0x1dead000)};
	reset_callback();
	bus_dmamap_load_mem(d, map, &mem, record, NULL, BUS_DMA_NOWAIT);
	assert(callbacks == 1 && callback_error == ENXIO &&
	       STAILQ_EMPTY(&map->bpages));
	release_map(d, map);
	assert(d->bounce_zone->map_count == 0);
	assert(bounce_bus_dma_tag_destroy(d) == 0);
	assert(bounce_bus_dma_tag_destroy(p) == 0);
	puts("checked reference overflow/ALLOCNOW/partial pool "
	     "rollback/fictitious-page refusal PASS");
}

static struct memdesc reload_mem;
static void reload_callback(void *arg, bus_dma_segment_t *s, int n, int error)
{
	record(arg, s, n, error);
	assert(error == 0);
	bounce_bus_dmamap_unload(callback_tag, callback_map);
	assert(bus_dmamap_load_mem(callback_tag, callback_map, &reload_mem,
				   record, NULL, BUS_DMA_NOWAIT) == 0);
}
static void test_deferred_reload(void)
{
	struct mtx lock = {0};
	bus_dma_tag_t p = physical_parent(0x3bffffff),
		      d = translated(p, false, UINT64_MAX, false, &lock);
	bus_dmamap_t m;
	assert(bounce_bus_dmamap_create(d, 0, &m) == 0);
	struct bounce_zone *z = d->bounce_zone;
	struct bus_dmamap pressure = {0};
	STAILQ_INIT(&pressure.bpages);
	pressure.dmat = d;
	STAILQ_CONCAT(&pressure.bpages, &z->bounce_page_list);
	z->active_bpages += z->free_bpages;
	z->free_bpages = 0;
	void *v = buffer_at(UINT64_C(0x100050000), 2 * PAGE_SIZE);
	struct memdesc mem = {
	    .md_type = MEMDESC_VADDR, .md_len = 2 * PAGE_SIZE, .u.md_vaddr = v};
	reload_mem = mem;
	reload_mem.md_len = PAGE_SIZE;
	callback_tag = d;
	callback_map = m;
	reset_callback();
	assert(bus_dmamap_load_mem(d, m, &mem, reload_callback, NULL,
				   BUS_DMA_WAITOK) == EINPROGRESS &&
	       callbacks == 0);
	free_bounce_pages(d, &pressure);
	assert(STAILQ_FIRST(&bounce_map_callbacklist) == m);
	STAILQ_REMOVE_HEAD(&bounce_map_callbacklist, links);
	replay_one(m);
	assert(callbacks == 2 && callback_error == 0 && !lock.held &&
	       (m->flags & DMAMAP_LOADED) && !(m->flags & DMAMAP_PENDING));
	release_map(d, m);
	assert(bounce_bus_dma_tag_destroy(d) == 0);
	assert(bounce_bus_dma_tag_destroy(p) == 0);
	puts("deferred callback starts a new load without post-callback state "
	     "corruption PASS");
}
int main(void)
{
	test_constructor();
	test_template();
	test_load_forms();
	test_bounce();
	test_deferred();
	test_domain_allocations();
	test_error_cleanup();
	test_ref_and_pool_failures();
	test_deferred_reload();
	puts("BCM2712 translated RAM busdma host controls PASS");
	return 0;
}
