/* SPDX-License-Identifier: BSD-2-Clause */
/* Independently walk the verbatim BIOS handoff tables at physical boundaries. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "stand/i386/libi386/amd64_bootmap.h"
#define PAGE_SIZE 4096
#define PG_V 0x001
#define PG_RW 0x002
#define PG_PS 0x080
typedef uint32_t vm_offset_t;
typedef uint64_t pml4_entry_t;
typedef uint64_t pdpt_entry_t;
typedef uint64_t pd_entry_t;
static uint64_t PT4[512], PT3[512], PT2[AMD64_BOOTMAP_PT2_ENTRIES];
static uintptr_t
vtop(uintptr_t address)
{
    if (address == (uintptr_t)PT3)
        return (0x10000);
    assert(address >= (uintptr_t)PT2 && address < (uintptr_t)(PT2 + AMD64_BOOTMAP_PT2_ENTRIES));
    return (0x20000 + address - (uintptr_t)PT2);
}
#define VTOP(address) vtop(address)
/* PRODUCTION_FUNCTIONS */
static uint64_t
translate(uint64_t address)
{
    uint64_t l4 = PT4[(address >> 39) & 511];
    uint64_t l3 = PT3[(address >> 30) & 511];
    assert((l4 & 0xfff) == (PG_V | PG_RW));
    assert((l4 & ~UINT64_C(0xfff)) == 0x10000);
    assert((l3 & 0xfff) == (PG_V | PG_RW));
    uint64_t table = l3 & ~UINT64_C(0xfff);
    assert(table >= 0x20000 && table < 0x22000);
    unsigned index = (unsigned)((table - 0x20000) / 8 + ((address >> 21) & 511));
    uint64_t entry = PT2[index];
    assert((entry & 0x1fffff) == (PG_V | PG_RW | PG_PS));
    return ((entry & ~UINT64_C(0x1fffff)) | (address & 0x1fffff));
}
int
main(int argc, char **argv)
{
    assert(argc == 2);
    const uint64_t base = UINT64_C(0xffffffff80000000);
    if (strcmp(argv[1], "boundary") == 0) {
        memset(PT4, 0xff, sizeof PT4); memset(PT3, 0xff, sizeof PT3); memset(PT2, 0xff, sizeof PT2);
        assert(amd64_bootmap_init(0x6b000000, 0x6b002000) == 0);
        const uint32_t physical[] = {0, 0xfff, 0x1000, 0x1fffff, 0x200000,
            0x3fffffff, 0x40000000, 0x40001000, 0x6b000000, 0x7fffffff};
        for (unsigned i = 0; i < sizeof physical / sizeof physical[0]; i++) {
            assert(translate(physical[i]) == physical[i]);
            assert(translate(base + physical[i]) == physical[i]);
        }
        for (uint64_t physical = 0; physical < UINT64_C(0x80000000); physical += 0x200000) {
            assert(translate(base + physical) == physical);
            assert(translate(base + physical + 0x1fffff) == physical + 0x1fffff);
        }
    } else if (strcmp(argv[1], "legacy-alias") == 0) {
        assert(amd64_bootmap_init(0x6b000000, 0x6b002000) == 0);
        assert(translate(base + 0x6b000000) == 0x6b000000);
        PT3[511] = PT3[510]; /* The old single-PT2 alias must be observable. */
        assert(translate(base + 0x6b000000) == 0x2b000000);
    } else {
        assert(strcmp(argv[1], "bounds") == 0);
        assert(amd64_bootmap_init(0x100000, 0x200000) == 0);
        assert(amd64_bootmap_init(0x7ffff000, 0x80000000) == 0);
        uint64_t before[512]; memcpy(before, PT4, sizeof before);
        assert(amd64_bootmap_init(0x7ffff000, 0x80001000) == EFBIG);
        assert(amd64_bootmap_init(0x80000000, 0x80001000) == EFBIG);
        assert(amd64_bootmap_init(0x1000, 0x1000) == EFBIG);
        assert(memcmp(before, PT4, sizeof before) == 0);
    }
    printf("PASS: BIOS handoff %s\n", argv[1]);
    return (0);
}
