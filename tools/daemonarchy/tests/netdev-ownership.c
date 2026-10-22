/* SPDX-License-Identifier: BSD-2-Clause */
/* Production ordinary-device cache, with deterministic transport callbacks. */
#include <stand/common/dev_net.c>
#include <assert.h>

struct in_addr myip, rootip, gateip;
n_long netmask;
u_int intf_mtu;
int netproto, rootport, errno;
char rootpath[FNAME_SIZE];
struct netif_driver *netif_drivers[] = { NULL };
static struct iodesc descriptor;
static unsigned opens, closes;
static unsigned configurations;

int
netif_open(void *hint)
{

	opens++;
	rootip.s_addr = htonl(1);
	return (((struct devdesc *)hint)->d_unit + 1);
}

int
netif_close(int sock)
{

	assert(sock > 0);
	closes++;
	return (0);
}

struct iodesc *socktodesc(int sock) { return (&descriptor); }
void netif_configure(int sock) { configurations++; }
void bootp(int sock) { assert(0); }
int rarp_getipaddress(int sock) { assert(0); return (EIO); }
int bp_whoami(int sock) { assert(0); return (EIO); }
int bp_getfile(int sock, char *key, struct in_addr *ip, char *path)
{ assert(0); return (EIO); }
n_long ip_convertaddr(char *text) { assert(0); return (0); }
char *ether_sprintf(u_char *mac) { return ("00:00:00:00:00:00"); }
char *intoa(n_long ip) { return ("0.0.0.0"); }
int pager_output(const char *text) { return (0); }
int noioctl(struct open_file *f, u_long cmd, void *data) { return (EINVAL); }

int
main(void)
{
	struct devsw driver = { .dv_name = "fixture" };
	struct devdesc a = { .d_dev = &driver, .d_unit = 0 };
	struct devdesc a2 = { .d_dev = &driver, .d_unit = 0 };
	struct devdesc b = { .d_dev = &driver, .d_unit = 1 };
	struct open_file first = { .f_devdata = &a };
	struct open_file second = { .f_devdata = &a2 };
	struct open_file other = { .f_devdata = &b };

	assert(net_open(&first, &a) == 0);
	assert(net_open(&second, &a2) == 0);
	assert(opens == 1 && closes == 0 && netdev_opens == 2);
	assert(configurations == 1);
	assert(net_open(&other, &b) == EBUSY);
	assert(net_configure(&b) == EBUSY);
	assert(net_close(&first) == 0 && netdev_opens == 1);
	assert(net_open(&other, &b) == EBUSY);
	assert(net_close(&second) == 0 && netdev_opens == 0);
	assert(net_open(&other, &b) == 0);
	assert(opens == 2 && closes == 1 && netdev_unit == 1);
	assert(net_close(&other) == 0 && netdev_opens == 0);
	assert(net_configure(&a) == 0);
	assert(opens == 3 && closes == 2 && netdev_unit == 0);
	assert(configurations == 3);
	net_cleanup();
	assert(closes == 3 && netdev_sock == -1 && netdev_name == NULL);
	printf("netdev active-file refusal, close accounting and unit switching passed\n");
	return (0);
}
