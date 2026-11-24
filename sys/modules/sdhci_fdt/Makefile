#

.PATH: ${SRCTOP}/sys/dev/sdhci

KMOD=	sdhci_fdt
SRCS=	sdhci_fdt.c sdhci_fdt_gpio.c sdhci_fdt_bcm2712.c sdhci_if.h device_if.h bus_if.h mmcbr_if.h opt_mmccam.h opt_cam.h gpio_if.h

SRCS+= ofw_bus_if.h fdt_pinctrl_if.h clknode_if.h regnode_if.h phynode_if.h opt_platform.h

.include <bsd.kmod.mk>
