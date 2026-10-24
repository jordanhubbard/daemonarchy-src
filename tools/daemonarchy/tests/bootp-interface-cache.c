/* SPDX-License-Identifier: BSD-2-Clause */
/* Observe production DHCP's first outgoing message without a network server. */
#include <stand/libsa/bootp.c>
#include <assert.h>

struct in_addr myip, rootip, gateip;
n_long netmask;
u_int intf_mtu;
int netproto, rootport, errno, hostnamelen, domainnamelen;
char rootpath[FNAME_SIZE], bootfile[FNAME_SIZE];
char hostname[FNAME_SIZE], domainname[FNAME_SIZE];
static struct iodesc descriptor;
static int first_message;

struct iodesc *socktodesc(int sock) { return (&descriptor); }
time_t getsecs(void) { return (1); }
char *intoa(n_long ip) { return ("0.0.0.0"); }
ssize_t sendudp(struct iodesc *d, void *p, size_t n) { return (n); }
ssize_t readudp(struct iodesc *d, void **p, void **v, time_t t)
{ return (-1); }

ssize_t
sendrecv(struct iodesc *d,
    ssize_t (*sendfn)(struct iodesc *, void *, size_t), void *data, size_t len,
    ssize_t (*recvfn)(struct iodesc *, void **, void **, time_t, void *),
    void **packet, void **payload, void *extra)
{

	if (first_message == 0)
		first_message = ((struct bootp *)data)->bp_vend[6];
	return (-1);
}

static void
prepare(void)
{

	free(bootp_response);
	bootp_response = calloc(1, sizeof(*bootp_response));
	assert(bootp_response != NULL);
	bootp_response_size = sizeof(*bootp_response);
	bootp_response->bp_htype = 1;
	bootp_response->bp_hlen = sizeof(descriptor.myea);
	memcpy(bootp_response->bp_chaddr, descriptor.myea,
	    sizeof(descriptor.myea));
	bootp_response->bp_yiaddr.s_addr = htonl(1);
	first_message = 0;
}

int
main(void)
{

	descriptor.myea[5] = 1;
	prepare();
	bootp(0);
	assert(first_message == DHCPREQUEST);
	prepare();
	bootp_response->bp_chaddr[5] = 2;
	bootp(0);
	assert(first_message == DHCPDISCOVER);
	prepare();
	bootp_response->bp_hlen = 0;
	bootp(0);
	assert(first_message == DHCPDISCOVER);
	prepare();
	bootp_response->bp_htype = 0;
	bootp(0);
	assert(first_message == DHCPDISCOVER);
	prepare();
	bootp_response->bp_yiaddr.s_addr = 0;
	bootp(0);
	assert(first_message == DHCPDISCOVER);
	prepare();
	free(bootp_response);
	bootp_response = malloc(1);
	bootp_response_size = 1;
	bootp(0);
	assert(first_message == DHCPDISCOVER);
	free(bootp_response);
	printf("DHCP cache interface identity, valid reuse and truncated-cache checks passed\n");
	return (0);
}
