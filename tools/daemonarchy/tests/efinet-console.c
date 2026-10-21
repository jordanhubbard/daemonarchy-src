/* SPDX-License-Identifier: BSD-2-Clause */
/* Exercise the actual driver with scripted SNP/boot-service callbacks. */
#include <assert.h>
#ifdef NDEBUG
#error "Fault checks require assertions"
#endif
#define malloc fault_malloc
#define calloc fault_calloc
#define free fault_free
#define main efi_application_main
#include "stand/efi/libefi/efinet.c"
#undef main
#undef malloc
#undef calloc
#undef free
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
static size_t live_allocations;
static bool allocation_failure;

void *
fault_malloc(size_t size)
{
	void *p;
	if (allocation_failure) return (NULL);
	p = malloc(size);
	if (p != NULL) live_allocations++;
	return (p);
}
void *
fault_calloc(size_t count, size_t size)
{
	void *p;
	if (allocation_failure) return (NULL);
	p = calloc(count, size);
	if (p != NULL) live_allocations++;
	return (p);
}
void
fault_free(void *p)
{
	if (p != NULL) {
		assert(live_allocations != 0);
		live_allocations--;
	}
	free(p);
}

/* These satisfy the unchanged driver table's unused initialization paths.
 * The harness must fail if a test accidentally enters those paths. */
struct in_addr rootip, myip, gateip, servip;
n_long netmask;
char rootpath[FNAME_SIZE];
struct bootp *bootp_response;
size_t bootp_response_size;

int
efi_status_to_errno(EFI_STATUS status)
{
	(void)status;
	assert(!"unexpected EFI initialization path");
	return (EIO);
}

EFI_STATUS
OpenProtocolByHandle(EFI_HANDLE handle, EFI_GUID *guid, void **out)
{
	(void)handle; (void)guid; (void)out;
	assert(!"unexpected EFI initialization path");
	return (EFI_DEVICE_ERROR);
}

uint32_t
net_parse_rootpath(void)
{
	assert(!"unexpected network initialization path");
	return (0);
}

/* Sanitizer registration retains the device table as well. */
uint16_t bootp_client_arch;
struct devsw netdev;

EFI_DEVICE_PATH *
efi_lookup_devpath(EFI_HANDLE handle)
{
	(void)handle; assert(!"unexpected device enumeration"); return (NULL);
}
EFI_DEVICE_PATH *
efi_devpath_last_node(EFI_DEVICE_PATH *path)
{
	(void)path; assert(!"unexpected device enumeration"); return (NULL);
}
int
efi_register_handles(struct devsw *dev, EFI_HANDLE *a, EFI_HANDLE *b, int n)
{
	(void)dev; (void)a; (void)b; (void)n;
	assert(!"unexpected device enumeration"); return (EIO);
}
EFI_HANDLE
efi_find_handle(struct devsw *dev, int unit)
{
	(void)dev; (void)unit; assert(!"unexpected device enumeration"); return (NULL);
}
CHAR16 *
efi_devpath_name(EFI_DEVICE_PATH *path)
{
	(void)path; assert(!"unexpected device enumeration"); return (NULL);
}
void
efi_free_devpath_name(CHAR16 *name)
{
	(void)name; assert(!"unexpected device enumeration");
}
int
pager_output(const char *text)
{
	(void)text; assert(!"unexpected pager"); return (EIO);
}
int
noioctl(struct open_file *file, u_long command, void *data)
{
	(void)file; (void)command; (void)data;
	assert(!"unexpected ioctl"); return (EIO);
}
void
nullsys(void)
{
	assert(!"unexpected device cleanup");
}

static EFI_BOOT_SERVICES services;
EFI_BOOT_SERVICES *BS = &services;
EFI_HANDLE IH;
static EFI_SIMPLE_NETWORK_MODE mode;
static EFI_SIMPLE_NETWORK protocol;
static struct netif_dif interface;
static struct netif nif;
static struct iodesc desc;
static uint64_t clock_us;
static EFI_STATUS transmit_status, completion_status, shutdown_status;
static void *completion, *owned;
static size_t owned_size;
static unsigned transmits, receives, closes, revocations;
static unsigned receive_count, receive_index;
static unsigned status_polls, complete_after;
static bool ordinary_completion, consume_even;
static EFI_STATUS receive_status;

static EFI_STATUS EFIAPI
fake_transmit(EFI_SIMPLE_NETWORK *self, UINTN header, UINTN size, void *buffer,
    EFI_MAC_ADDRESS *source, EFI_MAC_ADDRESS *destination, UINT16 *type)
{
	(void)self; (void)header; (void)source; (void)destination; (void)type;
	transmits++;
	if (transmit_status == EFI_SUCCESS) {
		owned = buffer;
		owned_size = size;
	}
	return (transmit_status);
}

static EFI_STATUS EFIAPI
fake_status(EFI_SIMPLE_NETWORK *self, UINT32 *interrupts, void **returned)
{
	(void)self; (void)interrupts;
	status_polls++;
	clock_us += 1000;
	if (ordinary_completion && status_polls >= complete_after)
		completion = owned;
	*returned = completion;
	completion = NULL;
	return (completion_status);
}

static EFI_STATUS EFIAPI
fake_receive(EFI_SIMPLE_NETWORK *self, UINTN *header, UINTN *size, void *buffer,
    EFI_MAC_ADDRESS *source, EFI_MAC_ADDRESS *destination, UINT16 *type)
{
	(void)self; (void)header; (void)source; (void)destination; (void)type;
	receives++;
	clock_us += 1000;
	if (receive_status != EFI_SUCCESS) return (receive_status);
	if (receive_index == receive_count)
		return (EFI_NOT_READY);
	assert(*size >= 60);
	memset(buffer, 0, 60);
	((uint8_t *)buffer)[0] = receive_index++;
	*size = 60;
	return (EFI_SUCCESS);
}

static EFI_STATUS EFIAPI
fake_shutdown(EFI_SIMPLE_NETWORK *self)
{
	(void)self;
	return (shutdown_status);
}

static EFI_STATUS EFIAPI
fake_close(EFI_HANDLE handle, EFI_GUID *guid, EFI_HANDLE agent,
    EFI_HANDLE controller)
{
	(void)handle; (void)guid; (void)agent; (void)controller;
	closes++;
	return (EFI_SUCCESS);
}

static EFI_STATUS EFIAPI
fake_stall(UINTN microseconds)
{
	clock_us += microseconds;
	return (EFI_SUCCESS);
}

time_t
getsecs(void)
{
	return (clock_us / 1000000);
}

static int
filter(const void *packet, size_t size, void *arg)
{
	(void)arg;
	assert(size == 60);
	return (consume_even && (*(const uint8_t *)packet % 2) == 0);
}

static void
revoked(void *arg)
{
	(void)arg;
	revocations++;
}

static void
reset(void)
{
	/* Reset is a fresh simulated firmware instance, never a recovery shortcut. */
	assert(console_io.count == 0 && live_allocations == 0);
	memset(&console_io, 0, sizeof(console_io));
	memset(&services, 0, sizeof(services));
	memset(&protocol, 0, sizeof(protocol));
	memset(&nif, 0, sizeof(nif));
	memset(&desc, 0, sizeof(desc));
	mode.MaxPacketSize = 1500;
	protocol.Mode = &mode;
	protocol.Transmit = fake_transmit;
	protocol.GetStatus = fake_status;
	protocol.Receive = fake_receive;
	protocol.Shutdown = fake_shutdown;
	services.Stall = fake_stall;
	services.CloseProtocol = fake_close;
	nif.nif_driver = &efinetif;
	nif.nif_devdata = &protocol;
	desc.io_netif = &nif;
	efinetif.netif_ifs = &interface;
	clock_us = 1000000;
	transmits = receives = closes = revocations = 0;
	receive_count = receive_index = status_polls = complete_after = 0;
	ordinary_completion = consume_even = allocation_failure = false;
	receive_status = EFI_SUCCESS;
	transmit_status = completion_status = shutdown_status = EFI_SUCCESS;
	completion = owned = NULL;
	owned_size = 0;
}

static void
test_timeout_and_late_completion(void)
{
	uint8_t source[60], second[60], snapshot[60];
	void *private_buffer;

	reset();
	assert(efinet_console_handoff_check() == 0);
	assert(efinet_console_bind(&desc, filter, revoked, NULL) == 0);
	assert(efinet_console_handoff_check() == EIO);
	memset(source, 0xa5, sizeof(source));
	memset(second, 0x5a, sizeof(second));
	assert(efinet_console_send(source, sizeof(source)) == 0);
	assert(transmits == 1 && owned != source && owned_size == sizeof(source));
	private_buffer = owned;
	memcpy(snapshot, owned, sizeof(snapshot));
	memset(source, 0, sizeof(source));
	assert(memcmp(owned, snapshot, sizeof(snapshot)) == 0);
	assert(efinet_console_send(second, sizeof(second)) == EBUSY);
	assert(transmits == 1 && owned == private_buffer);
	clock_us += 2000000;
	assert(efinet_console_send(second, sizeof(second)) == EIO);
	assert(console_io.pending && console_io.fault);
	assert(memcmp(private_buffer, snapshot, sizeof(snapshot)) == 0);
	efinet_console_unbind();
	assert(revocations == 1 && console_io.pending);
	assert(efinet_console_bind(&desc, filter, revoked, NULL) == EBUSY);
	completion = private_buffer;
	assert(console_reap() == EIO);
	assert(!console_io.pending && console_io.fault);
	assert(efinet_console_bind(&desc, filter, revoked, NULL) == 0);
	assert(!console_io.fault);
	assert(efinet_console_send(second, sizeof(second)) == 0);
	assert(transmits == 2);
	efinet_end(&nif);
	assert(!console_io.pending && revocations == 2 && closes == 1);
	assert(efinet_console_handoff_check() == 0);
	printf("PASS owned buffer, timeout, unbind and late completion\n");
}

static void
test_status_and_transmit_errors(void)
{
	uint8_t bytes[60], snapshot[60];
	void *buffer;

	reset();
	assert(efinet_console_send(bytes, sizeof(bytes)) == ENXIO);
	assert(efinet_console_bind(&desc, filter, revoked, NULL) == 0);
	assert(efinet_console_send(bytes, 0) == EINVAL);
	assert(efinet_console_send(bytes, CONSOLE_FRAME_MAX + 1) == EINVAL);
	assert(transmits == 0);
	transmit_status = EFI_NOT_READY;
	assert(efinet_console_send(bytes, sizeof(bytes)) == EBUSY);
	assert(!console_io.pending);
	transmit_status = EFI_DEVICE_ERROR;
	assert(efinet_console_send(bytes, sizeof(bytes)) == EIO);
	assert(!console_io.pending);
	transmit_status = EFI_SUCCESS;
	memset(bytes, 0x47, sizeof(bytes));
	assert(efinet_console_send(bytes, sizeof(bytes)) == 0);
	buffer = owned;
	memcpy(snapshot, buffer, sizeof(snapshot));
	completion_status = EFI_DEVICE_ERROR;
	completion = buffer; /* Error plus a pointer is not successful completion. */
	assert(console_reap() == EIO);
	assert(console_io.pending && console_io.fault);
	assert(efinet_console_send(bytes, sizeof(bytes)) == EIO);
	assert(transmits == 3 && memcmp(buffer, snapshot, sizeof(snapshot)) == 0);
	completion_status = EFI_SUCCESS;
	completion = buffer;
	assert(console_reap() == EIO && !console_io.pending);
	efinet_console_unbind();
	assert(efinet_console_bind(&desc, filter, revoked, NULL) == 0);
	assert(!console_io.fault);
	efinet_end(&nif);
	printf("PASS transmit refusal, error completion and explicit recovery\n");
}

static void
test_shared_transmit_and_shutdown(void)
{
	uint8_t bytes[60], snapshot[60];
	void *buffer;

	memset(bytes, 0x53, sizeof(bytes));
	reset();
	assert(efinet_console_bind(&desc, filter, revoked, NULL) == 0);
	ordinary_completion = true;
	complete_after = 3;
	assert(efinet_put(&desc, bytes, sizeof(bytes)) == sizeof(bytes));
	assert(transmits == 1 && owned != bytes && !console_io.pending);
	assert(status_polls == 3);
	efinet_end(&nif);
	reset();
	assert(efinet_console_bind(&desc, filter, revoked, NULL) == 0);
	assert(efinet_console_send(bytes, sizeof(bytes)) == 0);
	buffer = owned;
	memcpy(snapshot, buffer, sizeof(snapshot));
	assert(efinet_put(&desc, bytes, sizeof(bytes)) == -1);
	assert(transmits == 1 && status_polls < 2500);
	assert(console_io.pending && memcmp(buffer, snapshot, sizeof(snapshot)) == 0);
	shutdown_status = EFI_DEVICE_ERROR;
	efinet_end(&nif);
	assert(console_io.quarantined && console_io.pending);
	assert(efinet_console_handoff_check() == EIO);
	assert(console_io.nif == NULL && nif.nif_devdata == NULL);
	assert(revocations == 1 && closes == 1);
	assert(memcmp(buffer, snapshot, sizeof(snapshot)) == 0);
	/* Even another NIC cannot recover/reuse a quarantined DMA buffer. */
	nif.nif_devdata = &protocol;
	assert(efinet_console_bind(&desc, filter, revoked, NULL) == EBUSY);
	assert(transmits == 1);
	reset();
	assert(efinet_console_bind(&desc, filter, revoked, NULL) == 0);
	shutdown_status = EFI_DEVICE_ERROR;
	efinet_end(&nif);
	assert(console_io.quarantined && !console_io.pending);
	assert(efinet_console_handoff_check() == EIO);
	assert(revocations == 1 && closes == 1);
	printf("PASS shared synchronous transmit and failed-shutdown quarantine\n");
}

static void
test_receive_queue_and_poll_bounds(void)
{
	void *packet;
	unsigned i;

	reset();
	assert(efinet_console_bind(&desc, filter, revoked, NULL) == 0);
	packet = NULL;
	assert(efinet_receive(&desc, &packet, -1) == -1 && receives == 0);
	assert(efinet_receive(&desc, &packet, 0) == -1 && receives == 1);
	allocation_failure = true;
	unsigned before_failure = receives;
	assert(efinet_receive(&desc, &packet, 0) == -1);
	assert(receives == before_failure && live_allocations == 0);
	allocation_failure = false;
	receive_count = 20;
	for (i = 0; i < 5; i++) efinet_console_poll();
	assert(receive_index == 20 && console_io.count == 16);
	assert(efinet_console_drops() == 4);
	efinet_console_unbind();
	assert(revocations == 1 && console_io.count == 16);
	/* Rebinding this same NIC retains its already queued normal packets. */
	assert(efinet_console_bind(&desc, filter, revoked, NULL) == 0);
	for (i = 0; i < 16; i++) {
		packet = NULL;
		assert(efinet_get(&desc, &packet, 0) == 60);
		assert(((uint8_t *)packet)[ETHER_ALIGN] == i);
		fault_free(packet);
	}
	assert(console_io.count == 0);
	receive_count = receive_index + 3;
	efinet_console_poll();
	assert(console_io.count == 3);
	efinet_end(&nif);
	assert(console_io.count == 0 && revocations == 2);
	reset();
	assert(efinet_console_bind(&desc, filter, revoked, NULL) == 0);
	consume_even = true;
	receive_count = 6;
	efinet_console_poll(); efinet_console_poll();
	assert(console_io.count == 3);
	for (i = 0; i < 3; i++) {
		assert(efinet_get(&desc, &packet, 0) == 60);
		assert(((uint8_t *)packet)[ETHER_ALIGN] == i * 2 + 1);
		fault_free(packet);
	}
	receive_status = EFI_DEVICE_ERROR;
	assert(efinet_receive(&desc, &packet, 0) == -1);
	receive_status = EFI_BUFFER_TOO_SMALL;
	assert(efinet_receive(&desc, &packet, 0) == -1);
	receive_status = EFI_SUCCESS;
	unsigned before = receives;
	assert(efinet_receive(&desc, &packet, 2) == -1);
	assert(receives - before <= 2001);
	efinet_end(&nif);
	reset();
	/* The unattached path retains its ordinary caller-owned TX contract. */
	ordinary_completion = true;
	complete_after = 2;
	uint8_t bytes[60] = {0};
	assert(efinet_put(&desc, bytes, sizeof(bytes)) == sizeof(bytes));
	assert(owned == bytes && status_polls == 2);
	efinet_end(&nif);
	printf("PASS RX ordering, overflow, zero/negative polling and unattached path\n");
}

static void
test_invalid_firmware_mode(void)
{
	reset();
	mode.MaxPacketSize = UINT32_MAX;
	assert(efinet_console_bind(&desc, filter, revoked, NULL) != 0);
	mode.MaxPacketSize = 0;
	assert(efinet_console_bind(&desc, filter, revoked, NULL) != 0);
	mode.MaxPacketSize = CONSOLE_FRAME_MAX - ETHER_HDR_LEN - ETHER_CRC_LEN + 1;
	assert(efinet_console_bind(&desc, filter, revoked, NULL) != 0);
	protocol.Mode = NULL;
	assert(efinet_console_bind(&desc, filter, revoked, NULL) != 0);
	void *packet = NULL;
	assert(efinet_receive(&desc, &packet, 0) == -1);
	assert(receives == 0);
	protocol.Mode = &mode;
	mode.MaxPacketSize = 1500;
	assert(efinet_console_bind(&desc, filter, revoked, NULL) == 0);
	/* The mode may disappear or change after attachment too. */
	protocol.Mode = NULL;
	assert(efinet_receive(&desc, &packet, 0) == -1);
	protocol.Mode = &mode;
	mode.MaxPacketSize = UINT32_MAX;
	assert(efinet_receive(&desc, &packet, 0) == -1);
	assert(receives == 0 && transmits == 0 && !console_io.pending);
	efinet_end(&nif);
	printf("PASS invalid firmware mode and frame-size overflow refusal\n");
}

int
main(void)
{
	test_timeout_and_late_completion();
	test_status_and_transmit_errors();
	test_shared_transmit_and_shutdown();
	test_receive_queue_and_poll_bounds();
	test_invalid_firmware_mode();
	assert(live_allocations == 0);
	printf("efinet console fault checks passed\n");
	return (0);
}
