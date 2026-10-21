/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */

#include <stand.h>
#include <bootstrap.h>
#include <sys/endian.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <net.h>
#include <netif.h>
#include <dev_net.h>
#include <efi.h>
#include <efilib.h>
#include <Protocol/Rng.h>
#if defined(__amd64__) || defined(__i386__)
#include <machine/cpufunc.h>
#include <x86/specialreg.h>
#endif

#include "netconsole_proto.h"

#define NC_PORT 6668
#define NC_INPUT 1024
#define NC_OUTPUT 16384
static struct {
	struct devdesc dev;
	struct open_file file;
	struct iodesc io;
	struct in_addr local, peer;
	uint8_t key[32], session[16], challenge[16], peer_mac[6];
	uint8_t input[NC_INPUT], output[NC_OUTPUT];
	unsigned int ih, ic, oh, oc;
	uint8_t pending[LDNET_PACKET_SIZE], arp[60];
	size_t pending_size, arp_size;
	struct ldnet_replay input_order;
	uint64_t output_sequence, pending_sequence, ack, lost;
	uint16_t peer_port;
	time_t sent;
	bool enabled, paired, welcome, pumping;
	unsigned int echo;
} nc;
static uint8_t boot_id[16];
static bool have_boot_id;
static uint64_t session_counter;
static void nc_pump(void);
static int nc_ready(void);
static void nc_closed(void *);
extern struct console efi_console;
static void nc_probe(struct console *);
static int nc_init(int);
static void nc_out(int);
static int nc_in(void);
struct console netconsole = {
	.c_name = "netdebug",
	.c_desc = "authenticated wired debug console",
	.c_probe = nc_probe, .c_init = nc_init, .c_out = nc_out,
	.c_in = nc_in, .c_ready = nc_ready
};

static uint16_t
nc_checksum(const uint8_t *p, size_t length, uint32_t sum)
{
	while (length >= 2) {
		sum += be16dec(p);
		p += 2;
		length -= 2;
	}
	if (length != 0)
		sum += (uint16_t)*p << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ((uint16_t)~sum);
}

static int
nc_random(uint8_t *bytes, size_t length)
{
	EFI_GUID guid = EFI_RNG_PROTOCOL_GUID;
	EFI_RNG_PROTOCOL *rng;
#if defined(__amd64__) || defined(__i386__)
	u_int registers[4];
	uint32_t value, previous = 0;
	unsigned char ok;
	size_t i;
	int retry;
#endif

	if (BS->LocateProtocol(&guid, NULL, (void **)&rng) == EFI_SUCCESS &&
	    rng->GetRNG(rng, NULL, length, bytes) == EFI_SUCCESS)
		return (0);
#if defined(__amd64__) || defined(__i386__)
	do_cpuid(1, registers);
	if ((registers[2] & CPUID2_RDRAND) == 0 || length % 4 != 0)
		return (EIO);
	for (i = 0; i < length; i += 4) {
		for (retry = 0; retry < 16; retry++) {
			__asm __volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
			if (ok && value != 0 && value != UINT32_MAX &&
			    (i == 0 || value != previous))
				break;
		}
		if (retry == 16)
			return (EIO);
		memcpy(bytes + i, &value, sizeof(value));
		previous = value;
	}
	return (0);
#else
	return (EIO);
#endif
}

static void
nc_hex(char *text, const uint8_t *bytes, size_t length)
{
	static const char digits[] = "0123456789abcdef";
	size_t i;
	for (i = 0; i < length; i++) {
		text[i * 2] = digits[bytes[i] >> 4];
		text[i * 2 + 1] = digits[bytes[i] & 15];
	}
	text[length * 2] = 0;
}

static int
nc_digit(int c)
{
	if (c >= '0' && c <= '9') return (c - '0');
	if (c >= 'a' && c <= 'f') return (c - 'a' + 10);
	if (c >= 'A' && c <= 'F') return (c - 'A' + 10);
	return (-1);
}

static int
nc_key(const char *path)
{
	uint8_t bytes[66];
	ssize_t size;
	size_t total = 0;
	int fd, a, b, error = EINVAL;
	size_t i;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return (errno);
	do {
		size = read(fd, bytes + total, sizeof(bytes) - total);
		if (size > 0) total += size;
	} while (size > 0 && total < sizeof(bytes));
	close(fd);
	if (size >= 0) size = total;
	if (size == 32) {
		memcpy(nc.key, bytes, 32);
		error = 0;
	} else if (size == 64 || (size == 65 && bytes[64] == '\n')) {
		for (i = 0; i < 32; i++) {
			a = nc_digit(bytes[i * 2]); b = nc_digit(bytes[i * 2 + 1]);
			if (a < 0 || b < 0)
				break;
			nc.key[i] = (a << 4) | b;
		}
		if (i == 32)
			error = 0;
	}
	explicit_bzero(bytes, sizeof(bytes));
	if (error != 0)
		explicit_bzero(nc.key, sizeof(nc.key));
	return (error);
}

static int
nc_send(const void *packet, size_t size)
{
	uint8_t frame[14 + 20 + 8 + LDNET_PACKET_SIZE];
	uint8_t *ip = frame + 14, *udp = ip + 20;

	memset(frame, 0, sizeof(frame));
	memcpy(frame, nc.peer_mac, 6);
	memcpy(frame + 6, nc.io.myea, 6);
	be16enc(frame + 12, 0x0800);
	ip[0] = 0x45; ip[8] = 64; ip[9] = 17;
	be16enc(ip + 2, 28 + size);
	memcpy(ip + 12, &nc.local.s_addr, 4);
	memcpy(ip + 16, &nc.peer.s_addr, 4);
	be16enc(ip + 10, nc_checksum(ip, 20, 0));
	be16enc(udp, NC_PORT); be16enc(udp + 2, nc.peer_port);
	be16enc(udp + 4, 8 + size);
	/* IPv4 permits a zero UDP checksum; the full payload is authenticated. */
	memcpy(udp + 8, packet, size);
	return (efinet_console_send(frame, 42 + size));
}

static int
nc_control(uint8_t type, uint64_t sequence, const void *payload, size_t length)
{
	uint8_t packet[LDNET_PACKET_SIZE];
	size_t size;
	size = ldnet_encode(packet, sizeof(packet), nc.key, nc.session,
	    type, sequence, payload, length);
	return (size != 0 ? nc_send(packet, size) : EINVAL);
}

static int
nc_command(const struct ldnet_message *message)
{
	size_t i;
	if (message->length == 0 || message->length > 256 ||
	    message->payload[message->length - 1] != '\n')
		return (0);
	for (i = 0; i + 1 < message->length; i++)
		if (message->payload[i] != '\t' &&
		    (message->payload[i] < 32 || message->payload[i] > 126))
			return (0);
	return (1);
}

static int
nc_receive(const void *raw, size_t size, void *arg __unused)
{
	const uint8_t *frame = raw, *ip, *udp, *packet;
	const uint8_t zero[16] = { 0 };
	struct ldnet_message message;
	enum ldnet_order order;
	size_t ip_size, udp_size, length, i;
	uint8_t session[16];
	uint32_t sum;

	if (!nc.enabled || size < 14)
		return (0);
	if (be16dec(frame + 12) == 0x0806 && size >= 42) {
		const uint8_t *arp = frame + 14;
		if (be16dec(arp) != 1 || be16dec(arp + 2) != 0x0800 ||
		    arp[4] != 6 || arp[5] != 4 || be16dec(arp + 6) != 1 ||
		    memcmp(arp + 24, &nc.local.s_addr, 4) != 0)
			return (0);
		if (memcmp(frame + 6, arp + 8, 6) != 0)
			return (1);
		memset(nc.arp, 0, sizeof(nc.arp));
		memcpy(nc.arp, frame + 6, 6);
		memcpy(nc.arp + 6, nc.io.myea, 6);
		be16enc(nc.arp + 12, 0x0806);
		memcpy(nc.arp + 14, arp, 8);
		be16enc(nc.arp + 20, 2);
		memcpy(nc.arp + 22, nc.io.myea, 6);
		memcpy(nc.arp + 28, &nc.local.s_addr, 4);
		memcpy(nc.arp + 32, arp + 8, 10);
		nc.arp_size = sizeof(nc.arp);
		return (1);
	}
	if (be16dec(frame + 12) != 0x0800 || size < 42)
		return (0);
	ip = frame + 14;
	if (ip[0] != 0x45 || ip[9] != 17 ||
	    memcmp(ip + 16, &nc.local.s_addr, 4) != 0)
		return (0);
	/* Fragment payload is not a UDP header. Leave reassembly to libsa. */
	if ((be16dec(ip + 6) & 0x3fff) != 0)
		return (0);
	udp = ip + 20;
	if (be16dec(udp + 2) != NC_PORT)
		return (0);
	ip_size = be16dec(ip + 2); udp_size = be16dec(udp + 4);
	if (ip_size < 28 || ip_size > size - 14 || udp_size != ip_size - 20 ||
	    udp_size < 8 ||
	    nc_checksum(ip, 20, 0) != 0 ||
	    memcmp(ip + 12, &nc.peer.s_addr, 4) != 0)
		return (1);
	if (be16dec(udp + 6) != 0) {
		sum = 17 + udp_size;
		for (i = 12; i < 20; i += 2) sum += be16dec(ip + i);
		if (nc_checksum(udp, udp_size, sum) != 0) return (1);
	}
	packet = udp + 8; length = udp_size - 8;
	if (length < LDNET_HEADER_SIZE + LDNET_TAG_SIZE)
		return (1);
	if (packet[5] == LDNET_CHALLENGE) {
		if (!ldnet_decode(packet, length, nc.key, zero, &message))
			return (1);
		if (!nc.paired || memcmp(nc.challenge, message.payload, 16) != 0) {
			if (session_counter == UINT64_MAX ||
			    !ldnet_session(nc.key, boot_id, ++session_counter,
			    message.payload, session))
				return (1);
			memcpy(nc.session, session, 16);
			explicit_bzero(session, sizeof(session));
			memcpy(nc.challenge, message.payload, 16);
			memset(&nc.input_order, 0, sizeof(nc.input_order));
			nc.ih = 0; nc.ic = 1; nc.input[0] = 'u' & 037;
			/* Preserve unacknowledged output when replacing a session. */
			if (nc.pending_size != 0) {
				size_t n = be16dec(nc.pending + 6);
				if (n + nc.oc <= NC_OUTPUT) {
					for (i = n; i != 0; i--) {
						nc.oh = (nc.oh + NC_OUTPUT - 1) % NC_OUTPUT;
						nc.output[nc.oh] = nc.pending[LDNET_HEADER_SIZE + i - 1];
					}
					nc.oc += n;
				} else nc.lost += n;
			}
			nc.pending_size = 0; nc.output_sequence = 1; nc.ack = 0;
		}
		memcpy(nc.peer_mac, frame + 6, 6);
		nc.peer_port = be16dec(udp);
		nc.paired = true; nc.welcome = true;
		return (1);
	}
	if (!nc.paired || be16dec(udp) != nc.peer_port ||
	    !ldnet_decode(packet, length, nc.key, nc.session, &message))
		return (1);
	if (message.type == LDNET_OUTPUT_ACK && nc.pending_size != 0 &&
	    message.sequence == nc.pending_sequence) {
		nc.pending_size = 0;
	} else if (message.type == LDNET_INPUT && nc_command(&message)) {
		order = ldnet_order(&nc.input_order, &message);
		if (order == LDNET_NEXT && message.length + 1 <= NC_INPUT - nc.ic) {
			/* Cancel any partially read local/old-session command first. */
			nc.input[(nc.ih + nc.ic++) % NC_INPUT] = 'u' & 037;
			for (i = 0; i < message.length; i++) {
				nc.input[(nc.ih + nc.ic) % NC_INPUT] = message.payload[i];
				nc.ic++;
			}
			ldnet_commit(&nc.input_order, &message);
			nc.ack = message.sequence;
		} else if (order == LDNET_DUPLICATE) nc.ack = message.sequence;
	}
	return (1);
}

static void
nc_closed(void *arg __unused)
{
	nc.enabled = nc.paired = false;
	netconsole.c_flags = 0;
	nc.ic = 0; nc.echo = false;
	explicit_bzero(nc.key, sizeof(nc.key));
	explicit_bzero(nc.session, sizeof(nc.session));
}

static void
nc_pump(void)
{
	uint8_t payload[LDNET_PAYLOAD_SIZE], welcome[32];
	size_t i, length;
	int error = 0;

	if (!nc.enabled || nc.pumping)
		return;
	nc.pumping = true;
	efinet_console_poll();
	if (nc.arp_size != 0) {
		error = efinet_console_send(nc.arp, nc.arp_size);
		if (error == 0) nc.arp_size = 0;
	} else if (nc.paired && nc.welcome) {
		memcpy(welcome, nc.challenge, 16); memcpy(welcome + 16, boot_id, 16);
		error = nc_control(LDNET_WELCOME, 0, welcome, sizeof(welcome));
		if (error == 0) nc.welcome = false;
	} else if (nc.paired && nc.ack != 0) {
		error = nc_control(LDNET_ACK, nc.ack, NULL, 0);
		if (error == 0) nc.ack = 0;
	} else if (nc.paired) {
		if (nc.pending_size == 0 && nc.oc != 0 && nc.output_sequence != 0) {
			length = MIN(nc.oc, LDNET_PAYLOAD_SIZE);
			for (i = 0; i < length; i++) payload[i] = nc.output[(nc.oh + i) % NC_OUTPUT];
			nc.pending_sequence = nc.output_sequence++;
			nc.pending_size = ldnet_encode(nc.pending, sizeof(nc.pending),
			    nc.key, nc.session, LDNET_OUTPUT, nc.pending_sequence, payload, length);
			nc.oh = (nc.oh + length) % NC_OUTPUT; nc.oc -= length;
			nc.sent = getsecs() - 1;
		}
		if (nc.pending_size != 0 && getsecs() - nc.sent >= 1) {
			error = nc_send(nc.pending, nc.pending_size);
			if (error == 0) nc.sent = getsecs();
		}
	}
	nc.pumping = false;
	if (error != 0 && error != EBUSY)
		efinet_console_unbind();
}

static void
nc_probe(struct console *cp)
{
	cp->c_flags = nc.enabled ? C_PRESENTIN | C_PRESENTOUT : 0;
}

static int
nc_init(int arg __unused)
{
	if (!nc.enabled) return (1);
	efi_console.c_flags |= C_ACTIVEIN | C_ACTIVEOUT;
	return (0);
}

static void
nc_out(int c)
{
	if (!nc.enabled) return;
	if (nc.echo != 0) { nc.echo--; return; }
	if (nc.oc == NC_OUTPUT) {
		nc.oh = (nc.oh + 1) % NC_OUTPUT; nc.oc--; nc.lost++;
	}
	nc.output[(nc.oh + nc.oc++) % NC_OUTPUT] = c;
	nc_pump();
}

static int
nc_ready(void)
{
	nc_pump();
	if (nc.enabled && nc.ic == 0) BS->Stall(1000);
	return (nc.enabled && nc.ic != 0);
}

static int
nc_in(void)
{
	int c;
	if (!nc_ready()) return (-1);
	c = nc.input[nc.ih]; nc.ih = (nc.ih + 1) % NC_INPUT; nc.ic--;
	/* putchar expands ngets' newline/line-kill echo into CR and LF. */
	nc.echo = (c == '\n' || c == ('u' & 037)) ? 2 : 1;
	return (c);
}

void
netconsole_handoff(void)
{
	time_t start;
	if (!nc.enabled) return;
	printf("Network console: handing off to kernel; firmware transport ends here.\n");
	start = getsecs();
	while (nc.paired && (nc.oc != 0 || nc.pending_size != 0) && getsecs() - start < 2) {
		nc_pump(); BS->Stall(1000);
	}
	if (nc.paired) (void)nc_control(LDNET_FINISH, nc.output_sequence, NULL, 0);
	efinet_console_unbind();
}

static void
nc_cleanup(void)
{
	/* Architecture exec paths clean devices before constructing bootinfo. */
	netconsole_handoff();
	if (netdev.dv_cleanup != NULL) netdev.dv_cleanup();
}

static int
nc_start(const char *unit, const char *local, const char *peer, const char *keyfile)
{
	struct in_addr saved_ip = myip, saved_root = rootip;
	n_long saved_mask = netmask;
	struct iodesc *io;
	char *end, hex[33];
	unsigned long number;
	int error;

	if (nc.enabled) { command_errmsg = "stop the current debug console first"; return (CMD_ERROR); }
	number = strtoul(unit, &end, 10);
	if (*unit == 0 || *end != 0 || number >= (unsigned)efinetif.netif_nifs) {
		command_errmsg = "firmware network interface is unavailable"; return (CMD_ERROR);
	}
	memset(&nc, 0, sizeof(nc));
	nc.local.s_addr = inet_addr(__DECONST(char *, local));
	nc.peer.s_addr = inet_addr(__DECONST(char *, peer));
	if (nc.local.s_addr == 0 || nc.local.s_addr == INADDR_NONE ||
	    nc.peer.s_addr == 0 || nc.peer.s_addr == INADDR_NONE ||
	    nc.local.s_addr == nc.peer.s_addr) {
		command_errmsg = "supply distinct static IPv4 addresses"; return (CMD_ERROR);
	}
	if (nc_key(keyfile) != 0) { command_errmsg = "cannot read a 32-byte or 64-hex debug key file"; return (CMD_ERROR); }
	if (!have_boot_id) {
		if (nc_random(boot_id, sizeof(boot_id)) != 0) {
			nc_closed(NULL); command_errmsg = "no usable firmware/CPU entropy"; return (CMD_ERROR);
		}
		have_boot_id = true;
	}
	if (myip.s_addr != 0 && myip.s_addr != nc.local.s_addr) {
		nc_closed(NULL); command_errmsg = "debug address differs from active netboot interface"; return (CMD_ERROR);
	}
	myip = nc.local;
	if (rootip.s_addr == 0) rootip = nc.peer;
	if (netmask == 0) netmask = htonl(0xffffff00);
	nc.dev.d_dev = &efinet_dev; nc.dev.d_unit = number;
	nc.file.f_devdata = &nc.dev;
	error = efinet_dev.dv_open(&nc.file, &nc.dev);
	if (error != 0 || nc.dev.d_opendata == NULL) goto failed;
	io = socktodesc(*(int *)nc.dev.d_opendata);
	if (io == NULL || io->io_netif == NULL || io->io_netif->nif_unit != (int)number) goto failed;
	nc.io = *io;
	if (nc.io.io_netif->nif_devdata == NULL) goto failed;
	nc.io.myip = nc.local;
	if (efinet_console_bind(&nc.io, nc_receive, nc_closed, NULL) != 0) goto failed;
	efinet_dev.dv_cleanup = nc_cleanup;
	nc.enabled = true;
	netconsole.c_flags = C_PRESENTIN | C_PRESENTOUT | C_ACTIVEIN | C_ACTIVEOUT;
	efi_console.c_flags |= C_ACTIVEIN | C_ACTIVEOUT;
	nc_hex(hex, boot_id, 16); setenv("netdebug.boot_id", hex, 1);
	printf("Wired debug console enabled on net%lu; waiting for authenticated controller.\n", number);
	return (CMD_OK);
failed:
	if (saved_root.s_addr == 0 && netdev.dv_cleanup != NULL) netdev.dv_cleanup();
	myip = saved_ip; rootip = saved_root; netmask = saved_mask;
	nc_closed(NULL); command_errmsg = "cannot attach firmware network console";
	return (CMD_ERROR);
}

static int
command_netdebug(int argc, char *argv[])
{
	unsigned int i;
	if (argc == 2 && strcmp(argv[1], "devices") == 0) {
		for (i = 0; i < (unsigned)efinetif.netif_nifs; i++) printf("net%u: firmware Ethernet interface\n", i);
		if (efinetif.netif_nifs == 0) printf("No firmware-visible Ethernet interfaces.\n");
		return (CMD_OK);
	}
	if (argc == 2 && strcmp(argv[1], "stop") == 0) {
		efinet_console_unbind(); nc_closed(NULL);
		printf("Wired debug console revoked.\n"); return (CMD_OK);
	}
	if (argc == 2 && strcmp(argv[1], "auto") == 0) {
		const char *unit = getenv("netdebug.interface"), *ip = getenv("netdebug.ip");
		const char *peer = getenv("netdebug.peer"), *key = getenv("netdebug.keyfile");
		if (ip != NULL && peer != NULL && key != NULL) return (nc_start(unit != NULL ? unit : "0", ip, peer, key));
		command_errmsg = "set netdebug.ip, netdebug.peer and netdebug.keyfile"; return (CMD_ERROR);
	}
	if (argc == 6 && strcmp(argv[1], "start") == 0) return (nc_start(argv[2], argv[3], argv[4], argv[5]));
	if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
		printf("Wired debug console: %s; controller %s; lost output bytes %ju; normal RX drops %ju\n",
		    nc.enabled ? "enabled" : "disabled", nc.paired ? "paired" : "absent",
		    (uintmax_t)nc.lost, (uintmax_t)efinet_console_drops());
		return (CMD_OK);
	}
	command_errmsg = "netdebug devices|status|stop|auto|start UNIT LOCAL-IP PEER-IP KEY-FILE";
	return (CMD_ERROR);
}
COMMAND_SET(netdebug, "netdebug", "approved wired loader console", command_netdebug);
