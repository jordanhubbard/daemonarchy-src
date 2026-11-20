#!/usr/bin/env python3
"""Exercise production RP1 window, DMA and interrupt callbacks with bus mocks.

These controls cannot establish PCIe MSI delivery, DMA or physical I/O.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'sys/arm64/broadcom/rp1_pci.c'


def function(source, name):
    match = re.search(r'(?m)^static (?:int|void)\n' + name + r'\(', source)
    if match is None:
        raise ValueError(name)
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef unsigned u_int;
typedef int device_t;
#define KASSERT(test, msg) assert(test)
#define INTR_MAP_DATA_FDT 1
#define FILTER_HANDLED 1
struct intr_irqsrc { unsigned isrc_handlers; };
struct resource { int unused; };
struct rman { int unused; };
#define SYS_RES_IRQ 1
#define SYS_RES_MEMORY 2
#define PCIR_BAR(n) (0x10 + 4 * (n))
struct intr_map_data { int type; };
struct intr_map_data_fdt { int type; unsigned ncells; uint32_t cells[2]; };
struct rp1_softc;
struct rp1_irq {
 struct intr_irqsrc isrc;
 struct rp1_softc *sc;
 struct resource *parent_irq;
 void *cookie;
 u_int vector, trigger;
 bool registered;
};
struct rp1_softc {
struct resource *bar[2]; struct rp1_irq irq[RP1_IRQS]; struct rman memory;
bool manager, msix, published, busmaster, registers_valid;
};
static struct rp1_softc sc;
static struct resource mem;
static struct { void *td_intr_frame; } thread;
static __typeof__(thread) *curthread = &thread;
static unsigned events[32], nevents;
static int dispatch_result;
static bool cleaning;
static unsigned cleanup_calls, fail_cleanup, cfg_writes;
static void *device_get_softc(device_t dev) { assert(dev == 1); return &sc; }
static void event(unsigned e) { assert(nevents < 32); events[nevents++] = e; }
static void bus_write_4(struct resource *r, unsigned off, uint32_t bits) {
 assert(r == &mem);
 if (cleaning) {
  assert(off >= RP1_CFG(0) + RP1_CLR && off <= RP1_CFG(RP1_IRQS-1) + RP1_CLR);
  assert((off & 3) == 0 && bits == (RP1_ENABLE | RP1_ACK_ENABLE));
  cfg_writes++; return;
 }
 assert(off == RP1_CFG(6) + RP1_SET || off == RP1_CFG(6) + RP1_CLR);
 event((off == RP1_CFG(6) + RP1_SET ? 0x100 : 0x200) | bits);
}
static uint32_t bus_read_4(struct resource *r, unsigned off) {
 assert(r == &mem);
 if (cleaning) {
  assert(off >= RP1_CFG(0) && off <= RP1_CFG(RP1_IRQS-1)); return 0;
 }
 assert(off == RP1_CFG(6)); event(0x300); return 0;
}
static void arm_irq_memory_barrier(unsigned n) { assert(n == 0); event(0x400); }
static int intr_isrc_dispatch(struct intr_irqsrc *s, void *frame) {
 assert(s == &sc.irq[6].isrc && frame == thread.td_intr_frame);
 event(0x500); return dispatch_result;
}

static int step(void) { return ++cleanup_calls == fail_cleanup ? EBUSY : 0; }
static int bus_teardown_intr(device_t d, struct resource *r, void *cookie) {
 assert(d == 1 && r == &mem && cookie == &sc); return step();
}
static int bus_release_resource(device_t d, int type, int rid, struct resource *r) {
 assert(d == 1 && r == &mem);
 assert((type == SYS_RES_IRQ && rid == RP1_IRQS) ||
  (type == SYS_RES_MEMORY && (rid == PCIR_BAR(1) || rid == PCIR_BAR(2))));
 return step();
}
static int intr_isrc_deregister(struct intr_irqsrc *src) {
 assert(src == &sc.irq[RP1_IRQS-1].isrc); return step();
}
static int pci_release_msi(device_t d) { assert(d == 1); return step(); }
static int rman_fini(struct rman *r) { assert(r == &sc.memory); return step(); }
static void pci_disable_busmaster(device_t d) { assert(d == 1); cleanup_calls++; }
'''

MAIN = r'''
static void sequence(const unsigned *expected, unsigned n) {
 assert(nevents == n);
 assert(memcmp(events, expected, n * sizeof(*expected)) == 0);
 nevents = 0;
}
#define SEQ(...) do { const unsigned e[] = { __VA_ARGS__ }; \
 sequence(e, sizeof(e) / sizeof(e[0])); } while (0)
int main(void) {
 unsigned bar, vector, trigger;
 uint64_t offset, base = UINT64_C(0xc040000000);
 uint32_t range[7] = {0xc0,0x40000000,0x02000000,0,0,0,0x410000};
 uint32_t dma[21] = {0x10,0,0x43000000,0x10,0,0x10,0,
  0xc0,0x40000000,0x02000000,0,0,0,0x410000,
  0,0,0x02000000,0x10,0,0x10,0};
 struct intr_map_data_fdt data = {INTR_MAP_DATA_FDT,2,{6,4}};
 struct intr_map_data *map = (struct intr_map_data *)&data;
 struct intr_irqsrc *isrc = NULL;
 struct rp1_irq *irq = &sc.irq[6];

 assert(rp1_window(base, 0x400000, base, 0x410000, &bar, &offset) == 0);
 assert(bar == 0 && offset == 0);
 assert(rp1_window(base+0x400000, 0x10000, base, 0x410000, &bar, &offset) == 0);
 assert(bar == 1 && offset == 0);
 assert(rp1_window(base+0x40ffff, 1, base, 0x410000, &bar, &offset) == 0);
 assert(bar == 1 && offset == 0xffff);
 assert(rp1_window(base+0x3fffff, 2, base, 0x410000, &bar, &offset) == ERANGE);
 assert(rp1_window(base+0x410000, 1, base, 0x410000, &bar, &offset) == ERANGE);
 assert(rp1_window(base+0x400000, 1, base, 0x400000, &bar, &offset) == ERANGE);
 assert(rp1_window(base-1, 1, base, 0x410000, &bar, &offset) == EINVAL);
 assert(rp1_window(base, 0, base, 0x410000, &bar, &offset) == EINVAL);
 assert(rp1_window(base, UINT64_MAX, base, 0x410000, &bar, &offset) == ERANGE);
 assert(rp1_window(UINT64_MAX, 2, base, 0x410000, &bar, &offset) == ERANGE);
 assert(rp1_dma_layout(dma, range) == 0);
 for (unsigned i=0; i<21; i++) {
  dma[i] ^= 1;
  assert(rp1_dma_layout(dma, range) == EINVAL);
  dma[i] ^= 1;
 }
 sc.bar[0] = &mem; irq->sc = &sc; irq->vector = 6;
 assert(rp1_decode(NULL, &vector, &trigger) == EINVAL);
 assert(rp1_map(1, map, &isrc) == 0 && isrc == &irq->isrc);
 data.type = 2; assert(rp1_map(1, map, &isrc) == EINVAL); data.type = 1;
 data.ncells = 1; assert(rp1_map(1, map, &isrc) == EINVAL); data.ncells = 2;
 data.cells[0] = RP1_IRQS; assert(rp1_map(1, map, &isrc) == EINVAL); data.cells[0] = 6;
 for (unsigned i=0; i<16; i++) {
  data.cells[1] = i;
  assert((rp1_decode(map, &vector, &trigger) == 0) == (i == 1 || i == 4));
 }
 data.cells[1] = 4;
 assert(rp1_setup(1, &sc.irq[5].isrc, NULL, map) == EINVAL);
 assert(rp1_setup(1, isrc, NULL, map) == 0);
 SEQ(0x201,0x300,0x108,0x300,0x104,0x300);
 assert(irq->trigger == 4);
 irq->isrc.isrc_handlers = 1;
 assert(rp1_setup(1, isrc, NULL, map) == 0 && nevents == 0);
 data.cells[1] = 1; assert(rp1_setup(1, isrc, NULL, map) == EINVAL);
 data.cells[1] = 4;
 rp1_pre_thread(1, isrc); SEQ(0x201,0x300);
 rp1_post_thread(1, isrc); SEQ(0x400,0x104,0x300,0x101,0x300);
 rp1_post_filter(1, isrc); SEQ(0x400,0x104,0x300);
 dispatch_result = 0; assert(rp1_filter(irq) == FILTER_HANDLED); SEQ(0x500);
 dispatch_result = 1; assert(rp1_filter(irq) == FILTER_HANDLED);
 SEQ(0x500,0x201,0x300,0x104,0x300);
 assert(rp1_teardown(1, isrc, NULL, map) == 0 && nevents == 0);
 irq->isrc.isrc_handlers = 0;
 assert(rp1_teardown(1, isrc, NULL, map) == 0); SEQ(0x201,0x300,0x208,0x300);
 assert(irq->trigger == 0);
 data.cells[1] = 1;
 assert(rp1_setup(1, isrc, NULL, map) == 0); SEQ(0x201,0x300,0x208,0x300);
 rp1_pre_thread(1, isrc); SEQ(0x201,0x300);
 rp1_post_thread(1, isrc); SEQ(0x400,0x101,0x300);
 rp1_post_filter(1, isrc); SEQ(0x400);
 assert(rp1_filter(irq) == FILTER_HANDLED); SEQ(0x500,0x201,0x300);
 cleaning = true;
 /* Fail each release operation, preserve the busy owner, then retry it. */
 for (unsigned fail=1; fail<=7; fail++) {
  memset(&sc, 0, sizeof(sc)); cleanup_calls = cfg_writes = 0; fail_cleanup = fail;
  sc.bar[0] = sc.bar[1] = &mem;
  sc.manager = sc.msix = sc.busmaster = sc.registers_valid = true;
  for (unsigned i=0; i<RP1_IRQS; i++) { sc.irq[i].sc = &sc; sc.irq[i].vector = i; }
  sc.irq[RP1_IRQS-1].cookie = &sc;
  sc.irq[RP1_IRQS-1].parent_irq = &mem;
  sc.irq[RP1_IRQS-1].registered = true;
  assert(rp1_cleanup(1) == EBUSY && cleanup_calls == fail);
  assert(sc.bar[0] == &mem && sc.registers_valid && sc.busmaster);
  assert((sc.irq[RP1_IRQS-1].cookie != NULL) == (fail == 1));
  assert((sc.irq[RP1_IRQS-1].parent_irq != NULL) == (fail <= 2));
  assert(sc.irq[RP1_IRQS-1].registered == (fail <= 3));
  assert(sc.msix == (fail <= 4));
  assert(sc.manager == (fail <= 5));
  assert((sc.bar[1] != NULL) == (fail <= 6));
  fail_cleanup = 0;
  assert(rp1_cleanup(1) == 0);
  assert(!sc.bar[0] && !sc.bar[1] && !sc.registers_valid && !sc.busmaster);
 }
 memset(&sc, 0, sizeof(sc)); cfg_writes = cleanup_calls = 0;
 sc.bar[0] = &mem; /* Undersized BAR during attach: never touch registers. */
 assert(rp1_cleanup(1) == 0 && cfg_writes == 0);
 sc.published = true;
 assert(rp1_cleanup(1) == EBUSY);
 puts("PASS: RP1 BAR/DMA validation, IRQ ordering and fault-injected cleanup");
 return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = SOURCE.read_text()
    constants = '\n'.join(re.findall(r'(?m)^#define RP1_.*$', source))
    functions = '\n'.join(function(source, name) for name in (
        'rp1_window', 'rp1_dma_layout', 'rp1_config', 'rp1_disable',
        'rp1_enable', 'rp1_pre_thread', 'rp1_post_filter', 'rp1_post_thread',
        'rp1_filter', 'rp1_decode', 'rp1_map', 'rp1_setup', 'rp1_teardown',
        'rp1_cleanup'))
    with tempfile.TemporaryDirectory(prefix='rp1-controls-') as tmp:
        path = Path(tmp)
        (path / 'controls.c').write_text(constants + '\n' + PREFIX + functions + MAIN)
        subprocess.run([args.cc, '-std=gnu17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', str(path / 'controls.c'),
                        '-o', str(path / 'controls')], check=True)
        subprocess.run([str(path / 'controls')], check=True)


if __name__ == '__main__':
    main()
