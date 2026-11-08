/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Diagnostic native API consumer for an explicitly selected QEMU-only profile.
 * Normal GEOM/CAM/umass/xHCI submit real USB disk I/O from high physical RAM.
 * No physical disk or arbitrary provider name is accepted by this module.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/bus.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <geom/geom.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <machine/bus.h>
#include <machine/bus_dma.h>
#include <arm64/broadcom/bcm2712_dma_policy.h>
#include <arm64/broadcom/bcm2711_dma_test.h>
#include <crypto/sha2/sha256.h>

#define LENGTH (128 * 1024)
#define HIGH_START (UINT64_C(4) << 30)
#define HIGH_END ((UINT64_C(8) << 30) - 1)
#define SCRATCH_SIZE (16 * 1024 * 1024)
#define HOLD_COUNT 256
#define PRIVATE_LIMIT 0x1fffffff
static MALLOC_DEFINE(M_DMATEST, "bcm_dma_test", "Native DMA test owners");
static struct sx run_sx;
static struct sysctl_ctx_list sysctl_ctx;
static bool sysctls_live, unloading;
static char status[1024] = "not-run";
static struct g_class test_class;

struct io_owner {
	struct g_geom *geom;
	struct g_consumer *consumer;
	bool orphaned;
	char *allocation;
};
static struct io_owner *io_retained;

static void
orphan(struct g_consumer *consumer)
{
	struct io_owner *owner = consumer->private;

	g_topology_assert();
	/* The synchronous BIO owner retains this consumer and its access until
	 * BIO completion. Do not free a thread's consumer or buffer here.
	 */
	owner->orphaned = true;
}

static void *
high_alloc(size_t length)
{
	char *buffer;
	vm_paddr_t pa;
	size_t offset;

	buffer = contigmalloc(length, M_DMATEST, M_NOWAIT | M_ZERO,
	    HIGH_START, HIGH_END, PAGE_SIZE, 0);
	if (buffer == NULL)
		return (NULL);
	for (offset = 0; offset < length; offset += PAGE_SIZE) {
		pa = pmap_kextract((vm_offset_t)(buffer + offset));
		if (pa < HIGH_START || pa > HIGH_END - PAGE_MASK) {
			contigfree(buffer, length, M_DMATEST);
			return (NULL);
		}
	}
	return (buffer);
}

static int
bio_read_into(struct g_consumer *consumer, off_t offset, void *buffer,
    size_t length)
{
	struct bio *bio;
	int error;

	bio = g_alloc_bio();
	bio->bio_cmd = BIO_READ;
	bio->bio_offset = offset;
	bio->bio_length = length;
	bio->bio_data = buffer;
	g_io_request(bio, consumer);
	error = biowait(bio, "bcmread");
	if (error == 0 && bio->bio_completed != length)
		error = EIO;
	g_destroy_bio(bio);
	return (error);
}

static void
pattern(char *buffer, uint64_t seed, unsigned round)
{
	size_t i;

	for (i = 0; i < LENGTH; i++)
		buffer[i] = (uint8_t)((seed >> ((i & 7) * 8)) ^
		    (i * 29) ^ (i >> 9) ^ (round * 71));
}

static int
check_witness(struct bcm2711_dma_test_witness *witness)
{
	if (witness->callbacks == 0 || witness->errors != 0 ||
	    witness->covered_bytes != LENGTH || witness->bytes < LENGTH)
		return (EIO);
	return (0);
}

static int
io_cleanup(struct io_owner *owner)
{
	int error = 0;

	g_topology_lock();
	if (owner->consumer != NULL) {
		if (owner->consumer->acr || owner->consumer->acw || owner->consumer->ace)
			error = g_access(owner->consumer, -owner->consumer->acr,
			    -owner->consumer->acw, -owner->consumer->ace);
		/* g_access's negative close contract cannot fail normally. Retain
		 * every heap owner rather than detach with live access on a fault.
		 */
		if (error != 0) { g_topology_unlock(); return (error); }
		if (owner->consumer->provider != NULL)
			g_detach(owner->consumer);
		g_destroy_consumer(owner->consumer);
	}
	if (owner->geom != NULL)
		g_destroy_geom(owner->geom);
	g_topology_unlock();
	if (owner->allocation != NULL)
		contigfree(owner->allocation, LENGTH + 2 * PAGE_SIZE, M_DMATEST);
	free(owner, M_DMATEST);
	return (0);
}

static int
high_bio(void)
{
	struct io_owner *owner;
	struct g_provider *provider;
	struct bcm2711_dma_test_witness witness;
	SHA256_CTX hash;
	unsigned char expected[SHA256_DIGEST_LENGTH], actual[SHA256_DIGEST_LENGTH];
	char *allocation, *buffer;
	bus_dma_tag_t tag, physical;
	uint64_t seed;
	u_int round;
	size_t i;
	int error, cleanup_error;

	error = bcm2711_dma_test_tags(&tag, &physical);
	if (error != 0)
		return (error);
	if (maxphys < LENGTH)
		return (EOPNOTSUPP);
	owner = malloc(sizeof(*owner), M_DMATEST, M_WAITOK | M_ZERO);
	io_retained = owner;
	allocation = high_alloc(LENGTH + 2 * PAGE_SIZE);
	owner->allocation = allocation;
	if (allocation == NULL) {
		error = ENOMEM;
		goto cleanup;
	}
	buffer = allocation + PAGE_SIZE;
	memset(allocation, 0x5a, PAGE_SIZE);
	memset(buffer + LENGTH, 0xa5, PAGE_SIZE);
	g_topology_lock();
	provider = g_provider_by_name("da0");
	if (provider == NULL || strcmp(provider->geom->class->name, "DISK") != 0 ||
	    provider->mediasize != SCRATCH_SIZE || provider->sectorsize != 512) {
		error = ENXIO;
		goto unlock;
	}
	owner->geom = g_new_geomf(&test_class, "bcm-dma-scratch");
	owner->consumer = g_new_consumer(owner->geom);
	owner->consumer->private = owner;
	error = g_attach(owner->consumer, provider);
	if (error != 0)
		goto unlock;
	/* The scratch disk must have no active filesystem or other opener. */
	error = g_access(owner->consumer, 1, 1, 1);
	if (error != 0)
		goto unlock;
	g_topology_unlock();
	arc4random_buf(&seed, sizeof(seed));
	printf("BCM_DMA_TEST high source=%#jx length=%u seed=%#jx\n",
	    (uintmax_t)pmap_kextract((vm_offset_t)buffer), LENGTH, (uintmax_t)seed);
	for (round = 0; round < 8; round++) {
		pattern(buffer, seed, round);
		SHA256_Init(&hash);
		SHA256_Update(&hash, buffer, LENGTH);
		SHA256_Final(expected, &hash);
		error = bcm2711_dma_test_begin(buffer, LENGTH);
		if (error != 0)
			break;
		error = g_write_data(owner->consumer, 1024 * 1024 + round * LENGTH,
		    buffer, LENGTH);
		bcm2711_dma_test_end(&witness);
		if (error == 0)
			error = check_witness(&witness);
		if (error != 0)
			break;
		printf("BCM_DMA_TEST WRITE round=%u bytes=%ju covered=%ju callbacks=%ju first=%#jx\n",
		    round, (uintmax_t)witness.bytes, (uintmax_t)witness.covered_bytes,
		    (uintmax_t)witness.callbacks, (uintmax_t)witness.first_device);
		error = g_io_flush(owner->consumer);
		if (error != 0)
			break;
		memset(buffer, 0x93, LENGTH);
		error = bcm2711_dma_test_begin(buffer, LENGTH);
		if (error != 0)
			break;
		error = bio_read_into(owner->consumer, 1024 * 1024 + round * LENGTH,
		    buffer, LENGTH);
		bcm2711_dma_test_end(&witness);
		if (error == 0)
			error = check_witness(&witness);
		SHA256_Init(&hash);
		SHA256_Update(&hash, buffer, LENGTH);
		SHA256_Final(actual, &hash);
		if (error == 0 && timingsafe_bcmp(expected, actual, sizeof(actual)) != 0)
			error = EIO;
		if (error != 0)
			break;
		printf("BCM_DMA_TEST READ round=%u bytes=%ju covered=%ju callbacks=%ju first=%#jx\n",
		    round, (uintmax_t)witness.bytes, (uintmax_t)witness.covered_bytes,
		    (uintmax_t)witness.callbacks, (uintmax_t)witness.first_device);
	}
	for (i = 0; i < PAGE_SIZE; i++) {
		if ((uint8_t)allocation[i] != 0x5a ||
		    (uint8_t)buffer[LENGTH + i] != 0xa5)
			error = EIO;
	}
	g_topology_lock();
	if (owner->orphaned)
		error = ENXIO;
unlock:
	g_topology_unlock();
cleanup:
	cleanup_error = io_cleanup(owner);
	if (cleanup_error == 0)
		io_retained = NULL;
	if (error == 0)
		error = cleanup_error;
	if (error == 0)
		snprintf(status, sizeof(status), "PASS high-BIO 8x128KiB READ+WRITE, exact high-KVA DMA coverage, SHA256 and canaries seed=%#jx", (uintmax_t)seed);
	return (error);
}

struct pressure_owner {
	struct mtx mutex;
	struct cv completed;
	bus_dma_tag_t tag, physical;
	bus_dmamap_t holders[HOLD_COUNT], pending;
	char *buffer;
	unsigned created, loaded, callbacks, unlocks;
	bool deferred, prepared;
	unsigned expected_pages;
	int callback_error;
};
static struct pressure_owner *retained;
struct sync_result { int calls, error; };

static void
sync_callback(void *arg, bus_dma_segment_t *segments, int count, int error)
{
	struct sync_result *result = arg;

	result->calls++;
	result->error = error;
}

static void
pressure_lock(void *arg, bus_dma_lock_op_t op)
{
	struct pressure_owner *owner = arg;

	if (op == BUS_DMA_LOCK) {
		mtx_lock(&owner->mutex);
	} else {
		mtx_assert(&owner->mutex, MA_OWNED);
		owner->unlocks++;
		cv_broadcast(&owner->completed);
		mtx_unlock(&owner->mutex);
	}
}

static void
pressure_callback(void *arg, bus_dma_segment_t *segments, int count, int error)
{
	struct pressure_owner *owner = arg;
	int i;

	mtx_assert(&owner->mutex, MA_OWNED);
	owner->callbacks++;
	if (!owner->deferred || error != 0 || count <= 0)
		error = EIO;
	if (error == 0) {
		for (i = 0; i < count; i++) {
			if (segments[i].ds_addr < BCM2711_DMA_TEST_OFFSET ||
			    segments[i].ds_addr - BCM2711_DMA_TEST_OFFSET > PRIVATE_LIMIT ||
			    segments[i].ds_len == 0 || segments[i].ds_len - 1 >
			    PRIVATE_LIMIT - (segments[i].ds_addr - BCM2711_DMA_TEST_OFFSET))
				error = EIO;
		}
	}
	/* Existing API permits callback-time destruction. The production worker
	 * must retain its saved unlock function/arg, not dereference freed tags.
	 * This heap owner/mutex remains alive through the actual unlock.
	 */
	if (owner->deferred) {
		bus_dmamap_unload(owner->tag, owner->pending);
		if (bus_dmamap_destroy(owner->tag, owner->pending) == 0) {
			owner->pending = NULL;
			if (bus_dma_tag_destroy(owner->tag) == 0)
				owner->tag = NULL;
			else
				error = EBUSY;
		} else
			error = EBUSY;
	}
	owner->callback_error = error;
}

static int
pressure_cleanup(struct pressure_owner *owner)
{
	unsigned i;
	int error;

	mtx_lock(&owner->mutex);
	if (owner->deferred && owner->unlocks == 0) {
		mtx_unlock(&owner->mutex);
		return (EBUSY);
	}
	mtx_unlock(&owner->mutex);
	if (owner->pending != NULL) {
		bus_dmamap_unload(owner->tag, owner->pending);
		error = bus_dmamap_destroy(owner->tag, owner->pending);
		if (error != 0)
			return (error);
		owner->pending = NULL;
	}
	for (i = 0; i < owner->created; i++) {
		if (owner->holders[i] == NULL)
			continue;
		bus_dmamap_unload(owner->tag, owner->holders[i]);
		error = bus_dmamap_destroy(owner->tag, owner->holders[i]);
		if (error != 0)
			return (error);
		owner->holders[i] = NULL;
	}
	if (owner->tag != NULL) {
		error = bus_dma_tag_destroy(owner->tag);
		if (error != 0)
			return (error);
		owner->tag = NULL;
	}
	if (owner->physical != NULL) {
		error = bus_dma_tag_destroy(owner->physical);
		if (error != 0)
			return (error);
		owner->physical = NULL;
	}
	if (owner->buffer != NULL)
		contigfree(owner->buffer, LENGTH, M_DMATEST);
	cv_destroy(&owner->completed);
	mtx_destroy(&owner->mutex);
	free(owner, M_DMATEST);
	return (0);
}

static int
pressure_prepare(void)
{
	struct pressure_owner *owner;
	struct bcm2712_dma_policy policy;
	struct bcm2712_dma_root_window window = {0, BCM2711_DMA_TEST_OFFSET,
	    BCM2711_DMA_TEST_OFFSET, BCM2712_DMA_RAM, 0x43000000};
	bus_dma_tag_t root, physical;
	int error, cleanup_error;
	unsigned i;

	/* The guest controller must first verify no existing bounce zone with
	 * lowaddr<=PRIVATE_LIMIT and alignment>=PAGE_SIZE. See the controller.
	 * This separate parent cannot then starve GENET or SD's existing zones.
	 */
	error = bcm2711_dma_test_tags(&root, &physical);
	if (error != 0)
		return (error);
	owner = malloc(sizeof(*owner), M_DMATEST, M_WAITOK | M_ZERO);
	mtx_init(&owner->mutex, "DMA pressure callback", NULL, MTX_DEF);
	cv_init(&owner->completed, "DMA pressure callback");
	retained = owner;
	owner->buffer = high_alloc(LENGTH);
	if (owner->buffer == NULL) {
		error = ENOMEM;
		goto cleanup;
	}
	error = bus_dma_tag_create(physical, PAGE_SIZE, 0, PRIVATE_LIMIT,
	    BUS_SPACE_MAXADDR, NULL, NULL, LENGTH, 64, PAGE_SIZE, 0, NULL, NULL,
	    &owner->physical);
	if (error != 0)
		goto cleanup;
	error = bcm2712_dma_policy_root(&policy, &window, 1);
	if (error != 0)
		goto cleanup;
	error = bus_dma_tag_create_bcm2712_ram(owner->physical, &policy,
	    PAGE_SIZE, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
	    LENGTH, 64, PAGE_SIZE, 0, pressure_lock, owner, &owner->tag);
	if (error != 0)
		goto cleanup;
	for (i = 0; i < HOLD_COUNT; i++) {
		error = bus_dmamap_create(owner->tag, 0, &owner->holders[i]);
		if (error != 0)
			goto cleanup;
		owner->created++;
	}
	error = bus_dmamap_create(owner->tag, 0, &owner->pending);
	if (error != 0)
		goto cleanup;
	owner->prepared = true;
	snprintf(status, sizeof(status), "PREPARED pressure maps=%u private_limit=%#x; observe new zone before queue", owner->created + 1, PRIVATE_LIMIT);
	return (0);
cleanup:
	cleanup_error = pressure_cleanup(owner);
	if (cleanup_error == 0) retained = NULL;
	return (error != 0 ? error : cleanup_error);
}

static int
pressure_run(void)
{
	struct pressure_owner *owner = retained;
	struct sync_result sync;
	int error = 0, cleanup_error, result;
	unsigned i;

	if (owner == NULL || !owner->prepared || owner->deferred ||
	    owner->expected_pages == 0)
		return (EINVAL);
	for (i = 0; i < HOLD_COUNT; i++) {
		bzero(&sync, sizeof(sync));
		result = bus_dmamap_load(owner->tag, owner->holders[i],
		    owner->buffer, LENGTH, sync_callback, &sync, BUS_DMA_NOWAIT);
		if (result == ENOMEM && sync.calls == 1 && sync.error == ENOMEM)
			break;
		if (result != 0 || sync.calls != 1 || sync.error != 0) {
			error = EIO;
			goto cleanup;
		}
		owner->loaded++;
	}
	if (i == 0 || i == HOLD_COUNT ||
	    (uint64_t)owner->loaded * (LENGTH / PAGE_SIZE) > owner->expected_pages ||
	    owner->expected_pages - owner->loaded * (LENGTH / PAGE_SIZE) >= LENGTH / PAGE_SIZE) {
		error = EOPNOTSUPP;
		goto cleanup;
	}
	mtx_lock(&owner->mutex);
	result = bus_dmamap_load(owner->tag, owner->pending,
	    owner->buffer, LENGTH, pressure_callback, owner, BUS_DMA_WAITOK);
	if (result != EINPROGRESS || owner->callbacks != 0) {
		mtx_unlock(&owner->mutex);
		error = EIO;
		goto cleanup;
	}
	owner->deferred = true;
	if (bus_dmamap_destroy(owner->tag, owner->pending) != EBUSY ||
	    bus_dma_tag_destroy(owner->tag) != EBUSY) {
		mtx_unlock(&owner->mutex);
		error = EIO;
		goto cleanup;
	}
	/* Releasing real reservations wakes the production busdma thread. Hold
	 * its actual driver lock until all holder maps have been destroyed.
	 */
	for (i = 0; i < owner->created; i++) {
		bus_dmamap_unload(owner->tag, owner->holders[i]);
		error = bus_dmamap_destroy(owner->tag, owner->holders[i]);
		if (error != 0) {
			mtx_unlock(&owner->mutex);
			goto cleanup;
		}
		owner->holders[i] = NULL;
	}
	while (owner->unlocks == 0) {
		error = cv_timedwait(&owner->completed, &owner->mutex, 30 * hz);
		if (error != 0)
			break;
	}
	if (error == 0 && (owner->callbacks != 1 || owner->unlocks != 1 ||
	    owner->callback_error != 0 || owner->pending != NULL || owner->tag != NULL))
		error = EIO;
	mtx_unlock(&owner->mutex);
	if (error == 0)
		snprintf(status, sizeof(status), "PASS real pool pressure NOWAIT=ENOMEM WAITOK=EINPROGRESS, queued EBUSY, exactly-one callback-time map/tag destruction and saved worker unlock");
cleanup:
	/* A timeout leaves the entire callback owner in place. Module unload is
	 * EBUSY until explicit reap observes the real worker unlock.
	 */
	cleanup_error = pressure_cleanup(owner);
	if (cleanup_error == 0)
		retained = NULL;
	return (error != 0 ? error : cleanup_error);
}

static int
run_control(SYSCTL_HANDLER_ARGS)
{
	int command = 0, error;

	error = sysctl_handle_int(oidp, &command, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (command < 1 || command > 4)
		return (EINVAL);
	sx_xlock(&run_sx);
	if (unloading) {
		error = ENXIO;
	} else if (command == 3) {
		if (io_retained != NULL) {
			error = io_cleanup(io_retained);
			if (error == 0) io_retained = NULL;
		} else if (retained != NULL) {
			error = pressure_cleanup(retained);
			if (error == 0) retained = NULL;
		} else error = ENOENT;
	} else if (command == 4) {
		error = pressure_run();
	} else if (retained != NULL || io_retained != NULL) {
		error = EBUSY;
	} else {
		strlcpy(status, "running", sizeof(status));
		error = command == 1 ? high_bio() : pressure_prepare();
	}
	if (error != 0)
		snprintf(status, sizeof(status), "FAIL command=%d error=%d retained=%d", command, error, retained != NULL || io_retained != NULL);
	sx_xunlock(&run_sx);
	return (error);
}

static int
pool_pages(SYSCTL_HANDLER_ARGS)
{
	unsigned pages = 0;
	int error;

	error = sysctl_handle_int(oidp, &pages, 0, req);
	if (error != 0 || req->newptr == NULL) return (error);
	sx_xlock(&run_sx);
	if (unloading || retained == NULL || !retained->prepared || retained->deferred ||
	    pages == 0 || pages > 8192)
		error = EINVAL;
	else retained->expected_pages = pages;
	sx_xunlock(&run_sx);
	return (error);
}

static int
read_status(SYSCTL_HANDLER_ARGS)
{
	char snapshot[sizeof(status)];

	sx_slock(&run_sx);
	strlcpy(snapshot, status, sizeof(snapshot));
	sx_sunlock(&run_sx);
	return (sysctl_handle_string(oidp, snapshot, sizeof(snapshot), req));
}

static struct g_class test_class = {
	.name = "BCM_DMA_TEST", .version = G_VERSION, .orphan = orphan,
};

static int
modevent(module_t module, int event, void *arg)
{
	struct sysctl_oid *node;
	int error;

	switch (event) {
	case MOD_LOAD:
		sx_init(&run_sx, "DMA probe controls");
		error = g_modevent(module, MOD_LOAD, &test_class);
		if (error != 0) { sx_destroy(&run_sx); return (error); }
		sysctl_ctx_init(&sysctl_ctx);
		node = SYSCTL_ADD_NODE(&sysctl_ctx, SYSCTL_STATIC_CHILDREN(_debug),
		    OID_AUTO, "bcm2711_dma_probe", CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
		    "Explicit native diagnostic controls, only private16MiB scratch da0");
		if (node == NULL) { error = ENOMEM; goto load_failed; }
		if (SYSCTL_ADD_PROC(&sysctl_ctx, SYSCTL_CHILDREN(node), OID_AUTO,
		    "run", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
		    run_control, "I", "1=high BIO,2=prepare,3=reap,4=queue after observed-zone accounting") == NULL ||
		    SYSCTL_ADD_PROC(&sysctl_ctx, SYSCTL_CHILDREN(node), OID_AUTO,
		    "status", CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
		    read_status, "A", "Last completed diagnostic result") == NULL ||
		    SYSCTL_ADD_PROC(&sysctl_ctx, SYSCTL_CHILDREN(node), OID_AUTO,
		    "pool_pages", CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
		    pool_pages, "IU", "Observed isolated zone pages, required before run4") == NULL) {
			error = ENOMEM;
			goto load_failed;
		}
		sysctls_live = true;
		return (0);
	load_failed:
		sysctl_ctx_free(&sysctl_ctx);
		g_modevent(module, MOD_UNLOAD, &test_class);
		sx_destroy(&run_sx);
		return (error);
	case MOD_UNLOAD:
		if (!sx_try_xlock(&run_sx)) return (EBUSY);
		if (retained != NULL || io_retained != NULL) { sx_xunlock(&run_sx); return (EBUSY); }
		unloading = true;
		sx_xunlock(&run_sx);
		if (sysctls_live) {
			error = sysctl_ctx_free(&sysctl_ctx);
			if (error != 0) return (error);
			sysctls_live = false;
		}
		error = g_modevent(module, MOD_UNLOAD, &test_class);
		if (error != 0) return (error);
		sx_destroy(&run_sx);
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}
static moduledata_t moduledata = {"bcm2711_dma_probe", modevent, NULL};
DECLARE_MODULE(bcm2711_dma_probe, moduledata, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
MODULE_VERSION(bcm2711_dma_probe, 1);
