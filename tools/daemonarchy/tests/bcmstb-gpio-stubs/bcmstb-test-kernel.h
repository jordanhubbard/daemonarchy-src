/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BCMSTB_TEST_KERNEL_H
#define BCMSTB_TEST_KERNEL_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
typedef uint32_t phandle_t;
typedef uint32_t pcell_t;
struct resource { uint64_t start, size; uint32_t words[16]; };
struct mtx { bool initialized, locked; };
struct device { void *softc; phandle_t node; bool attached; };
typedef struct device *device_t;
typedef int device_method_t;
typedef struct { const char *name; device_method_t *methods; size_t size; } driver_t;
#define DEVMETHOD(name, fn) 0
#define DEVMETHOD_END 0
#define EARLY_DRIVER_MODULE(...)
#define MODULE_DEPEND(...)
#define MODULE_VERSION(...)
#define BUS_PROBE_DEFAULT 0
#define SYS_RES_MEMORY 1
#define RF_ACTIVE 1
#define MTX_DEF 0
#define BUS_SPACE_BARRIER_WRITE 2
#define GPIOMAXNAME 64
#define GPIO_PIN_INPUT 1u
#define GPIO_PIN_OUTPUT 2u
#define GPIO_PIN_OPENDRAIN 4u
#define GPIO_PIN_PULLUP 32u
#define GPIO_PIN_PRESET_LOW 0x400u
#define GPIO_PIN_PRESET_HIGH 0x800u
#define GPIO_INTR_EDGE_RISING 0x40000u
void *device_get_softc(device_t);
const char *device_get_nameunit(device_t);
void device_set_desc(device_t, const char *);
void device_printf(device_t, const char *, ...);
bool device_is_attached(device_t);
int device_delete_children(device_t);
struct resource *bus_alloc_resource_any(device_t, int, int *, int);
int bus_release_resource(device_t, int, int, struct resource *);
void bus_attach_children(device_t);
uint32_t bus_read_4(struct resource *, unsigned);
void bus_write_4(struct resource *, unsigned, uint32_t);
void bus_barrier(struct resource *, unsigned, unsigned, int);
uint64_t rman_get_start(struct resource *);
uint64_t rman_get_size(struct resource *);
void mtx_init(struct mtx *, const char *, const char *, int);
void mtx_destroy(struct mtx *);
void mtx_lock(struct mtx *);
void mtx_unlock(struct mtx *);
phandle_t ofw_bus_get_node(device_t);
bool ofw_bus_status_okay(device_t);
bool ofw_bus_is_compatible(device_t, const char *);
bool ofw_bus_node_status_okay(phandle_t);
bool ofw_bus_node_is_compatible(phandle_t, const char *);
phandle_t OF_parent(phandle_t);
phandle_t OF_child(phandle_t);
phandle_t OF_peer(phandle_t);
phandle_t OF_xref_from_node(phandle_t);
device_t OF_device_from_xref(phandle_t);
int OF_getproplen(phandle_t, const char *);
int OF_getencprop(phandle_t, const char *, void *, int);
bool OF_hasprop(phandle_t, const char *);
device_t gpiobus_add_bus(device_t);
int gpiobus_detach_bus(device_t);
int bcmstb_test_mux(device_t, device_t, uint32_t, uint32_t);
#define FDT_PINCTRL_SET_FLAGS(p, g, n, f) bcmstb_test_mux(p, g, n, f)
#endif
