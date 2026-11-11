/* SPDX-License-Identifier: BSD-2-Clause */
/* Host fault harness: run via run-regnode-lifetime.py. The marked sections are
 * replaced by actual source definitions, including allocation-bearing types.
 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
#define FDT 1
#define KOBJ_FIELDS struct kobj_ops *ops
struct kobj_class { size_t size; int refs; };
struct kobj_ops { struct kobj_class *cls; };
typedef struct kobj_class *regnode_class_t;
typedef struct kobj_class *kobj_class_t;
struct kobj { struct kobj_ops *ops; };
typedef struct kobj *kobj_t;
struct sx { int live; int locked; };
typedef int phandle_t;
struct device { phandle_t node; void *softc; };
typedef struct device *device_t;
struct regnode;
struct regulator;
typedef TAILQ_HEAD(regnode_list, regnode) regnode_list_t;
typedef TAILQ_HEAD(regulator_list, regulator) regulator_list_t;
struct sysctl_oid { TAILQ_ENTRY(sysctl_oid) link; const void *arg; };
TAILQ_HEAD(sysctl_ctx_list, sysctl_oid);
/* PRODUCTION_TYPES */

static regnode_list_t regnode_list = TAILQ_HEAD_INITIALIZER(regnode_list);
static int topo, sx_live, sysctl_live, sysctl_error, init_error;
static int pin_get_failure = -1, pin_init_failure = -1, pin_configs, pin_releases;
static int parse_error, published, init_ok, registered_xref, class_refs;
static struct regnode *last_node;
static struct kobj_class gpioregulator_regnode_class = { sizeof(struct gpioregulator_reg_sc), 0 };
#define M_REGULATOR 1
#define M_DEVBUF 2
#define M_OFWPROP 3
#define M_GPIO 4
#define M_SYSCTL 5
#define M_WAITOK 1
#define M_ZERO 2
#define GPIO_PIN_OUTPUT 2
struct gpiobus_pin { int index; };
struct allocation { void *pointer; int pool; };
static struct allocation allocations[256];
static int live_allocations;

static void *
test_malloc(size_t size, int pool, int flags)
{
	void *p = flags & M_ZERO ? calloc(1, size) : malloc(size);
	assert(p != NULL);
	for (unsigned i = 0; i < 256; i++) {
		if (allocations[i].pointer == NULL) {
			allocations[i] = (struct allocation){ p, pool };
			live_allocations++;
			return (p);
		}
	}
	abort();
}
static void
test_free(void *p, int pool)
{
	if (p == NULL)
		return;
	/* A dangling diagnostic owner must be caught before libc free. */
	if (pool == M_REGULATOR)
		assert(sysctl_live == 0);
	for (unsigned i = 0; i < 256; i++) {
		if (allocations[i].pointer == p) {
			assert(allocations[i].pool == pool);
			allocations[i].pointer = NULL;
			live_allocations--;
			free(p);
			return;
		}
	}
	abort();
}
static char *
test_strdup(const char *s, int pool)
{
	char *p = test_malloc(strlen(s) + 1, pool, 0);
	strcpy(p, s);
	return (p);
}
#define malloc(s, p, f) test_malloc(s, p, f)
#define free(p, t) test_free(p, t)
#define strdup(s, p) test_strdup(s, p)
#define __DECONST(t, p) ((t)(uintptr_t)(p))
#define KASSERT(c, m) assert(c)
#define REG_TOPO_SLOCK() do { assert(topo == 0); topo = 1; } while (0)
#define REG_TOPO_XLOCK() do { assert(topo == 0); topo = 2; } while (0)
#define REG_TOPO_UNLOCK() do { assert(topo != 0); topo = 0; } while (0)
#define REG_TOPO_ASSERT() assert(topo != 0)
#define REGNODE_XLOCK(n) do { assert(!(n)->lock.locked); (n)->lock.locked = 1; } while (0)
#define REGNODE_UNLOCK(n) do { assert((n)->lock.locked); (n)->lock.locked = 0; } while (0)
static void sx_init(struct sx *s, const char *name)
{ (void)name; assert(!s->live); s->live = 1; sx_live++; }
static void sx_destroy(struct sx *s)
{ assert(s->live && !s->locked && sysctl_live == 0); s->live = 0; sx_live--; }
static void
kobj_init(kobj_t obj, kobj_class_t cls)
{
	obj->ops = test_malloc(sizeof(*obj->ops), M_REGULATOR, 0);
	obj->ops->cls = cls;
	cls->refs++;
	class_refs++;
}
static void
kobj_delete(kobj_t obj, int pool)
{
	assert(obj->ops->cls->refs > 0);
	obj->ops->cls->refs--;
	class_refs--;
	test_free(obj->ops, M_REGULATOR);
	test_free(obj, pool);
}
static struct sysctl_oid *
test_sysctl_add(struct sysctl_ctx_list *ctx, struct sysctl_oid *parent,
    const char *name, const void *arg)
{
	(void)parent;
	if (last_node != NULL && (last_node->flags & REGULATOR_FLAGS_DEFER_SYSCTL)) {
		assert(init_ok && registered_xref && !TAILQ_EMPTY(&regnode_list));
		assert(topo == 0); /* No allocation/public callback under topo lock. */
	}
	struct sysctl_oid *oid = test_malloc(sizeof(*oid), M_SYSCTL, M_ZERO);
	oid->arg = arg;
	TAILQ_INSERT_TAIL(ctx, oid, link);
	sysctl_live++;
	if (!strcmp(name, "uvolt"))
		assert(arg != NULL);
	return (oid);
}
static void sysctl_ctx_init(struct sysctl_ctx_list *ctx) { TAILQ_INIT(ctx); }
static int
sysctl_ctx_free(struct sysctl_ctx_list *ctx)
{
	assert(topo == 0); /* Draining while holding topo can deadlock a handler. */
	if (sysctl_error)
		return (sysctl_error);
	struct sysctl_oid *oid;
	while ((oid = TAILQ_FIRST(ctx)) != NULL) {
		TAILQ_REMOVE(ctx, oid, link);
		sysctl_live--;
		test_free(oid, M_SYSCTL);
	}
	return (0);
}
#define SYSCTL_STATIC_CHILDREN(x) NULL
#define SYSCTL_CHILDREN(x) (x)
#define SYSCTL_ADD_NODE(c,p,i,n,f,a,d) test_sysctl_add(c,p,n,NULL)
#define SYSCTL_ADD_INT(c,p,i,n,f,a,v,d) test_sysctl_add(c,p,n,a)
#define SYSCTL_ADD_U8(c,p,i,n,f,a,v,d) test_sysctl_add(c,p,n,a)
#define SYSCTL_ADD_PROC(c,p,i,n,f,a,v,h,t,d) test_sysctl_add(c,p,n,a)
static int gpioregulator_regnode_init(struct regnode *);
static int
mock_init(struct regnode *node)
{
	last_node = node;
	if (node->flags & REGULATOR_FLAGS_DEFER_SYSCTL)
		assert(TAILQ_EMPTY(&node->sysctl_ctx));
	int error = init_error ? init_error : gpioregulator_regnode_init(node);
	init_ok = error == 0;
	return (error);
}
#define REGNODE_INIT(n) mock_init(n)
#define panic(...) abort()
static void *device_get_softc(device_t d) { return (d->softc); }
static void device_printf(device_t d, const char *fmt, ...) { (void)d; (void)fmt; }
static phandle_t ofw_bus_get_node(device_t d) { return (d->node); }
static phandle_t OF_xref_from_node(phandle_t n) { return (n); }
static void OF_device_register_xref(phandle_t n, device_t d)
{ assert(n > 0 && d != NULL && init_ok); registered_xref++; published++; }
static void OF_prop_free(void *p) { test_free(p, M_OFWPROP); }
static void *regnode_get_softc(struct regnode *n) { return (n->softc); }
static struct regnode_std_param *regnode_get_stdparam(struct regnode *n)
{ return (&n->std_param); }
static int
regulator_parse_ofw_stdparam(device_t d, phandle_t node, struct regnode_init_def *def)
{
	(void)d; (void)node;
	def->name = test_strdup("sd-io", M_OFWPROP);
	def->parent_name = test_strdup("upstream", M_OFWPROP);
	return (parse_error);
}
static ssize_t
OF_getencprop_alloc_multi(phandle_t n, const char *name, int size, void **p)
{
	(void)n; assert(!strcmp(name, "states") && size == 4);
	uint32_t data[] = { 1800000, 3, 3300000, 0 };
	*p = test_malloc(sizeof(data), M_OFWPROP, 0);
	memcpy(*p, data, sizeof(data));
	return (4);
}
static int OF_getproplen(phandle_t n, const char *p) { (void)n; (void)p; return (-1); }
static int OF_getencprop(phandle_t n, const char *p, void *v, int len)
{ (void)n; (void)p; (void)v; (void)len; return (-1); }
static int
mock_pin_get(int index, struct gpiobus_pin **pin)
{
	if (index == pin_get_failure)
		return (EBUSY);
	*pin = test_malloc(sizeof(**pin), M_GPIO, 0);
	(*pin)->index = index;
	return (0);
}
static int gpio_pin_get_by_ofw_property(device_t d, phandle_t n,
    const char *name, struct gpiobus_pin **pin)
{ (void)d; (void)n; assert(!strcmp(name, "enable-gpio")); return (mock_pin_get(2, pin)); }
static int gpio_pin_get_by_ofw_idx(device_t d, phandle_t n,
    unsigned index, struct gpiobus_pin **pin)
{ (void)d; (void)n; return (mock_pin_get(index, pin)); }
static int gpio_pin_setflags(struct gpiobus_pin *pin, unsigned flags)
{ assert(flags == GPIO_PIN_OUTPUT); pin_configs++; return (pin->index == pin_init_failure ? EIO : 0); }
static void gpio_pin_release(struct gpiobus_pin *pin)
{ pin_releases++; test_free(pin, M_GPIO); }

/* PRODUCTION_FUNCTIONS */

static void
reset(void)
{
	assert(live_allocations == 0 && sysctl_live == 0 && sx_live == 0);
	assert(class_refs == 0 && topo == 0 && TAILQ_EMPTY(&regnode_list));
	init_error = sysctl_error = parse_error = published = registered_xref = init_ok = 0;
	pin_configs = pin_releases = 0;
	pin_get_failure = pin_init_failure = -1;
	last_node = NULL;
}
static void
node_controls(void)
{
	struct device dev = { 1, NULL };
	struct regnode_init_def def = { .name = "node", .parent_name = "parent" };
	struct regnode *node;
	struct kobj_class cls = { sizeof(struct gpioregulator_reg_sc), 0 };
	reset();
	node = regnode_create(&dev, &cls, &def);
	assert(sysctl_live > 0); /* Legacy create still publishes immediately. */
	int before = live_allocations;
	sysctl_error = EBUSY;
	assert(regnode_destroy_unpublished(node) == EBUSY);
	assert(live_allocations == before && sx_live == 1 && class_refs == 1);
	assert(node->name[0] == 'n' && !TAILQ_EMPTY(&node->sysctl_ctx));
	sysctl_error = 0;
	assert(regnode_destroy_unpublished(node) == 0);
	reset();
	def.flags = REGULATOR_FLAGS_STATIC | REGULATOR_FLAGS_DEFER_SYSCTL;
	cls.size = 0;
	node = regnode_create(&dev, &cls, &def);
	assert(sysctl_live == 0 && node->softc == NULL);
	node->ref_cnt = 1;
	assert(regnode_destroy_unpublished(node) == EBUSY);
	node->ref_cnt = 0; node->enable_cnt = 1;
	assert(regnode_destroy_unpublished(node) == EBUSY);
	node->enable_cnt = 0;
	struct regulator consumer;
	TAILQ_INSERT_TAIL(&node->consumers_list, &consumer, link);
	assert(regnode_destroy_unpublished(node) == EBUSY);
	TAILQ_REMOVE(&node->consumers_list, &consumer, link);
	assert(regnode_destroy_unpublished(node) == 0);
	reset();
}
int
main(void)
{
	node_controls();
	struct gpioregulator_softc sc = { 0 };
	struct device dev = { 1, &sc };
	parse_error = EINVAL;
	assert(gpioregulator_attach(&dev) == EINVAL);
	assert(published == 0 && pin_configs == 0);
	reset(); memset(&sc, 0, sizeof(sc));
	pin_get_failure = 1; /* Enable and first state pin were already acquired. */
	assert(gpioregulator_attach(&dev) == EBUSY);
	assert(pin_releases == 2 && published == 0);
	reset(); memset(&sc, 0, sizeof(sc));
	pin_init_failure = 1; /* Actual INIT fails after earlier GPIO directions. */
	assert(gpioregulator_attach(&dev) == ENXIO);
	assert(pin_configs == 3 && pin_releases == 3 && sc.reg_sc == NULL);
	assert(published == 0 && sysctl_live == 0);
	gpioregulator_release_def(&sc.init_def); /* Idempotent empty unwind. */
	reset(); memset(&sc, 0, sizeof(sc));
	dev.node = 0; /* FDT registration can fail before invoking INIT. */
	assert(gpioregulator_attach(&dev) == ENXIO);
	assert(pin_configs == 0 && pin_releases == 3 && published == 0);
	reset(); memset(&sc, 0, sizeof(sc));
	dev.node = 1;
	assert(gpioregulator_attach(&dev) == 0);
	assert(published == 1 && sysctl_live > 0 && init_ok && pin_configs == 3);
	struct regnode *node = sc.reg_sc->regnode;
	int before = live_allocations;
	assert(regnode_destroy_unpublished(node) == EBUSY && live_allocations == before);
	/* Private fixture teardown, not a production unregister claim. */
	TAILQ_REMOVE(&regnode_list, node, reglist_link);
	registered_xref = published = 0;
	assert(regnode_destroy_unpublished(node) == 0);
	gpioregulator_release_def(&sc.init_def);
	reset();
	puts("PASS: actual regnode create/register/destroy and GPIO producer ownership; deferred diagnostics, init/parse/pin faults, busy SYSCTL retry, published/active guards, static/copied allocation owners");
	return (0);
}
