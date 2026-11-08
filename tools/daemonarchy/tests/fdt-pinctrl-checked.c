/* SPDX-License-Identifier: BSD-2-Clause */
/* Compile the actual fdt_pinctrl.c against fault-injected OF/provider services.
 * This is helper behavior coverage, not native provider or hardware acceptance.
 */
#include <sys/param.h>
#include <assert.h>
#include <stdlib.h>
#include <limits.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/fdt/fdt_pinctrl.h>

int bootverbose;
static struct device client = { 1, 0 }, providers[] = { { 2, 1 }, { 3, 2 } };
static phandle_t refs[3];
static const char *names;
static int nameslen, refslen, alloc_failure, calls, lookups, live_allocs;
static unsigned expected_index;
static unsigned available;
static int provider_error[2];
static phandle_t configured[3];

static void *
allocate(size_t size)
{
	void *p = malloc(size);
	assert(p != NULL);
	live_allocs++;
	return (p);
}

void *
pinctrl_test_mallocarray(size_t count, size_t size)
{
	assert(count <= SIZE_MAX / size);
	return (allocate(count * size));
}

void
pinctrl_test_free(void *p)
{
	if (p != NULL) {
		assert(live_allocs > 0);
		live_allocs--;
		free(p);
	}
}

phandle_t ofw_bus_get_node(device_t d) { return (d->node); }
void OF_prop_free(void *p) { pinctrl_test_free(p); }

int
OF_getproplen(phandle_t node, const char *name)
{
	char expected[32];
	assert(node == client.node);
	snprintf(expected, sizeof(expected), "pinctrl-%u", expected_index);
	assert(strcmp(name, expected) == 0);
	return (refslen);
}

ssize_t
OF_getencprop_alloc_multi(phandle_t node, const char *name, int size, void **p)
{
	int len = OF_getproplen(node, name);
	*p = NULL;
	assert(size == sizeof(phandle_t));
	if (len < 0 || len % size != 0 || alloc_failure)
		return (-1);
	if (len > 0) {
		*p = allocate(len);
		memcpy(*p, refs, len);
	}
	return (len / size);
}

ssize_t
OF_getprop_alloc(phandle_t node, const char *name, void **p)
{
	assert(node == client.node && strcmp(name, "pinctrl-names") == 0);
	*p = NULL;
	if (nameslen > 0) {
		*p = allocate(nameslen);
		memcpy(*p, names, nameslen);
	}
	return (nameslen);
}

device_t
OF_device_from_xref(phandle_t xref)
{
	lookups++;
	if (xref >= 10 && xref <= 11 && (available & (1u << (xref - 10))))
		return (&providers[xref - 10]);
	return (NULL);
}

int
pinctrl_test_configure(device_t provider, phandle_t xref)
{
	assert(provider == &providers[0] || provider == &providers[1]);
	assert(calls < 3);
	configured[calls++] = xref;
	return (provider_error[provider->number - 1]);
}

/* Unused tree traversal services remain linked: the production translation
 * unit, including legacy registration/tree helpers, is compiled unmodified.
 */
phandle_t OF_child(phandle_t n) { (void)n; return (0); }
phandle_t OF_peer(phandle_t n) { (void)n; return (0); }
int OF_hasprop(phandle_t n, const char *p) { (void)n; (void)p; return (0); }
void OF_device_register_xref(phandle_t x, device_t d) { (void)x; (void)d; }
phandle_t OF_xref_from_node(phandle_t n) { return (n); }
int ofw_bus_node_status_okay(phandle_t n) { (void)n; return (1); }
int OF_getprop(phandle_t n, const char *p, void *b, int s)
{ (void)n; (void)p; (void)b; (void)s; return (-1); }

static void
reset(void)
{
	assert(live_allocs == 0);
	refs[0] = 10;
	refs[1] = 11;
	refs[2] = 10;
	refslen = 2 * sizeof(phandle_t);
	names = "default";
	nameslen = 8;
	available = 3;
	calls = lookups = alloc_failure = 0;
	expected_index = 0;
	provider_error[0] = provider_error[1] = 0;
}

int
main(void)
{
	/* A missing second provider must invoke neither provider. */
	reset(); available = 1;
	assert(fdt_pinctrl_configure_checked(&client, 0) == ENXIO);
	assert(calls == 0 && lookups == 2);
	reset(); available = 2;
	assert(fdt_pinctrl_configure_checked(&client, 0) == ENXIO && calls == 0);
	/* Provider failures must retain their actual error, not report success. */
	reset(); provider_error[0] = EBUSY;
	assert(fdt_pinctrl_configure_checked(&client, 0) == EBUSY && calls == 1);
	reset(); provider_error[1] = EIO;
	assert(fdt_pinctrl_configure_checked(&client, 0) == EIO && calls == 2);
	reset();
	assert(fdt_pinctrl_configure_checked(&client, 0) == 0 && calls == 2);
	assert(configured[0] == 10 && configured[1] == 11);
	/* Property shape is checked before a provider can touch hardware. */
	reset(); refslen = -1;
	assert(fdt_pinctrl_configure_checked(&client, 0) == ENOENT && calls == 0);
	reset(); refslen = 3;
	assert(fdt_pinctrl_configure_checked(&client, 0) == EINVAL && calls == 0);
	reset(); refslen = 0;
	assert(fdt_pinctrl_configure_checked(&client, 0) == 0 && lookups == 0);
	reset(); alloc_failure = 1;
	assert(fdt_pinctrl_configure_checked(&client, 0) == EIO && calls == 0);
	reset(); expected_index = UINT_MAX;
	assert(fdt_pinctrl_configure_checked(&client, UINT_MAX) == 0 && calls == 2);
	/* Resolve index by name, then use exactly the same preflight contract. */
	reset(); names = "sleep\0default"; nameslen = 14; expected_index = 1;
	assert(fdt_pinctrl_configure_by_name_checked(&client, "default") == 0);
	assert(calls == 2);
	reset(); available = 1;
	assert(fdt_pinctrl_configure_by_name_checked(&client, "default") == ENXIO);
	assert(calls == 0);
	reset(); provider_error[0] = EACCES;
	assert(fdt_pinctrl_configure_by_name_checked(&client, "default") == EACCES);
	assert(calls == 1);
	reset(); refslen = 0;
	assert(fdt_pinctrl_configure_by_name_checked(&client, "default") == 0);
	assert(calls == 0);
	reset(); nameslen = -1;
	assert(fdt_pinctrl_configure_by_name_checked(&client, "default") == ENOENT);
	reset(); nameslen = 0;
	assert(fdt_pinctrl_configure_by_name_checked(&client, "default") == ENOENT);
	reset();
	assert(fdt_pinctrl_configure_by_name_checked(&client, "other") == ENOENT);
	assert(fdt_pinctrl_configure_by_name_checked(&client, NULL) == EINVAL);
	assert(fdt_pinctrl_configure_by_name_checked(&client, "") == EINVAL);
	assert(calls == 0);
	reset(); nameslen = 7; /* Missing terminator. */
	assert(fdt_pinctrl_configure_by_name_checked(&client, "default") == EINVAL);
	assert(calls == 0);
	reset(); names = "default\0bad"; nameslen = 11; /* Bad suffix after match. */
	assert(fdt_pinctrl_configure_by_name_checked(&client, "default") == EINVAL);
	assert(calls == 0);
	reset(); names = "\0default"; nameslen = 9;
	assert(fdt_pinctrl_configure_by_name_checked(&client, "default") == EINVAL);
	assert(calls == 0);
	/* Explicit legacy controls prevent accidentally changing global behavior. */
	reset(); available = 1; provider_error[0] = EIO;
	assert(fdt_pinctrl_configure(&client, 0) == 0 && calls == 1);
	reset(); available = 1; provider_error[0] = EIO;
	assert(fdt_pinctrl_configure_by_name(&client, "default") == 0 && calls == 1);
	reset();
	puts("PASS: actual fdt_pinctrl helpers preflight all providers, propagate errors, validate names/cells, preserve empty states and legacy semantics");
	return (0);
}
