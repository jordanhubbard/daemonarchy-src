/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/queue.h>
typedef uint32_t phandle_t;
struct device;struct devclass;struct driverlink;
typedef struct device *device_t;
typedef struct devclass *devclass_t;
typedef struct driverlink *driverlink_t;
typedef struct driver {const char *name;} driver_t;
TAILQ_HEAD(driver_list,driverlink);
struct devclass {const char *name;struct driver_list drivers;devclass_t parent;};
struct driverlink {driver_t *driver;int pass;TAILQ_ENTRY(driverlink) link;};
struct device {devclass_t devclass;device_t parent;driver_t *driver;int state,unit,devflags,flags,node;};
struct bcmstb_gpio_softc {device_t busdev;};
static struct bcmstb_gpio_softc sc;
static device_t controller;
static phandle_t (*node_method)(device_t,device_t);
static unsigned probes;
static int bus_current_pass=100007;
#define PDEBUG(x) ((void)0)
#define DEVCLANAME(dc) ((dc)->name)
#define DRIVERNAME(d) ((d)->name)
#define DS_ALIVE 1
#define BUS_PROBE_DEFAULT 0
#define BUS_PROBE_NOWILDCARD -1000
#define DF_FIXEDCLASS 1
#define DBT_KIND_PROBE 1
#define DBT_DEVICE(...) ((void)0)
#define DEVICE_PROBE(d) (probes++,ofw_gpiobus_probe(d))
static void bus_topo_assert(void) {}
static void panic(const char *s) {(void)s;assert(false);}
static int device_set_driver(device_t d,driver_t *driver) {d->driver=driver;return 0;}
static int device_set_devclass(device_t d,const char *s) {(void)d;(void)s;assert(false);return EINVAL;}
static const char *device_get_name(device_t d) {return d->devclass?d->devclass->name:NULL;}
static void *device_get_softc(device_t d) {assert(d==controller);return &sc;}
static void resource_int_value(const char *name,int unit,const char *key,int *out) {(void)name;(void)unit;(void)key;*out=0;}
static void device_verbose(device_t d) {(void)d;}
static void bus_data_generation_update(void) {}
static int ofw_gpiobus_probe(device_t);
static phandle_t ofw_bus_get_node(device_t d) {return d==controller?d->node:node_method(d->parent,d);}
static void device_set_desc(device_t d,const char *s) {(void)d;(void)s;}
