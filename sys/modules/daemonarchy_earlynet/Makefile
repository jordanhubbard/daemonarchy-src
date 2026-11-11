# Daemonarchy early debug transport; normally built into DAEMONARCHY_DIAGNOSTIC.
.PATH: ${SRCTOP}/sys/dev/daemonarchy/earlynet

KMOD=	daemonarchy_earlynet
SRCS=	earlynet_core.c earlynet_em.c earlynet_vtnet.c earlynet_kern.c
SRCS+=	bus_if.h device_if.h pci_if.h

.include <bsd.kmod.mk>
