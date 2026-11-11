/* SPDX-License-Identifier: BSD-2-Clause */
static unsigned checks;
static void check(bool value, const char *name) {
    if (!value) { fprintf(stderr, "FAIL: %s\n", name); abort(); }
    checks++;
}
static void fault_exit(int signo) { (void)signo; _exit(77); }
static void walk_faults_before_publication(void) {
    pid_t child = fork();
    int status;
    assert(child >= 0);
    if (child == 0) {
        struct rlimit limit = {0, 0};
        setrlimit(RLIMIT_CORE, &limit);
        signal(SIGSEGV, fault_exit);
        signal(SIGBUS, fault_exit);
        bs_state.dmap_valid = false;
        unreadable = true;
        translation_result = 1;
#if HAVE_BASELINE
        bs_state.dmap_valid = true;
        (void)baseline_pmap_klookup(DMAP_MIN_ADDRESS + UINT64_C(0x1044ac110), NULL);
#else
        (void)pmap_klookup(DMAP_MIN_ADDRESS + UINT64_C(0x1044ac110), NULL);
#endif
        _exit(78);
    }
    assert(waitpid(child, &status, 0) == child);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 77,
        "unpublished/original page-table path faults on protected PTE memory");
}
int main(void) {
    vm_paddr_t pa;
    unsigned i;
    fault_page = mmap(NULL, PAGE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(fault_page != MAP_FAILED);
    memcpy(supplied, fixture_ranges, sizeof(fixture_ranges));
    supplied_count = sizeof(fixture_ranges) / sizeof(fixture_ranges[0]);
    pmap_bootstrap_dmap(PAGE_SIZE);
    check(tlbi_calls == 1 && !tlbi_ready && exclude_ready,
        "actual bootstrap publishes after TLBI and before allocation exclusions");
    check(dmap_mapped_idx == supplied_count &&
        memcmp(dmap_mapped, fixture_ranges, sizeof(fixture_ranges)) == 0,
        "actual bootstrap captures complete exact mapped intervals");
    unreadable = true;
    for (i = 0; i < sizeof(fixture_cases) / sizeof(fixture_cases[0]); i++) {
        unsigned before = walk_calls;
        bool fast = VIRT_IN_DMAP(fixture_cases[i].va);
        unreadable = fast; l1pte = 0;
        pa = UINT64_C(0xfeedface);
        bool valid = pmap_klookup(fixture_cases[i].va, &pa);
        check(valid == fixture_cases[i].valid, "presealed byte membership");
        check(pa == (valid ? fixture_cases[i].pa : UINT64_C(0xfeedface)),
            "byte offset or unchanged failure output");
        check(walk_calls == before + (fast ? 0 : 1) && intr_depth == 0,
            "published DMAP lookup does not access unreadable PTE memory");
        check(pmap_klookup(fixture_cases[i].va, NULL) == valid,
            "NULL physical-output lookup");
    }
    unreadable = true;
    /* Rebuild allocation availability, as EFI/kernel/sanitizer exclusions do. */
    supplied_count = 0;
    (void)physmem_avail(physmap, sizeof(physmap) / sizeof(physmap[0]));
    pa = 0;
    check(pmap_klookup(DMAP_MIN_ADDRESS + UINT64_C(0x1044ac110), &pa) &&
        pa == UINT64_C(0x1044ac110), "later NOALLOC mutation cannot erase mapped PT page");
    walk_faults_before_publication();
    unreadable = false;
    l1pte = UINT64_C(0x80000001);
    bs_state.dmap_valid = false;
    unsigned before = walk_calls;
    check(pmap_klookup(DMAP_MIN_ADDRESS + 7, &pa) && walk_calls == before + 1,
        "unpublished DMAP preserves original page-table fallback");
    bs_state.dmap_valid = true;
    before = walk_calls;
    check(pmap_klookup(KERNBASE + 7, &pa) && pa == UINT64_C(0x80000007) &&
        walk_calls == before + 1, "non-DMAP L1 block fallback preserved");
    l1pte = 3; l2pte = UINT64_C(0x80200001);
    check(pmap_klookup(KERNBASE + 17, &pa) && pa == UINT64_C(0x80200011),
        "non-DMAP L2 block fallback preserved");
    l2pte = 3; l3pte = UINT64_C(0x80300003);
    check(pmap_klookup(KERNBASE + 29, &pa) && pa == UINT64_C(0x8030001d),
        "non-DMAP L3 page fallback preserved");
    l1pte = 0; pa = UINT64_C(0xfeedface);
    check(!pmap_klookup(DMAP_MIN_ADDRESS + UINT64_C(0x200000000), &pa) &&
        pa == UINT64_C(0xfeedface), "beyond actual DMAP delegates old false fallback");
    check(!pmap_klookup(DMAP_MAX_ADDRESS, NULL), "static DMAP end delegates fallback");
    translation_result = UINT64_C(0x12345000); unreadable = true;
    before = walk_calls;
    check(pmap_klookup(DMAP_MIN_ADDRESS + 9, &pa) && pa == UINT64_C(0x12345009) &&
        walk_calls == before, "successful AT retains precedence including holes");
    translation_result = 1;
    /* Nonzero 1GiB-aligned base and a gap before the first mapped page. */
    dmap_phys_base = UINT64_C(0x40000000);
    dmap_phys_max = UINT64_C(0x40400000);
    dmap_max_addr = DMAP_MIN_ADDRESS + UINT64_C(0x400000);
    dmap_mapped_idx = 2;
    dmap_mapped[0] = UINT64_C(0x40100000);
    dmap_mapped[1] = UINT64_C(0x40400000);
    pa = UINT64_C(0xfeedface);
    check(!pmap_klookup(DMAP_MIN_ADDRESS, &pa) && pa == UINT64_C(0xfeedface),
        "nonzero physical base leading hole");
    check(pmap_klookup(DMAP_MIN_ADDRESS + UINT64_C(0x10000b), &pa) &&
        pa == UINT64_C(0x4010000b), "nonzero physical base exact byte inverse");
    /* An artificial unaligned physmem input checks actual bootstrap rounding. */
    bs_state.dmap_valid = false; tlbi_calls = 0;
    supplied[0] = UINT64_C(0x40100003); supplied[1] = UINT64_C(0x40104000);
    supplied_count = 2;
    pmap_bootstrap_dmap(PAGE_SIZE);
    check(dmap_mapped[0] == UINT64_C(0x40100000) &&
        dmap_mapped[1] == UINT64_C(0x40104000),
        "snapshot records actual bootstrap rounded start and exclusive end");
    munmap(fault_page, PAGE_SIZE);
    printf("PASS: %u actual pmap lookup/bootstrap host controls; no native MMU qualification\n", checks);
    return (0);
}
