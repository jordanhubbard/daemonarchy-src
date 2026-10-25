/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */
/*
 * Diagnostic, polled console for a forced mountroot prompt.  The interface
 * belongs exclusively to debugnet until mountroot completes; it must not be
 * used as a normal networking interface during that interval.
 */
#include <sys/param.h>
#include <sys/cons.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/systm.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/if_types.h>
#include <sys/epoch.h>
#include <net/debugnet.h>
#include <netinet/in.h>

#define DNC_PORT 26003
#define DNC_CLIENT_PORT 26004
#define DNC_BUFSIZE 512

static struct debugnet_pcb *dnc_pcb;
static if_t dnc_ifp;
static eventhandler_tag dnc_mountroot_tag;
static unsigned char dnc_input[DNC_BUFSIZE];
static unsigned int dnc_head, dnc_tail;
static uint32_t dnc_input_last_seq;
static char dnc_output[DNC_BUFSIZE];
static size_t dnc_output_len;
static bool dnc_busy;

static int
dnc_rx(struct mbuf *m)
{
	uint32_t net_seq, seq;
	uint32_t len;
	unsigned int tail;
	int i;

	/* debugnet already consumed its transport header before this callback. */
	if (m->m_pkthdr.len < sizeof(net_seq))
		return (EINVAL);
	m_copydata(m, 0, sizeof(net_seq), (void *)&net_seq);
	seq = ntohl(net_seq);
	if (seq <= dnc_input_last_seq)
		return (0);
	len = m->m_pkthdr.len - sizeof(net_seq);
	if (len > DNC_BUFSIZE)
		return (EINVAL);
	if (len > (dnc_head + DNC_BUFSIZE - dnc_tail - 1) % DNC_BUFSIZE)
		return (ENOBUFS);
	for (i = 0; i < len; i++) {
		tail = (dnc_tail + 1) % DNC_BUFSIZE;
		dnc_tail = tail;
		m_copydata(m, sizeof(net_seq) + i, 1,
		    (void *)&dnc_input[(tail + DNC_BUFSIZE - 1) % DNC_BUFSIZE]);
	}
	dnc_input_last_seq = seq;
	return (0);
}

static void
dnc_flush(void)
{
	size_t len;

	if (dnc_pcb == NULL || dnc_busy || dnc_output_len == 0)
		return;
	dnc_busy = true;
	len = dnc_output_len;
	dnc_output_len = 0;
	(void)debugnet_send(dnc_pcb, DEBUGNET_DATA, dnc_output, len, NULL);
	dnc_busy = false;
}

static void
dnc_probe(struct consdev *cn)
{
	cn->cn_pri = CN_DEAD;
}

static void dnc_init(struct consdev *cn __unused) {}
static void dnc_term(struct consdev *cn __unused) {}
static void dnc_grab(struct consdev *cn __unused) {}
static void dnc_ungrab(struct consdev *cn __unused) {}

static int
dnc_getc(struct consdev *cn __unused)
{
	int ch;

	if (dnc_pcb == NULL || dnc_busy)
		return (-1);
	dnc_flush();
	if (dnc_head == dnc_tail) {
		dnc_busy = true;
		debugnet_network_poll(dnc_pcb);
		dnc_busy = false;
	}
	if (dnc_head == dnc_tail)
		return (-1);
	ch = dnc_input[dnc_head];
	dnc_head = (dnc_head + 1) % DNC_BUFSIZE;
	return (ch);
}

static void
dnc_putc(struct consdev *cn __unused, int ch)
{
	if (dnc_pcb == NULL || dnc_busy)
		return;
	/* cnputc may run in a critical section: transmit only from getc. */
	if (dnc_output_len == sizeof(dnc_output)) {
		memmove(dnc_output, dnc_output + 1,
		    sizeof(dnc_output) - 1);
		dnc_output_len--;
	}
	dnc_output[dnc_output_len++] = ch;
}

static const struct consdev_ops dnc_ops = {
	.cn_probe = dnc_probe,
	.cn_init = dnc_init,
	.cn_term = dnc_term,
	.cn_getc = dnc_getc,
	.cn_putc = dnc_putc,
	.cn_grab = dnc_grab,
	.cn_ungrab = dnc_ungrab,
};
static struct consdev dnc_console = {
	.cn_ops = &dnc_ops,
	.cn_pri = CN_REMOTE,
	.cn_name = "daemonarchy_netconsole",
};

static void
dnc_close(void *arg __unused)
{
	if (dnc_pcb == NULL)
		return;
	dnc_flush();
	cnremove(&dnc_console);
	(void)debugnet_sendempty(dnc_pcb, DEBUGNET_FINISHED);
	debugnet_free(dnc_pcb);
	dnc_pcb = NULL;
	if_rele(dnc_ifp);
	dnc_ifp = NULL;
	if (dnc_mountroot_tag != NULL) {
		/* Called by mountroot itself: waiting for this invocation deadlocks. */
		EVENTHANDLER_DEREGISTER_NOWAIT(mountroot, dnc_mountroot_tag);
		dnc_mountroot_tag = NULL;
	}
}

static int
dnc_match_if(if_t ifp, void *arg)
{
	struct ifreq ifr;
	const unsigned char *ea;
	char mac[18];

	if (!DEBUGNET_SUPPORTED_NIC(ifp) || if_gethwaddr(ifp, &ifr) != 0)
		return (0);
	ea = (const unsigned char *)ifr.ifr_addr.sa_data;
	snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
	    ea[0], ea[1], ea[2], ea[3], ea[4], ea[5]);
	if (strcmp(mac, arg) != 0)
		return (0);
	if_ref(ifp);
	dnc_ifp = ifp;
	return (1);
}

static void
dnc_rootconf(void *arg __unused)
{
	struct debugnet_conn_params cfg;
	struct in_addr local, peer;
	char *mac, *ip, *server, *enabled, *boot_id;
	struct epoch_tracker et;
	int error;

	enabled = kern_getenv("netdebug.plain");
	if (enabled == NULL)
		return;
	if (strcmp(enabled, "1") != 0) {
		freeenv(enabled);
		return;
	}
	freeenv(enabled);
	mac = kern_getenv("netdebug.hwaddr");
	ip = kern_getenv("netdebug.local_ip");
	server = kern_getenv("netdebug.peer_ip");
	boot_id = kern_getenv("netdebug.boot_id");
	if (mac == NULL || ip == NULL || server == NULL || boot_id == NULL ||
	    inet_aton(ip, &local) == 0 || inet_aton(server, &peer) == 0)
		goto done;
	NET_EPOCH_ENTER(et);
	(void)if_foreach(dnc_match_if, mac);
	NET_EPOCH_EXIT(et);
	if (dnc_ifp == NULL)
		goto done;
	if ((if_getflags(dnc_ifp) & IFF_UP) == 0) {
		if_setflagbits(dnc_ifp, IFF_UP, 0);
		if_init(dnc_ifp, if_getsoftc(dnc_ifp));
	}
	debugnet_any_ifnet_update(dnc_ifp);
	cfg = (struct debugnet_conn_params) {
		.dc_ifp = dnc_ifp,
		.dc_client = local.s_addr,
		.dc_server = peer.s_addr,
		.dc_gateway = peer.s_addr,
		.dc_herald_port = DNC_PORT,
		.dc_client_port = DNC_CLIENT_PORT,
		.dc_herald_data = boot_id,
		.dc_herald_datalen = strlen(boot_id),
		.dc_rx_handler = dnc_rx,
	};
	error = debugnet_connect(&cfg, &dnc_pcb);
	if (error != 0)
		goto done;
	dnc_mountroot_tag = EVENTHANDLER_REGISTER(mountroot, dnc_close, NULL,
	    EVENTHANDLER_PRI_LAST);
	if (cnadd(&dnc_console) != 0)
		dnc_close(NULL);
done:
	if (dnc_pcb == NULL && dnc_ifp != NULL) {
		if_rele(dnc_ifp);
		dnc_ifp = NULL;
	}
	if (mac != NULL)
		freeenv(mac);
	if (ip != NULL)
		freeenv(ip);
	if (server != NULL)
		freeenv(server);
	if (boot_id != NULL)
		freeenv(boot_id);
}
SYSINIT(daemonarchy_netconsole, SI_SUB_ROOT_CONF, SI_ORDER_LAST, dnc_rootconf,
    NULL);
