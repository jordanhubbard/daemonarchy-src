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
static int mux_error, mux_error_at, drop_offset;
static int delete_error, release_error, reserve_error_at, reservations, handles, published_at;
static bool quarantined, drop_write;
static struct { pcell_t cells[8]; int len, mode; bool enabled, nested; int badprop; int boollen; const char *line_name; } hogs[4];
static unsigned nhogs;
static bool mux_checks_preload;
static void (*attach_hook)(void);

void *device_get_softc(device_t d) { return (d->softc); }
const char *device_get_nameunit(device_t d) { (void)d; return ("bcmstb-test"); }
void device_set_desc(device_t d, const char *s) { (void)d; (void)s; }
void device_printf(device_t d, const char *s, ...) { (void)d; (void)s; }
bool device_is_attached(device_t d) { return (d->attached); }
int device_delete_child(device_t d, device_t c) {
 assert(d == &gpio && c == &bus && !registered);
 if (delete_error) return delete_error;
 assert(handles == 0); bus.attached=false; return 0;
}
void bus_generic_new_pass(device_t d) { assert(d==&gpio && softc.ready); }
void bus_generic_driver_added(device_t d, void *p) { (void)p;assert(d==&gpio && softc.ready); }
int bus_generic_suspend(device_t d) { assert(d==&gpio && softc.ready);return 0; }
int bus_generic_resume(device_t d) { assert(d==&gpio && softc.ready);return 0; }
int bus_generic_shutdown(device_t d) { assert(d==&gpio && softc.ready);return 0; }
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
	if (release_error) return release_error;
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
	if (!drop_write && drop_offset!=(int)offset) r->words[offset / 4] = value;
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
bool ofw_bus_node_status_okay(phandle_t p) { return p==20 || (p>=40 && p<40+nhogs && hogs[p-40].enabled); }
bool ofw_bus_node_is_compatible(phandle_t p, const char *s)
{ return (p == 20 && !strcmp(s, "brcm,bcm2712d0-aon-pinctrl")); }
phandle_t OF_parent(phandle_t p) { assert(p == 10); return (1); }
phandle_t OF_child(phandle_t p) { if(p==1)return 10; if(p==10)return nhogs?40:0; assert(p>=40 && p<40+nhogs);return hogs[p-40].nested?100:0; }
phandle_t OF_peer(phandle_t p) { if(p>=40 && p<40+nhogs)return p+1<40+nhogs?p+1:0;return p==10?20:0; }
phandle_t OF_xref_from_node(phandle_t p) { return (p + 100); }
device_t OF_device_from_xref(phandle_t p)
{ assert(p == 120); return (provider_missing ? NULL : &pinctrl); }
int OF_getproplen(phandle_t p, const char *name)
{
	if(p>=40 && p<40+nhogs) {
	 if(!strcmp(name,"gpios"))return hogs[p-40].len;
	 if(!strcmp(name,"line-name"))return hogs[p-40].line_name?(int)strlen(hogs[p-40].line_name)+1:-1;
	 return OF_hasprop(p,name)?hogs[p-40].boollen:-1;
	}
	assert(p == 10);
	if (!strcmp(name, "reg")) return (sizeof(bus_reg));
	assert(!strcmp(name, "brcm,gpio-bank-widths")); return (widthslen);
}
bool OF_hasprop(phandle_t p, const char *name)
{ if(p>=40 && p<40+nhogs) {
 if(!strcmp(name,"gpio-hog"))return hogs[p-40].mode!=0;
 return (!strcmp(name,"input") && (hogs[p-40].mode&1)) ||
 (!strcmp(name,"output-high") && (hogs[p-40].mode&2)) ||
 (!strcmp(name,"output-low") && (hogs[p-40].mode&4));
 } assert(p == 10 && !strcmp(name, "gpio-controller")); return (true); }
int OF_nextprop(phandle_t p,const char *prev,char *out,size_t len) {
 assert(p>=40 && p<40+nhogs && len>=64);
 if(prev==NULL){strcpy(out,"gpio-hog");return 1;}
 if(!strcmp(prev,"gpio-hog")){strcpy(out,"gpios");return 1;}
 if(!strcmp(prev,"gpios") && hogs[p-40].badprop){
 if(hogs[p-40].badprop==2){memset(out,'x',len);return 1;}
 if(hogs[p-40].badprop==3)return -1;
 strcpy(out,"drive-strength");return 1;}
 return 0;
}
int OF_getprop(phandle_t p,const char *n,void *v,int l) { assert(p>=40 && p<40+nhogs && !strcmp(n,"line-name") && hogs[p-40].line_name);size_t len=strlen(hogs[p-40].line_name)+1;assert(l>=(int)len);memcpy(v,hogs[p-40].line_name,len);return len; }

int
OF_getencprop(phandle_t p, const char *name, void *out, int len)
{
	if(p>=40 && p<40+nhogs) {
	 assert(!strcmp(name,"gpios") && len>=hogs[p-40].len);
	 memcpy(out,hogs[p-40].cells,hogs[p-40].len);return hogs[p-40].len;
	}
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
gpiobus_add_bus_deferred(device_t d)
{
	assert(d == &gpio && softc.lock_initialized && softc.pinctrl == &pinctrl);
	if (add_bus_fail)
		return (NULL);
	assert(!registered);
	return (&bus);
}

void
bus_attach_children(device_t d)
{
	assert(d == &gpio && !registered);
	if (attach_hook != NULL)
		attach_hook();
	bus.attached = !child_attach_fail;
}

bool gpiobus_ready(device_t d) { assert(d==&bus);return !child_attach_fail && !quarantined; }
void gpiobus_quarantine(device_t d,int error) { assert(d==&bus && error);quarantined=true; }
void ofw_gpiobus_register_provider(device_t d) {
 assert(d==&gpio && softc.ready && !registered);registered=1;published_at=nevents;
}
int gpio_pin_get_by_bus_pinnum(device_t d,uint32_t pin,gpio_pin_t *out) {
 assert(d==&bus && !softc.ready && !registered && mux_calls==0);
 if(++reservations==reserve_error_at)return EBUSY;
 *out=calloc(1,sizeof(**out));assert(*out);(*out)->dev=&gpio;(*out)->pin=pin;handles++;return 0;
}
void gpio_pin_release(gpio_pin_t p) { assert(p && handles>0);handles--;free(p); }

int
bcmstb_test_mux(device_t p, device_t g, uint32_t pin, uint32_t flags)
{
	assert(p == &pinctrl && g == &gpio && pin >= 2 && pin <= 5);
	assert(softc.lock.locked);
	if (mux_checks_preload)
		assert(!!(registers.words[GIO_DATA(0) / 4] & (1U << pin)) ==
		    !!(flags & GPIO_PIN_PRESET_HIGH));
	if (softc.nhogs) assert(reservations==(int)softc.nhogs && handles==(int)softc.nhogs);
	mux_calls++;
	events[nevents++] = 0x100;
	return mux_error_at ? (mux_calls==(unsigned)mux_error_at ? EIO : 0) : mux_error;
}

static void
reset(void)
{
	assert(allocations == 0 && registered == 0 && handles==0);
	memset(&softc, 0, sizeof(softc));
	registers.start = BCM2712_AON_GPIO_BASE;
	registers.size = 64;
	for (unsigned i = 0; i < 16; i++)
		registers.words[i] = 0xa5a50000U | (i * 17);
	widths[0] = 15; widths[1] = 6; widthslen = 8; gpio_cells = 2;
	bus_reg[0] = 0x7d517c00; bus_reg[1] = 64;
	allocation_fail = provider_missing = add_bus_fail = child_attach_fail = false;
	mux_error = mux_error_at = 0;drop_offset=-1; mux_checks_preload = false; attach_hook = NULL;
	writes = mux_calls = nevents = 0;
	delete_error=release_error=reserve_error_at=reservations=published_at=0;
	quarantined=drop_write=false;nhogs=0;memset(hogs,0,sizeof(hogs));
}

static void
assert_release(void)
{
	/* Test-only teardown of a previously published provider with no consumers. */
	registered=0;softc.published=false;softc.ready=false;
	assert(bcmstb_gpio_release(&gpio)==0);
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
	assert(bcmstb_gpio_get_node(&gpio, &bus) == gpio.node);
	assert(bcmstb_gpio_get_node(&gpio, &pinctrl) == (phandle_t)-1);
	assert(bcmstb_gpio_get_node(&gpio, NULL) == (phandle_t)-1);
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

/* Exact selected Pi500 child: gpio-hog; gpios=<2 0>; output-high;
 * line-name="RP1 RUN pin". Additional vectors deliberately exercise the same
 * binding parser with multiple reservations, polarity and failure injection.
 */
static void
hog(unsigned index, uint32_t pin, uint32_t active_low, unsigned mode)
{
	if (nhogs <= index) nhogs = index + 1;
	hogs[index].enabled = true;
	hogs[index].cells[0] = pin; hogs[index].cells[1] = active_low;
	hogs[index].len = 8; hogs[index].mode = mode;
}

static void
retained_test_cleanup(void)
{
	/* Only the harness may dispose the modeled device after assertions. */
	softc.hog_write = false;
	assert_release();
}

static void
assert_unavailable(void)
{
	uint32_t pin, flags;
	pcell_t cells[2] = {3,0};
	unsigned before = writes;
	assert(!softc.ready && !registered);
	assert(bcmstb_gpio_pin_setflags(&gpio,3,GPIO_PIN_OUTPUT)==ENXIO);
	assert(bcmstb_gpio_pin_set(&gpio,3,1)==ENXIO);
	assert(bcmstb_gpio_pin_toggle(&gpio,3)==ENXIO);
	assert(bcmstb_gpio_map_gpios(&gpio,50,10,2,cells,&pin,&flags)==ENXIO);
	bcmstb_gpio_new_pass(&gpio);
	bcmstb_gpio_driver_added(&gpio,NULL);
	assert(bcmstb_gpio_rescan(&gpio)==ENXIO);
	assert(bcmstb_gpio_suspend(&gpio)==ENXIO);
	assert(bcmstb_gpio_resume(&gpio)==ENXIO);
	assert(bcmstb_gpio_shutdown(&gpio)==ENXIO);
	assert(writes==before);
}

static void
test_hog_preflight(void)
{
	for (int failure=0; failure<15; failure++) {
		reset();hog(0,2,0,2);
		switch(failure) {
		case 0: hog(1,2,0,4);break; /* Duplicate across children. */
		case 1: hogs[0].mode=0;break; /* Enabled non-hog child. */
		case 2: hogs[0].len=7;break;
		case 3: hogs[0].mode=6;break;
		case 4: hogs[0].boollen=4;break;
		case 5: hogs[0].cells[1]=2;break;
		case 6: hogs[0].cells[0]=5;break; /* Output not supported. */
		case 7: hogs[0].cells[0]=15;break; /* Bank hole. */
		case 8: hogs[0].badprop=true;break;
		case 9: hogs[0].nested=true;break;
		case 10: hogs[0].len=0;break;
		case 11: hogs[0].cells[0]=UINT32_MAX;break;
		case 12: hogs[0].badprop=2;break;
		case 13: hogs[0].badprop=3;break;
		case 14: hogs[0].cells[2]=2;hogs[0].cells[3]=0;hogs[0].len=16;break;
		}
		assert(bcmstb_gpio_attach(&gpio)!=0);
		assert(writes==0 && mux_calls==0 && allocations==0 && handles==0 && !registered);
		assert_unavailable();
	}
	reset();hog(0,2,0,0);hogs[0].enabled=false;
	assert(bcmstb_gpio_attach(&gpio)==0 && registered && !softc.nhogs);
	assert_release();
}

static void
test_hog_success(void)
{
	uint32_t pin,flags,words[16];pcell_t cells[2]={2,0};
	for (int mode=1; mode<=4; mode*=2) {
	 for(unsigned polarity=0;polarity<2;polarity++) {
		reset();hog(0,2,polarity,mode);hog(1,3,0,4);
		mux_checks_preload=mode!=1;
		assert(bcmstb_gpio_attach(&gpio)==0);
		assert(softc.ready && softc.published && registered && handles==2 &&
		    reservations==2 && mux_calls==2 && published_at==(int)nevents);
		assert(!!(registers.words[GIO_IODIR(0)/4] & 4)==(mode==1));
		if(mode!=1)assert(!!(registers.words[GIO_DATA(0)/4]&4)==((mode==2)!=!!polarity));
		memcpy(words,registers.words,sizeof(words));
		assert(bcmstb_gpio_pin_setflags(&gpio,2,GPIO_PIN_INPUT)==EBUSY);
		assert(bcmstb_gpio_pin_set(&gpio,2,0)==EBUSY);
		assert(bcmstb_gpio_pin_toggle(&gpio,2)==EBUSY);
		assert(bcmstb_gpio_map_gpios(&gpio,50,10,2,cells,&pin,&flags)==EBUSY);
		assert(!memcmp(words,registers.words,sizeof(words)));
		assert(bcmstb_gpio_detach(&gpio)==EBUSY && handles==2);
		retained_test_cleanup();
	 }
	}
	reset();hog(0,SELECTED_HOG_PIN,SELECTED_HOG_FLAGS,2);
	hogs[0].line_name=SELECTED_HOG_LINE_NAME;
	assert(bcmstb_gpio_attach(&gpio)==0 && softc.nhogs==1 && registered);
	assert(softc.hogs[0].pin==SELECTED_HOG_PIN && handles==1);
	retained_test_cleanup();
	reset();hog(0,5,0,1);assert(bcmstb_gpio_attach(&gpio)==0);
	assert(registers.words[GIO_IODIR(0)/4]&(1U<<5));retained_test_cleanup();
}

static void
test_hog_failures(void)
{
	for (int failure=0;failure<9;failure++) {
		reset();hog(0,2,0,2);hog(1,3,0,4);
		switch(failure) {
		case 0:reserve_error_at=1;break;
		case 1:reserve_error_at=2;break;
		case 2:mux_error=EIO;break;
		case 3:drop_write=true;break;
		case 4:child_attach_fail=true;delete_error=EBUSY;break;
		case 5:add_bus_fail=true;release_error=EBUSY;break;
		case 6:reserve_error_at=2;delete_error=EBUSY;break;
		case 7:mux_error_at=2;break;
		case 8:drop_offset=GIO_IODIR(0);registers.words[GIO_IODIR(0)/4]|=4;break;
		}
		int error=bcmstb_gpio_attach(&gpio);
		if(failure<2) {
		 assert(error==EBUSY && mux_calls==0 && allocations==0 && handles==0);
		 for(unsigned i=0;i<nevents;i++)assert(events[i]!=GIO_DATA(0) && events[i]!=GIO_IODIR(0));
		} else {
		 assert(error==0 && allocations==1 && softc.lock_initialized);
		 if(failure==2 || failure==3 || failure==7 || failure==8) {
		  assert(softc.hog_write && handles==2 && quarantined);
		  assert(bcmstb_gpio_detach(&gpio)==EBUSY);
		 } else assert(!softc.hog_write && handles==0);
		 assert_unavailable();
		 delete_error=release_error=0;drop_write=false;drop_offset=-1;
		 retained_test_cleanup();
		}
	}
}

int
main(void)
{
	test_failed_attach();
	test_gpio_operations();
	test_hog_preflight();
	test_hog_success();
	test_hog_failures();
	puts("PASS: production AON GPIO/hog complete preflight, all-pin reservation, private ordered apply/readback, polarity, raw-write exclusion and retained-owner failure/retry controls");
	return (0);
}
