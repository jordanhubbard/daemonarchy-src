/* SPDX-License-Identifier: BSD-2-Clause */
/* Compile the actual driver, replacing only kernel/OF/MMIO services. */
#include <bcmstb-test-kernel.h>
#include "../../../sys/dev/gpio/bcmstb_gpio.c"

static struct bcmstb_gpio_softc softc;
static struct resource registers;
static struct device gpio = { &softc, 10, false }, pinctrl = { NULL, 20, true };
static struct device bus = { NULL, 30, true };
static uint32_t widths[2];
static uint32_t bus_reg[2];
static int widthslen, gpio_cells, allocations, registered;
static bool allocation_fail, provider_missing, add_bus_fail, child_attach_fail;
static unsigned writes, mux_calls, events[100], nevents;
static int mux_error;
static bool mux_checks_preload;
static void (*attach_hook)(void);

void *device_get_softc(device_t d) { return (d->softc); }
const char *device_get_nameunit(device_t d) { (void)d; return ("bcmstb-test"); }
void device_set_desc(device_t d, const char *s) { (void)d; (void)s; }
void device_printf(device_t d, const char *s, ...) { (void)d; (void)s; }
bool device_is_attached(device_t d) { return (d->attached); }
int device_delete_children(device_t d) { assert(d == &gpio); return (0); }
uint64_t rman_get_start(struct resource *r) { return (r->start); }
uint64_t rman_get_size(struct resource *r) { return (r->size); }

struct resource *
bus_alloc_resource_any(device_t d, int type, int *rid, int flags)
{
	assert(d == &gpio && type == SYS_RES_MEMORY && *rid == 0 && flags == RF_ACTIVE);
	if (allocation_fail)
		return (NULL);
	assert(allocations++ == 0);
	return (&registers);
}

int
bus_release_resource(device_t d, int type, int rid, struct resource *r)
{
	assert(d == &gpio && type == SYS_RES_MEMORY && rid == 0 && r == &registers);
	assert(allocations-- == 1 && !registered);
	return (0);
}

uint32_t
bus_read_4(struct resource *r, unsigned offset)
{
	assert(r == &registers && offset < 64 && offset % 4 == 0);
	return (r->words[offset / 4]);
}

void
bus_write_4(struct resource *r, unsigned offset, uint32_t value)
{
	assert(r == &registers && offset < 64 && offset % 4 == 0);
	assert(nevents < 100);
	events[nevents++] = offset;
	writes++;
	r->words[offset / 4] = value;
}

void
bus_barrier(struct resource *r, unsigned offset, unsigned length, int flags)
{
	assert(r == &registers && offset + length <= 64);
	assert(flags == BUS_SPACE_BARRIER_WRITE && nevents < 100);
	events[nevents++] = 0x200;
}

void mtx_init(struct mtx *m, const char *s, const char *t, int f)
{ (void)s; (void)t; (void)f; assert(!m->initialized); m->initialized = true; }
void mtx_destroy(struct mtx *m)
{ assert(m->initialized && !m->locked); m->initialized = false; }
void mtx_lock(struct mtx *m)
{ assert(m->initialized && !m->locked); m->locked = true; }
void mtx_unlock(struct mtx *m)
{ assert(m->locked); m->locked = false; }
phandle_t ofw_bus_get_node(device_t d) { return (d->node); }
bool ofw_bus_status_okay(device_t d) { return (d == &gpio); }
bool ofw_bus_is_compatible(device_t d, const char *s)
{ return (d == &gpio && !strcmp(s, "brcm,brcmstb-gpio")); }
bool ofw_bus_node_status_okay(phandle_t p) { return (p == 20); }
bool ofw_bus_node_is_compatible(phandle_t p, const char *s)
{ return (p == 20 && !strcmp(s, "brcm,bcm2712d0-aon-pinctrl")); }
phandle_t OF_parent(phandle_t p) { assert(p == 10); return (1); }
phandle_t OF_child(phandle_t p) { assert(p == 1); return (10); }
phandle_t OF_peer(phandle_t p) { return (p == 10 ? 20 : 0); }
phandle_t OF_xref_from_node(phandle_t p) { return (p + 100); }
device_t OF_device_from_xref(phandle_t p)
{ assert(p == 120); return (provider_missing ? NULL : &pinctrl); }
int OF_getproplen(phandle_t p, const char *name)
{
	assert(p == 10);
	if (!strcmp(name, "reg")) return (sizeof(bus_reg));
	assert(!strcmp(name, "brcm,gpio-bank-widths")); return (widthslen);
}
bool OF_hasprop(phandle_t p, const char *name)
{ assert(p == 10 && !strcmp(name, "gpio-controller")); return (true); }

int
OF_getencprop(phandle_t p, const char *name, void *out, int len)
{
	assert(p == 10);
	if (!strcmp(name, "reg")) {
		assert(len == sizeof(bus_reg));
		memcpy(out, bus_reg, sizeof(bus_reg));
		return (sizeof(bus_reg));
	}
	if (!strcmp(name, "brcm,gpio-bank-widths")) {
		assert(len == sizeof(widths));
		memcpy(out, widths, sizeof(widths));
		return (widthslen);
	}
	assert(!strcmp(name, "#gpio-cells") && len == sizeof(uint32_t));
	*(uint32_t *)out = gpio_cells;
	return (sizeof(uint32_t));
}

device_t
gpiobus_add_bus(device_t d)
{
	assert(d == &gpio && softc.lock_initialized && softc.pinctrl == &pinctrl);
	if (add_bus_fail)
		return (NULL);
	registered = 1;
	return (&bus);
}

void
bus_attach_children(device_t d)
{
	assert(d == &gpio && registered);
	if (attach_hook != NULL)
		attach_hook();
	bus.attached = !child_attach_fail;
}

int gpiobus_detach_bus(device_t d)
{ assert(d == &gpio && registered); registered = 0; return (0); }

int
bcmstb_test_mux(device_t p, device_t g, uint32_t pin, uint32_t flags)
{
	assert(p == &pinctrl && g == &gpio && pin >= 2 && pin <= 5);
	assert(softc.lock.locked);
	if (mux_checks_preload)
		assert(!!(registers.words[GIO_DATA(0) / 4] & (1U << pin)) ==
		    !!(flags & GPIO_PIN_PRESET_HIGH));
	mux_calls++;
	events[nevents++] = 0x100;
	return (mux_error);
}

static void
reset(void)
{
	assert(allocations == 0 && registered == 0);
	memset(&softc, 0, sizeof(softc));
	registers.start = BCM2712_AON_GPIO_BASE;
	registers.size = 64;
	for (unsigned i = 0; i < 16; i++)
		registers.words[i] = 0xa5a50000U | (i * 17);
	widths[0] = 15; widths[1] = 6; widthslen = 8; gpio_cells = 2;
	bus_reg[0] = 0x7d517c00; bus_reg[1] = 64;
	allocation_fail = provider_missing = add_bus_fail = child_attach_fail = false;
	mux_error = 0; mux_checks_preload = false; attach_hook = NULL;
	writes = mux_calls = nevents = 0;
}

static void
assert_release(void)
{
	bcmstb_gpio_release(&gpio);
	assert(!allocations && !registered && !softc.lock.initialized && softc.mem == NULL);
}

static void
test_failed_attach(void)
{
	uint32_t original[16];
	for (int failure = 0; failure < 8; failure++) {
		reset();
		memcpy(original, registers.words, sizeof(original));
		switch (failure) {
		case 0: widthslen = 4; break;
		case 1: widths[0] = 32; break;
		case 2: gpio_cells = 3; break;
		case 3: allocation_fail = true; break;
		case 4: registers.start += 4; break;
		case 5: provider_missing = true; softc.pinctrl = &pinctrl; break;
		case 6: add_bus_fail = true; break;
		case 7: child_attach_fail = true; break;
		}
		assert(bcmstb_gpio_attach(&gpio) != 0);
		assert(!allocations && !registered && !softc.lock_initialized);
		assert(!memcmp(original, registers.words, sizeof(original)));
		assert(mux_calls == 0);
		if (failure < 6)
			assert(writes == 0);
	}
}

static void
test_gpio_operations(void)
{
	uint32_t before[16], value, pin, flags;
	pcell_t cells[2] = { 5, 1 };
	int maxpin;
	reset();
	assert(bcmstb_gpio_probe(&gpio) == 0);
	bus_reg[0] = 0x7d508500;
	assert(bcmstb_gpio_probe(&gpio) == ENXIO);
	bus_reg[0] = 0x7d517c00;
	assert(bcmstb_gpio_attach(&gpio) == 0);
	assert(bcmstb_gpio_get_bus(&gpio) == &bus);
	assert(bcmstb_gpio_pin_max(&gpio, &maxpin) == 0 && maxpin == 63);
	assert(registers.words[GIO_MASK(0) / 4] == 0 && registers.words[GIO_MASK(1) / 4] == 0);
	assert(bcmstb_gpio_map_gpios(&gpio, 40, 10, 2, cells, &pin, &flags) == 0);
	assert(pin == 5 && flags == 1); /* Consumer, not controller, inverts polarity. */
	assert(bcmstb_gpio_pin_getcaps(&gpio, 5, &value) == 0 && value == GPIO_PIN_INPUT);
	for (unsigned bad = 0; bad < 64; bad++) {
		if (bad >= 2 && bad <= 5)
			continue;
		assert(bcmstb_gpio_pin_getcaps(&gpio, bad, &value) == 0 && value == 0);
		assert(bcmstb_gpio_pin_set(&gpio, bad, 1) == EOPNOTSUPP);
	}
	memcpy(before, registers.words, sizeof(before));
	assert(bcmstb_gpio_pin_setflags(&gpio, 5, GPIO_PIN_OUTPUT) == EOPNOTSUPP);
	assert(bcmstb_gpio_pin_setflags(&gpio, 3, GPIO_PIN_INPUT | GPIO_PIN_OUTPUT) == EINVAL);
	assert(bcmstb_gpio_pin_setflags(&gpio, 3, GPIO_PIN_OUTPUT | GPIO_PIN_PULLUP) == EINVAL);
	assert(bcmstb_gpio_pin_setflags(&gpio, 3, GPIO_PIN_OUTPUT | GPIO_INTR_EDGE_RISING) == EINVAL);
	assert(bcmstb_gpio_pin_setflags(&gpio, UINT32_MAX, GPIO_PIN_OUTPUT) == EOPNOTSUPP);
	assert(!memcmp(before, registers.words, sizeof(before)));
	nevents = 0; mux_checks_preload = true;
	assert(bcmstb_gpio_pin_setflags(&gpio, 3, GPIO_PIN_OUTPUT | GPIO_PIN_PRESET_HIGH) == 0);
	assert(nevents == 5 && events[0] == GIO_DATA(0) && events[1] == 0x200 &&
	    events[2] == 0x100 && events[3] == GIO_IODIR(0) && events[4] == 0x200);
	assert(registers.words[1] == (before[1] | (1U << 3)));
	assert(registers.words[2] == (before[2] & ~(1U << 3)));
	mux_checks_preload = false;
	assert(bcmstb_gpio_pin_getflags(&gpio, 3, &value) == 0 && value == GPIO_PIN_OUTPUT);
	assert(bcmstb_gpio_pin_set(&gpio, 3, 0) == 0);
	assert(bcmstb_gpio_pin_get(&gpio, 3, &value) == 0 && value == 0);
	assert(bcmstb_gpio_pin_toggle(&gpio, 3) == 0);
	assert(bcmstb_gpio_pin_get(&gpio, 3, &value) == 0 && value == 1);
	memcpy(before, registers.words, sizeof(before));
	mux_error = EIO;
	assert(bcmstb_gpio_pin_setflags(&gpio, 3, GPIO_PIN_OUTPUT | GPIO_PIN_PRESET_LOW) == EIO);
	assert(!memcmp(before, registers.words, sizeof(before)));
	mux_error = 0;
	assert(bcmstb_gpio_pin_setflags(&gpio, 5, GPIO_PIN_INPUT) == 0);
	assert(registers.words[GIO_IODIR(0) / 4] & (1U << 5));
	assert(bcmstb_gpio_detach(&gpio) == EBUSY && registered && allocations == 1);
	assert_release();
}

int
main(void)
{
	test_failed_attach();
	test_gpio_operations();
	puts("PASS: actual BCM2712 AON GPIO driver MMIO/mux ordering, raw polarity, bounds, failures and retained-provider lifetime");
	return (0);
}
