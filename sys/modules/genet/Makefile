.PATH: ${SRCTOP}/sys/arm64/broadcom/genet

KMOD=	if_genet
SRCS=	if_genet.c
SRCS+=	bus_if.h device_if.h gpio_if.h miibus_if.h ofw_bus_if.h syscon_if.h
SRCS+=	opt_device_polling.h opt_platform.h opt_acpi.h
SRCS+=	acpi_if.h

.include <bsd.kmod.mk>
