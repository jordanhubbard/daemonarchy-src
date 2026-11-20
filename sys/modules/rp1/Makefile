.PATH: ${SRCTOP}/sys/arm64/broadcom
KMOD= rp1
SRCS= rp1_pci.c rp1_gpio.c
SRCS+= bus_if.h device_if.h ofw_bus_if.h pci_if.h pcib_if.h pic_if.h gpio_if.h
SRCS+= opt_platform.h opt_global.h
.include <bsd.kmod.mk>
