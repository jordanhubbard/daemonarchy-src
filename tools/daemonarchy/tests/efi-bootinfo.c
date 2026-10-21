/* SPDX-License-Identifier: BSD-2-Clause */
/* Native failure tests include the actual EFI metadata and caller implementation. */
#include <assert.h>
#include <stdarg.h>
#ifdef NDEBUG
#error "Bootinfo checks require assertions"
#endif
#define main efi_application_main
#define printf fault_printf
#include "stand/efi/loader/bootinfo.c"
#undef main
#undef printf
extern int vprintf(const char *, va_list);

static EFI_BOOT_SERVICES services;
EFI_BOOT_SERVICES *BS = &services;
static EFI_RUNTIME_SERVICES runtime;
EFI_RUNTIME_SERVICES *RS = &runtime;
EFI_SYSTEM_TABLE *ST;
EFI_HANDLE IH;
bool boot_services_active;
EFI_PHYSICAL_ADDRESS staging;
teken_gfx_t gfx_state;
struct arch_switch archsw;
const char md_kerntype[] = "elf kernel";
static struct devdesc root;
static struct preloaded_file kernel;
static struct { struct file_metadata header; uint8_t space[16]; } end_metadata;
static uint8_t map_buffer[EFI_PAGE_SIZE * 4] __aligned(16);
static unsigned map_calls, exit_calls, alloc_calls, free_calls, module_calls;
static unsigned metadata_calls, map_metadata, vmap_calls;
static bool critical_window, allocation_live, want_exit, quiescence_failure;
enum scenario { QUERY_FAIL, ALLOC_FAIL, MAP_FAIL, EXIT_FAIL, RETRY_OK, OK };
static enum scenario scenario;

int
fault_printf(const char *format, ...)
{
	va_list ap;
	int result;
	assert(!critical_window);
	va_start(ap, format); result = vprintf(format, ap); va_end(ap);
	return (result);
}

int
env_setenv(const char *name, int flags, const void *value,
    ev_sethook_t sethook, ev_unsethook_t unsethook)
{
	(void)name; (void)flags; (void)value; (void)sethook; (void)unsethook;
	assert(!"unexpected serial environment path"); return (EINVAL);
}
int boot_parse_cmdline(char *args) { (void)args; return (0); }
int boot_env_to_howto(void) { return (0); }
int efinet_console_handoff_check(void) { return (quiescence_failure ? EIO : 0); }
void netconsole_handoff(void) { assert(!critical_window); }
static int
getdev(void **out, const char *name, const char **path)
{
	(void)name; (void)path; *out = &root; return (0);
}
char *devformat(struct devdesc *dev) { assert(dev == &root); return ("disk0:"); }
int getrootmount(char *name) { assert(strcmp(name, "disk0:") == 0); return (0); }
vm_offset_t build_font_module(vm_offset_t addr) { return (addr); }
vm_offset_t build_splash_module(vm_offset_t addr, int which)
{ (void)which; return (addr); }
vm_offset_t md_align(vm_offset_t addr) { return (roundup2(addr, EFI_PAGE_SIZE)); }
vm_offset_t md_copyenv(vm_offset_t addr) { return (addr); }
struct preloaded_file *
file_findfile(const char *name, const char *type)
{ (void)name; (void)type; return (&kernel); }
void
file_addmetadata(struct preloaded_file *file, int type, size_t size, void *data)
{
	assert(file == &kernel && !critical_window);
	metadata_calls++;
	if (type == MODINFOMD_KERNEND) {
		assert(size == sizeof(uint64_t));
		memcpy(end_metadata.header.md_data, data, size);
	}
	if (type == MODINFOMD_EFI_MAP) map_metadata++;
}
struct file_metadata *
file_findmetadata(struct preloaded_file *file, int type)
{
	assert(file == &kernel && type == MODINFOMD_KERNEND);
	return (&end_metadata.header);
}
vm_offset_t
md_copymodules(vm_offset_t addr, bool is64)
{
	(void)addr; assert(is64 && !critical_window);
	/* Failed EFI metadata must never reach the final module-copy path. */
	assert(scenario == OK || scenario == RETRY_OK);
	module_calls++;
	return (128);
}
void
panic(const char *format, ...)
{
	(void)format; assert(!"unexpected panic"); __builtin_trap();
}

static EFI_STATUS EFIAPI
get_map(UINTN *size, EFI_MEMORY_DESCRIPTOR *map, UINTN *key,
    UINTN *stride, UINT32 *version)
{
	assert(!critical_window);
	map_calls++; *stride = sizeof(*map); *version = 1;
	if (map == NULL) {
		*size = sizeof(*map);
		return (scenario == QUERY_FAIL ? EFI_DEVICE_ERROR : EFI_BUFFER_TOO_SMALL);
	}
	assert(allocation_live && *size >= sizeof(*map));
	if (scenario == MAP_FAIL) return (EFI_DEVICE_ERROR);
	memset(map, 0, sizeof(*map));
	map->Type = EfiRuntimeServicesData; map->PhysicalStart = 0x100000;
	map->NumberOfPages = 1; map->Attribute = EFI_MEMORY_RUNTIME;
	*size = sizeof(*map); *key = 100 + map_calls;
	critical_window = want_exit;
	return (EFI_SUCCESS);
}
static EFI_STATUS EFIAPI
allocate(EFI_ALLOCATE_TYPE type, EFI_MEMORY_TYPE memory, UINTN pages,
    EFI_PHYSICAL_ADDRESS *address)
{
	(void)type; (void)memory;
	assert(!critical_window && !allocation_live && pages * EFI_PAGE_SIZE <= sizeof(map_buffer));
	alloc_calls++;
	if (scenario == ALLOC_FAIL) return (EFI_OUT_OF_RESOURCES);
	*address = (uintptr_t)map_buffer; allocation_live = true;
	return (EFI_SUCCESS);
}
static EFI_STATUS EFIAPI
release(EFI_PHYSICAL_ADDRESS address, UINTN pages)
{
	(void)pages;
	assert(!critical_window && allocation_live && address == (uintptr_t)map_buffer);
	free_calls++; allocation_live = false; return (EFI_SUCCESS);
}
static EFI_STATUS EFIAPI
exit_services(EFI_HANDLE handle, UINTN key)
{
	(void)handle;
	assert(critical_window && key == 100 + map_calls);
	critical_window = false; exit_calls++;
	if (scenario == EXIT_FAIL || (scenario == RETRY_OK && exit_calls == 1))
		return (EFI_INVALID_PARAMETER);
	return (EFI_SUCCESS);
}

static EFI_STATUS EFIAPI
set_virtual_map(UINTN size, UINTN stride, UINT32 version, EFI_MEMORY_DESCRIPTOR *map)
{
	assert(!boot_services_active && !critical_window);
	assert(size == sizeof(*map) && stride == sizeof(*map) && version == 1);
	assert(map->VirtualStart == map->PhysicalStart);
	vmap_calls++;
	return (EFI_SUCCESS);
}

static void
reset(enum scenario selected, bool exit_bs)
{
	memset(&services, 0, sizeof(services)); memset(&kernel, 0, sizeof(kernel));
	memset(&archsw, 0, sizeof(archsw)); memset(&gfx_state, 0, sizeof(gfx_state));
	services.GetMemoryMap = get_map; services.AllocatePages = allocate;
	services.FreePages = release; services.ExitBootServices = exit_services;
	memset(&runtime, 0, sizeof(runtime)); runtime.SetVirtualAddressMap = set_virtual_map;
	archsw.arch_getdev = getdev; kernel.f_addr = 4096; kernel.f_size = 4096;
	map_calls = exit_calls = alloc_calls = free_calls = module_calls = 0;
	metadata_calls = map_metadata = vmap_calls = 0; critical_window = allocation_live = false;
	quiescence_failure = false; scenario = selected; want_exit = exit_bs;
	boot_services_active = true;
	unsetenv("console"); setenv("efi_disable_vmap", "YES", 1);
}

static void
test_failure(enum scenario selected, int expected, unsigned maps,
    unsigned allocations, unsigned frees, unsigned exits)
{
	vm_offset_t module = 0, end = 0xdeadbeef;
	reset(selected, true);
	assert(bi_load(NULL, &module, &end, true) == expected);
	assert(end == 0xdeadbeef && module_calls == 0 && map_metadata == 0);
	assert(map_calls == maps && alloc_calls == allocations);
	assert(free_calls == frees && exit_calls == exits);
	assert(boot_services_active && !allocation_live && !critical_window);
}

static void
test_success(enum scenario selected, bool exit_bs, bool vmap)
{
	vm_offset_t module = 0, end = 0xdeadbeef;
	reset(selected, exit_bs);
	if (vmap) unsetenv("efi_disable_vmap");
	assert(bi_load(NULL, &module, &end, exit_bs) == 0);
	assert(module == 8192 && end == 12288 && module_calls == 2);
	assert(map_metadata == 1 && alloc_calls == 1 && free_calls == 0);
	assert(allocation_live && !critical_window);
	assert(boot_services_active == !exit_bs);
	assert(exit_calls == (exit_bs ? (selected == RETRY_OK ? 2 : 1) : 0));
	assert(map_calls == (selected == RETRY_OK ? 3 : 2));
	assert(vmap_calls == (vmap ? 1 : 0));
}

int
main(void)
{
	test_failure(QUERY_FAIL, EINVAL, 1, 0, 0, 0);
	test_failure(ALLOC_FAIL, ENOMEM, 1, 1, 0, 0);
	test_failure(MAP_FAIL, EINVAL, 2, 1, 1, 0);
	test_failure(EXIT_FAIL, EINVAL, 3, 1, 1, 2);
	fault_printf("PASS EFI query/allocation/final-map/exit errors propagate without final module copying\n");
	test_success(OK, true, false);
	test_success(RETRY_OK, true, false);
	test_success(OK, true, true);
	test_success(OK, false, false);
	fault_printf("PASS normal/retried EFI exit, virtual map and retained-services metadata\n");
	reset(OK, true);
	quiescence_failure = true;
	vm_offset_t module = 0, end = 0xdeadbeef;
	assert(bi_load(NULL, &module, &end, true) == EIO);
	assert(module == 0 && end == 0xdeadbeef && metadata_calls == 0 && map_calls == 0);
	fault_printf("PASS unquiesced NIC refuses before metadata and memory-map calls\n");
	return (0);
}
