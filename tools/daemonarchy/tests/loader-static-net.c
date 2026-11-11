/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Static loader network parameters and the TFTP server port, around the
 * verbatim production net_parse_rootpath(), net_static_params() and
 * tftp_server_port().  A debug link without DHCP must reach the
 * controller's TFTP server; an invalid or partial setting must refuse
 * rather than fall back to an unintended server.
 */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#error "Static network checks require assertions"
#endif

#define	FNAME_SIZE	128
#define	IPPORT_TFTP	69
#define	DEBUG_PRINTF(level, args)	do { } while (0)
#define	bcopy(src, dst, len)	memmove((dst), (src), (len))
#define	NETPROTO_DEFAULT	NET_NFS
#define	nitems(x)	(sizeof((x)) / sizeof((x)[0]))
typedef uint32_t n_long;
enum net_proto { NET_NONE, NET_NFS, NET_TFTP };
struct iodesc { struct in_addr myip; };

static char rootpath[FNAME_SIZE];
static int netproto, rootport;
static struct in_addr myip, rootip, gateip;
static n_long netmask;
static struct iodesc socket0;
static const char *env[8][2];

static struct iodesc *
socktodesc(int sock)
{
	return (sock == 0 ? &socket0 : NULL);
}

static char *
getenv_stub(const char *name)
{
	for (int i = 0; i < 8 && env[i][0] != NULL; i++)
		if (strcmp(env[i][0], name) == 0)
			return ((char *)env[i][1]);
	return (NULL);
}
#define	getenv(name)	getenv_stub(name)

/* PRODUCTION_FUNCTIONS */

static void
reset(const char *ip, const char *path, const char *mask)
{
	memset(env, 0, sizeof(env));
	int n = 0;
	if (ip != NULL) { env[n][0] = "net.static.ip"; env[n++][1] = ip; }
	if (path != NULL) { env[n][0] = "net.static.rootpath"; env[n++][1] = path; }
	if (mask != NULL) { env[n][0] = "net.static.netmask"; env[n][1] = mask; }
	rootpath[0] = 0; netproto = NET_NONE; rootport = 0;
	myip.s_addr = rootip.s_addr = gateip.s_addr = 0; netmask = 0;
	socket0.myip.s_addr = 0;
}

static void
tftp_with_port(void)
{
	reset("192.0.2.2", "tftp://192.0.2.1:6969/", NULL);
	assert(net_static_params(0) == 0);
	assert(netproto == NET_TFTP && rootport == 6969);
	assert(rootip.s_addr == inet_addr("192.0.2.1"));
	assert(myip.s_addr == inet_addr("192.0.2.2"));
	assert(socket0.myip.s_addr == myip.s_addr);
	assert(netmask == htonl(0xffffff00) && gateip.s_addr == 0);
	assert(strcmp(rootpath, "/") == 0);
	assert(tftp_server_port() == 6969);
}

static void
default_port_and_mask(void)
{
	reset("10.1.2.3", "tftp://10.1.0.9/boot/", "255.255.0.0");
	assert(net_static_params(0) == 0);
	assert(rootport == 0 && tftp_server_port() == IPPORT_TFTP);
	assert(netmask == htonl(0xffff0000));
	assert(strcmp(rootpath, "/boot/") == 0);
}

static void
not_configured(void)
{
	reset(NULL, NULL, NULL);
	assert(net_static_params(0) == ENOENT);
	assert(myip.s_addr == 0 && rootip.s_addr == 0);
}

static void
refused(void)
{
	static const char *bad[][3] = {
		{ "192.0.2.2", NULL, NULL },			/* partial */
		{ NULL, "tftp://192.0.2.1/", NULL },		/* partial */
		{ "not-an-ip", "tftp://192.0.2.1/", NULL },
		{ "192.0.2.2", "tftp://198.51.100.1/", NULL },	/* off link */
		{ "192.0.2.2", "/no/server", NULL },
		{ "192.0.2.2", "tftp://192.0.2.1/", "garbage" },
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		reset(bad[i][0], bad[i][1], bad[i][2]);
		assert(net_static_params(0) == EINVAL);
		assert(myip.s_addr == 0 && rootip.s_addr == 0);
		assert(socket0.myip.s_addr == 0);
	}
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } cases[] = {
		{ "tftp-port", tftp_with_port },
		{ "defaults", default_port_and_mask },
		{ "not-configured", not_configured },
		{ "refused", refused },
	};
	if (argc != 2)
		return (2);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		if (strcmp(argv[1], cases[i].name) == 0) {
			cases[i].run();
			printf("%s: ok\n", argv[1]);
			return (0);
		}
	return (2);
}
