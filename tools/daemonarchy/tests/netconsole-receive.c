/* SPDX-License-Identifier: BSD-2-Clause */
/* Exercise the production Ethernet demultiplexer, including NFS fragments. */
#define main efi_application_main
#define setenv fixture_setenv
#include <stand/efi/loader/netconsole.c>
#undef main
#undef setenv
#include <assert.h>

extern int setenv(const char *, const char *, int);
static unsigned env_calls, fail_env_call;
int
fixture_setenv(const char *name, const char *value, int overwrite)
{
	if (++env_calls == fail_env_call)
		return (-1);
	return (setenv(name, value, overwrite));
}

/* Sanitizer global registration retains console callbacks. None may run here. */
struct console efi_console;
EFI_BOOT_SERVICES *BS;
time_t getsecs(void) { assert(0); return (0); }
void efinet_console_poll(void) { assert(0); }
void efinet_console_unbind(void) { assert(0); }
int efinet_console_send(const void *p, size_t n) { assert(0); return (EIO); }

int
main(void)
{
	uint8_t frame[64] = { 0 }, *ip = frame + 14, *payload = ip + 20;
	unsigned int i;
	const uint16_t fragmented[] = { 1, 0x2000, 0x2001, 0x1fff };

	nc.enabled = true;
	nc.local.s_addr = htonl(0xac10210a);
	nc.peer.s_addr = htonl(0xac102114);
	be16enc(frame + 12, 0x0800);
	ip[0] = 0x45;
	ip[9] = 17;
	be16enc(ip + 2, sizeof(frame) - 14);
	memcpy(ip + 12, &nc.peer.s_addr, 4);
	memcpy(ip + 16, &nc.local.s_addr, 4);
	/* NFS data can look like a UDP header at a noninitial fragment offset. */
	be16enc(payload + 2, NC_PORT);
	be16enc(payload + 4, sizeof(frame) - 34);
	for (i = 0; i < sizeof(fragmented) / sizeof(fragmented[0]); i++) {
		be16enc(ip + 6, fragmented[i]);
		be16enc(ip + 10, 0);
		be16enc(ip + 10, nc_checksum(ip, 20, 0));
		assert(nc_receive(frame, sizeof(frame), NULL) == 0);
		assert(nc.ic == 0 && !nc.paired && nc.arp_size == 0);
	}
	/* An ordinary NFS UDP datagram is also left to the network stack. */
	be16enc(ip + 6, 0x4000); /* Don't-fragment is not fragmentation. */
	be16enc(ip + 10, 0);
	be16enc(ip + 10, nc_checksum(ip, 20, 0));
	be16enc(payload + 2, 2049);
	assert(nc_receive(frame, sizeof(frame), NULL) == 0);
	/* Unfragmented malformed debug traffic is consumed, never queued as input. */
	be16enc(payload + 2, NC_PORT);
	assert(nc_receive(frame, sizeof(frame), NULL) == 1);
	assert(nc.ic == 0 && !nc.paired);
	memcpy(nc.io.myea, "\x52\x54\x00\x33\x00\x10", 6);
	memset(boot_id, 0xab, sizeof(boot_id));
	assert(nc_export_identity() == 0);
	assert(strcmp(getenv("netdebug.hwaddr"), "52:54:00:33:00:10") == 0);
	assert(strcmp(getenv("netdebug.local_ip"), "172.16.33.10") == 0);
	assert(strcmp(getenv("netdebug.peer_ip"), "172.16.33.20") == 0);
	assert(strcmp(getenv("netdebug.boot_id"), "abababababababababababababababab") == 0);
	assert(setenv("boot.netif.hwaddr", "52:54:00:33:00:99", 1) == 0);
	assert(strcmp(getenv("netdebug.hwaddr"), "52:54:00:33:00:10") == 0);
	/* Each partial export failure must remove all snapshot fields. */
	for (i = 1; i <= 4; i++) {
		env_calls = 0; fail_env_call = 0;
		assert(nc_export_identity() == 0);
		env_calls = 0; fail_env_call = i;
		assert(nc_export_identity() == ENOMEM);
		assert(getenv("netdebug.hwaddr") == NULL);
		assert(getenv("netdebug.local_ip") == NULL);
		assert(getenv("netdebug.peer_ip") == NULL);
		assert(getenv("netdebug.boot_id") == NULL);
	}
	unsetenv("boot.netif.hwaddr");
	printf("netconsole fragment and handoff identity checks passed\n");
	return (0);
}
