/* SPDX-License-Identifier: BSD-2-Clause */
#include <sys/param.h>
phandle_t ofw_bus_get_node(device_t);
device_t OF_device_from_xref(phandle_t);
ssize_t OF_getencprop_alloc_multi(phandle_t, const char *, int, void **);
ssize_t OF_getprop_alloc(phandle_t, const char *, void **);
int OF_getproplen(phandle_t, const char *);
void OF_prop_free(void *);
phandle_t OF_child(phandle_t);
phandle_t OF_peer(phandle_t);
int OF_hasprop(phandle_t, const char *);
void OF_device_register_xref(phandle_t, device_t);
phandle_t OF_xref_from_node(phandle_t);
int ofw_bus_node_status_okay(phandle_t);
int OF_getprop(phandle_t, const char *, void *, int);
