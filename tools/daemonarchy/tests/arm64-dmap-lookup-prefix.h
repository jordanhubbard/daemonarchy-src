/* SPDX-License-Identifier: BSD-2-Clause */
/* Typed host services, not ARM64 MMU or native-kernel emulation. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef uint64_t vm_paddr_t;
typedef uint64_t vm_offset_t;
typedef uint64_t vm_size_t;
typedef uint64_t pt_entry_t;
typedef uint64_t pd_entry_t;
#define NO_PERTHREAD_SSP __attribute__((no_stack_protector))
#define VM_PHYSSEG_MAX 16
#define DMAP_MIN_ADDRESS UINT64_C(0xffffa00000000000)
#define DMAP_MAX_ADDRESS UINT64_C(0xffffff0000000000)
#define VIRT_IN_DMAP(va) ((uint64_t)(va) >= DMAP_MIN_ADDRESS && (uint64_t)(va) < dmap_max_addr)
#define PAGE_SIZE UINT64_C(4096)
#define L3_SIZE PAGE_SIZE
#define L3_OFFSET (L3_SIZE - 1)
#define L2_SIZE UINT64_C(0x200000)
#define L2_OFFSET (L2_SIZE - 1)
#define L1_SIZE UINT64_C(0x40000000)
#define L1_OFFSET (L1_SIZE - 1)
#define L1_BLOCKS_SUPPORTED 1
#define KERNBASE UINT64_C(0xffff000000000000)
#define rounddown2(x,n) ((x) & ~((n)-1))
#define roundup2(x,n) (((x) + (n)-1) & ~((n)-1))
#define READ_SPECIALREG(r) 0
#define TCR_A1 1
#define TCR_DS 2
#define MPASS(x) assert(x)
#define ATTR_AF 0
#define ATTR_S1_XN 0
#define ATTR_KERN_GP 0
#define ATTR_S1_IDX(x) 0
#define VM_MEMATTR_WRITE_BACK 0
#define L1_BLOCK 1
#define PHYS_TO_PTE(pa) (pa)
#define ATTR_DESCR_TYPE_MASK 2
#define ATTR_DESCR_TYPE_BLOCK 0
#define PTE_TO_PHYS(pte) ((pte) & ~UINT64_C(0xfff))
#define PAR_SUCCESS(par) (((par) & 1) == 0)
#define PAR_PA_MASK UINT64_C(0x0000fffffffff000)
#define PAR_LOW_MASK UINT64_C(0xfff)
#define EXFLAG_NOALLOC 1
#define TATTR_PXN_TABLE 0
#define L0_ENTRIES 512
#define Ln_ENTRIES 512
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
#define KASSERT(x,msg) assert(x)

static vm_paddr_t dmap_phys_base, dmap_phys_max;
static vm_offset_t dmap_max_addr;
static bool pmap_lpa_enabled, pmap_l1_supported;
static uint64_t pmap_sh_attr;
static void *kernel_pmap;
static uint64_t translation_result = 1;
static unsigned intr_depth, at_calls, walk_calls, loads, tlbi_calls, excludes;
static bool unreadable, tlbi_ready, exclude_ready;
static pt_entry_t l1pte, l2pte, l3pte, boot_l1[512];
static pt_entry_t *fault_page;
static vm_paddr_t supplied[64];
static unsigned supplied_count;
static register_t intr_disable(void) { intr_depth++; return (0); }
static void intr_restore(register_t saved) { (void)saved; assert(intr_depth == 1); intr_depth--; }
static uint64_t arm64_address_translate_s1e1r(vm_offset_t va) {
    (void)va; assert(intr_depth == 1); at_calls++; return (translation_result);
}
static pt_entry_t pmap_load(pt_entry_t *p) { loads++; return (*p); }
static pt_entry_t *pmap_l1(void *pmap, vm_offset_t va) {
    (void)pmap; (void)va; walk_calls++;
    return (unreadable ? fault_page : &l1pte);
}
static pt_entry_t *pmap_l1_to_l2(pt_entry_t *p, vm_offset_t va) {
    (void)p; (void)va; return (&l2pte);
}
static pt_entry_t *pmap_l2_to_l3(pt_entry_t *p, vm_offset_t va) {
    (void)p; (void)va; return (&l3pte);
}
static vm_paddr_t pmap_early_vtophys(vm_offset_t va) { (void)va; return (PAGE_SIZE); }
static size_t physmem_avail(vm_paddr_t *out, size_t capacity) {
    assert(supplied_count < capacity && supplied_count % 2 == 0);
    memset(out, 0, capacity * sizeof(*out));
    memcpy(out, supplied, supplied_count * sizeof(*out));
    return (supplied_count);
}
static unsigned pmap_l1_index(vm_offset_t va) { return ((va >> 30) & 511); }
static void pmap_store(pt_entry_t *p, pt_entry_t value) { *p = value; }
/* Definitions follow the actual production bootstrap-state declaration. */
