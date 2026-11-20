#!/usr/bin/env python3
"""Exercise the production legacy Arasan ACPI accessors and attach callbacks.

A bus trace checks observable packed-register transactions, not a second copy
of the driver algorithm. Sanitizers catch invalid shifts/accesses. This does
not establish physical SD clocking, firmware pin routing or card I/O.
"""
import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'sys/dev/sdhci/sdhci_acpi.c'
spec = importlib.util.spec_from_file_location('pi4_sd', Path(__file__).with_name('pi4-acpi-sd-controls.py'))
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def function(name):
    source = SOURCE.read_text()
    match = re.search(r'(?m)^static (?:int|uint8_t|uint16_t|uint32_t|void)\n' + name + r'\(', source)
    start = match.start()
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


BUS = r'''
static uint32_t registers[256],write_values[64];
static unsigned write_offsets[64],writes,reads,subwords,delay_total;
static uint64_t firmware_caps;
static int firmware_size;
static void delay(unsigned n) { delay_total+=n; }
#define DELAY(n) delay(n)
static uint32_t bus_read_4(struct resource *r,bus_size_t off) {
 assert(r==&mem&&off%4==0&&off<sizeof(registers));reads++;return registers[off/4];
}
static void bus_write_4(struct resource *r,bus_size_t off,uint32_t value) {
 assert(r==&mem&&off%4==0&&off<sizeof(registers)&&writes<64);
 write_offsets[writes]=off;write_values[writes++]=value;registers[off/4]=value;
}
static uint8_t bus_read_1(struct resource *r,bus_size_t o) {assert(r==&mem);subwords++;return ((uint8_t*)registers)[o];}
static uint16_t bus_read_2(struct resource *r,bus_size_t o) {assert(r==&mem&&o%2==0);subwords++;return (uint16_t)(registers[o/4]>>((o%4)*8));}
static void bus_write_1(struct resource *r,bus_size_t o,uint8_t v) {assert(r==&mem);subwords++;((uint8_t*)registers)[o]=v;}
static void bus_write_2(struct resource *r,bus_size_t o,uint16_t v) {assert(r==&mem&&o%2==0);subwords++;registers[o/4]=(registers[o/4]&~(0xffffU<<((o%4)*8)))|((uint32_t)v<<((o%4)*8));}
'''
PROPERTY = r'''
static long device_get_property(device_t d,const char *name,void *out,size_t len,int type) {
 assert(d==1&&len==8&&type==DEVICE_PROP_UINT64);
 if(!strcmp(name,"sdhci-caps")) {*(uint64_t*)out=firmware_caps;return firmware_size;}
 assert(!strcmp(name,"sdhci-caps-mask"));*(uint64_t*)out=property;return property_size;
}
'''
TEST = r'''
static void reset_bcm(void) {
 reset("BCM2847");
 for(unsigned n=0;sdhci_acpi_devices[n].hid;n++)
  if(!strcmp(sdhci_acpi_devices[n].hid,"BCM2847")) selected=sdhci_acpi_devices[n];
 assert(selected.quirks&SDHCI_QUIRK_BROKEN_DMA);
 firmware_caps=0x0120fa81;firmware_size=8;
 writes=reads=subwords=delay_total=0;memset(registers,0,sizeof(registers));
}
int main(void) {
 bool id=false;for(unsigned n=0;sdhci_ids[n];n++) if(!strcmp(sdhci_ids[n],"BCM2847")) id=true;
 assert(id);
 reset_bcm();
 assert(sdhci_acpi_attach(1)==0&&starts==1&&sc.bcm_32bit);
 assert(seen_caps==0x0120fa81&&seen_caps2==0);
 assert(sc.slot.max_clk==250000000&&(sc.slot.opt&SDHCI_MAX_CLK_FIXED));
 // A v2 core would otherwise decode the firmware clock as 58 MHz.
 assert(((seen_caps&SDHCI_CLOCK_BASE_MASK)>>SDHCI_CLOCK_BASE_SHIFT)==58);
 assert((seen_quirks&(SDHCI_QUIRK_BROKEN_DMA|SDHCI_QUIRK_BROKEN_TIMEOUT_VAL|SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK|SDHCI_QUIRK_DONT_SET_HISPD_BIT))==(SDHCI_QUIRK_BROKEN_DMA|SDHCI_QUIRK_BROKEN_TIMEOUT_VAL|SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK|SDHCI_QUIRK_DONT_SET_HISPD_BIT));
 sdhci_acpi_detach(1);assert(cleanups==1&&releases==2);
 for(unsigned n=0;n<5;n++) {
  reset_bcm();if(n==0)firmware_size=-1;else if(n==1)firmware_size=4;else if(n==2)firmware_caps=0;else if(n==3)firmware_caps&=~SDHCI_CLOCK_V3_BASE_MASK;else firmware_caps&=~SDHCI_CAN_VDD_330;
  assert(sdhci_acpi_attach(1)==ENXIO&&allocations==0&&starts==0);
 }
 reset_bcm();firmware_caps=~0ULL;property=(1ULL<<10);property_size=8;
 sdhci_quirk_clear=~0U;sdhci_quirk_set=SDHCI_QUIRK_FORCE_DMA;
 assert(sdhci_acpi_attach(1)==0);
 assert(!(seen_quirks&SDHCI_QUIRK_FORCE_DMA));
 assert((seen_caps&(SDHCI_CAN_VDD_180|SDHCI_CAN_VDD_300|SDHCI_CAN_DO_DMA|SDHCI_CAN_DO_ADMA2|(1U<<10)))==0);
 assert(!(seen_caps2&(SDHCI_CAN_SDR50|SDHCI_CAN_SDR104|SDHCI_CAN_DDR50)));
 assert(seen_quirks&SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK);
 sdhci_acpi_detach(1);
 for(unsigned failure=1;failure<=4;failure++) {
  reset_bcm();fail_at=failure;assert(sdhci_acpi_attach(1)!=0);
  assert(starts==0&&cleanups==(failure==4));assert(releases==(failure==1?0:failure==2?1:2));
 }
 reset_bcm();sc.bcm_32bit=true;sc.mem_res=&mem;sc.slot.clock=400000;
 registers[0x28/4]=0x89abcdef;
 for(unsigned n=0;n<4;n++) assert(sdhci_acpi_read_1(1,&sc.slot,0x28+n)==(uint8_t)(0x89abcdefU>>(8*n)));
 assert(sdhci_acpi_read_2(1,&sc.slot,0x28)==0xcdef&&sdhci_acpi_read_2(1,&sc.slot,0x2a)==0x89ab);
 sdhci_acpi_write_1(1,&sc.slot,0x2b,0xfe);assert(writes==1&&write_offsets[0]==0x28&&write_values[0]==0xfeabcdef&&delay_total==6);
 sdhci_acpi_write_2(1,&sc.slot,0x2a,0xfedc);assert(writes==2&&write_values[1]==0xfedccdef&&delay_total==12);
 // Block-count changes made by hardware must not be replaced with stale cache.
 registers[SDHCI_BLOCK_SIZE/4]=0x00052200;
 sdhci_acpi_write_2(1,&sc.slot,SDHCI_BLOCK_COUNT,0xffff);
 sdhci_acpi_write_2(1,&sc.slot,SDHCI_BLOCK_SIZE,0x0200);
 assert(writes==2&&sc.need_update_blk);
 assert(sdhci_acpi_read_2(1,&sc.slot,SDHCI_BLOCK_SIZE)==0x0200);
 assert(sdhci_acpi_read_2(1,&sc.slot,SDHCI_BLOCK_COUNT)==0xffff);
 registers[SDHCI_TRANSFER_MODE/4]=0xabcd0000;
 sdhci_acpi_write_2(1,&sc.slot,SDHCI_TRANSFER_MODE,0x0033);
 assert(writes==2&&sdhci_acpi_read_2(1,&sc.slot,SDHCI_TRANSFER_MODE)==0x0033);
 sdhci_acpi_write_2(1,&sc.slot,SDHCI_COMMAND_FLAGS,0xff12);
 assert(writes==4&&write_offsets[2]==SDHCI_BLOCK_SIZE&&write_values[2]==0xffff0200);
 assert(write_offsets[3]==SDHCI_TRANSFER_MODE&&write_values[3]==0xff120033&&!sc.need_update_blk);
 assert(sdhci_acpi_read_2(1,&sc.slot,SDHCI_COMMAND_FLAGS)==0xff12);
 registers[SDHCI_BLOCK_SIZE/4]=0x00012200;
 sdhci_acpi_write_2(1,&sc.slot,SDHCI_BLOCK_SIZE,0x0200);
 sdhci_acpi_write_2(1,&sc.slot,SDHCI_COMMAND_FLAGS,0x1234);
 assert(write_values[4]==0x00010200&&write_values[5]==0x12340033);
 sdhci_acpi_write_4(1,&sc.slot,SDHCI_INT_STATUS,0x80000000);
 assert(write_values[6]==0x80000000&&delay_total==42&&subwords==0);
 sc.slot.clock=0;sdhci_acpi_write_4(1,&sc.slot,SDHCI_INT_STATUS,1);assert(delay_total==42);
 sc.slot.clock=25000000;sdhci_acpi_write_4(1,&sc.slot,SDHCI_INT_STATUS,2);assert(delay_total==43);
 // Other HIDs keep their existing subword access path and acquire no delay.
 sc.bcm_32bit=false;
 sdhci_acpi_write_1(1,&sc.slot,0x2b,0x12);assert(sdhci_acpi_read_1(1,&sc.slot,0x2b)==0x12);
 sdhci_acpi_write_2(1,&sc.slot,0x2a,0x5678);assert(sdhci_acpi_read_2(1,&sc.slot,0x2a)==0x5678&&subwords==4&&delay_total==43);
 puts("PASS: actual BCM2847 ID/capabilities/attach, 32-bit-only MMIO, command/block transactions, write spacing, failure cleanup and unrelated HID preservation");
 return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = SOURCE.read_text()
    definitions = '\n'.join(re.findall(r'^#define\s+SDHCI_\w+[^\n]*', (ROOT / 'sys/dev/sdhci/sdhci.h').read_text(), re.M))
    prefix = base.PREFIX.replace('struct sdhci_slot { unsigned quirks,max_clk,opt; };', 'struct sdhci_slot { unsigned quirks,max_clk,opt,clock; };')
    prefix = prefix.replace(' bool bcm_32bit;', ' uint32_t blksz_and_count,cmd_and_mode;\n bool bcm_32bit,need_update_blk;')
    prefix = re.sub(r'static uint32_t bus_read_4[^\n]*\n', BUS, prefix)
    prefix = re.sub(r'static long device_get_property[^\n]*\n', PROPERTY, prefix)
    prefix = prefix.replace('static uint32_t rawcaps=0xffffffff,rawcaps2=0xffffffff,seen_caps,seen_caps2;', 'static uint32_t seen_caps,seen_caps2;')
    table = source[source.index('static const struct sdhci_acpi_device {'):source.index('struct sdhci_acpi_softc {')]
    prefix = prefix.replace('struct sdhci_acpi_device { const char *hid; int uid; const char *desc; unsigned quirks; };', table)
    helpers = (ROOT / 'sys/dev/sdhci/sdhci_acpi_bcm2835.h').read_text()
    callbacks = '\n'.join(function('sdhci_acpi_'+name) for name in ('read_1','write_1','read_2','write_2','read_4','write_4','attach','detach'))
    reset = base.TEST[base.TEST.index('static void reset('):base.TEST.index('int main(')]
    with tempfile.TemporaryDirectory(prefix='bcm2847-acpi-') as temp:
        path = Path(temp) / 'control.c'
        path.write_text(definitions+'\n'+prefix+'\n'+helpers+'\n'+callbacks+'\n'+reset+TEST)
        binary = Path(temp) / 'control'
        subprocess.run([args.cc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-sign-compare','-fsanitize=address,undefined',str(path),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    main()
