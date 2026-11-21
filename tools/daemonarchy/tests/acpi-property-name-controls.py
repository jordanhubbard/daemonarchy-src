#!/usr/bin/env python3
"""Compile the actual ACPI _DSD lookup against exact/prefix property fixtures."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define ACPI_TYPE_PACKAGE 1
#define ACPI_TYPE_STRING 2
#define ACPI_TYPE_INTEGER 3
#define AE_OK 0
#define AE_NOT_FOUND 1
#define AE_BAD_PARAMETER 2
#define ACPI_FAILURE(x) ((x)!=AE_OK)
typedef int ACPI_STATUS;
typedef int device_t;
typedef struct obj ACPI_OBJECT;
struct obj {
 int Type;
 struct { size_t Count; ACPI_OBJECT *Elements; } Package;
 struct { size_t Length; const char *Pointer; } String;
 uint64_t value;
};
struct acpi_device { ACPI_OBJECT *dsd_pkg; struct { void *Pointer; } dsd; };
static struct acpi_device ad;
static int fail_find,find_calls,no_ivars;
static void *device_get_ivars(device_t d) {assert(d==1);return no_ivars?NULL:&ad;}
static ACPI_STATUS acpi_find_dsd(struct acpi_device *p) {assert(p==&ad);find_calls++;return fail_find?AE_NOT_FOUND:AE_OK;}
'''
TEST = r'''
static ACPI_OBJECT entries[3],pairs[3][2],package;
static void pair(unsigned n,const char *name,size_t length,uint64_t value) {
 pairs[n][0]=(ACPI_OBJECT){.Type=ACPI_TYPE_STRING,.String={length,name}};
 pairs[n][1]=(ACPI_OBJECT){.Type=ACPI_TYPE_INTEGER,.value=value};
 entries[n]=(ACPI_OBJECT){.Type=ACPI_TYPE_PACKAGE,.Package={2,pairs[n]}};
}
static void setup(void) {
 memset(entries,0,sizeof(entries));memset(pairs,0,sizeof(pairs));
 package=(ACPI_OBJECT){.Type=ACPI_TYPE_PACKAGE,.Package={3,entries}};
 ad=(struct acpi_device){.dsd_pkg=&package};no_ivars=fail_find=find_calls=0;
}
int main(int argc,char **argv) {
 const ACPI_OBJECT *out=NULL;(void)argv;
 setup();pair(0,"sdhci-caps",10,0x0120fa81);
 if(argc>1) {assert(acpi_device_get_prop(0,1,"sdhci-caps-mask",&out)==AE_OK&&out->value==0x0120fa81);puts("REPRODUCED: baseline aliases sdhci-caps-mask to sdhci-caps");return 0;}
 assert(acpi_device_get_prop(0,1,"sdhci-caps",&out)==AE_OK&&out->value==0x0120fa81);
 out=NULL;assert(acpi_device_get_prop(0,1,"sdhci-caps-mask",&out)==AE_NOT_FOUND&&out==NULL);
 pair(1,"sdhci-caps-mask",15,0x12345678);assert(acpi_device_get_prop(0,1,"sdhci-caps-mask",&out)==AE_OK&&out->value==0x12345678);
 pair(0,"sdhci-caps-mask",15,0xabcdef);pair(1,"sdhci-caps",10,0x0120fa81);
 assert(acpi_device_get_prop(0,1,"sdhci-caps",&out)==AE_OK&&out->value==0x0120fa81);
 assert(acpi_device_get_prop(0,1,"sdhci-cap",&out)==AE_NOT_FOUND);
 assert(acpi_device_get_prop(0,1,"SDHCI-CAPS",&out)==AE_NOT_FOUND);
 assert(acpi_device_get_prop(0,1,"sdhci-caps-mask-extra",&out)==AE_NOT_FOUND);
 pair(0,"",0,99);assert(acpi_device_get_prop(0,1,"sdhci-caps-mask",&out)==AE_NOT_FOUND);
 assert(acpi_device_get_prop(0,1,"",&out)==AE_OK&&out->value==99);
 // ACPI lengths bound comparisons; no read beyond a nonterminated name.
 char bounded[3]={'a','b','c'};pair(0,bounded,3,42);
 assert(acpi_device_get_prop(0,1,"abc",&out)==AE_OK&&out->value==42);
 assert(acpi_device_get_prop(0,1,"abcd",&out)==AE_NOT_FOUND);
 assert(acpi_device_get_prop(0,1,"ab",&out)==AE_NOT_FOUND);
 pair(0,NULL,3,0);assert(acpi_device_get_prop(0,1,"abc",&out)==AE_NOT_FOUND);
 pair(0,"abc",3,42);entries[0].Type=0;assert(acpi_device_get_prop(0,1,"abc",&out)==AE_NOT_FOUND);
 entries[0].Type=ACPI_TYPE_PACKAGE;entries[0].Package.Count=1;assert(acpi_device_get_prop(0,1,"abc",&out)==AE_NOT_FOUND);
 entries[0].Package.Count=2;pairs[0][0].Type=ACPI_TYPE_INTEGER;assert(acpi_device_get_prop(0,1,"abc",&out)==AE_NOT_FOUND);
 setup();pair(0,"abc",3,42);assert(acpi_device_get_prop(0,1,"abc",NULL)==AE_OK);
 assert(acpi_device_get_prop(0,1,NULL,&out)==AE_BAD_PARAMETER);
 no_ivars=1;assert(acpi_device_get_prop(0,1,"abc",&out)==AE_BAD_PARAMETER);no_ivars=0;
 ad.dsd_pkg=NULL;ad.dsd.Pointer=&ad;assert(acpi_device_get_prop(0,1,"abc",&out)==AE_NOT_FOUND&&find_calls==0);
 ad.dsd.Pointer=NULL;fail_find=1;assert(acpi_device_get_prop(0,1,"abc",&out)==AE_NOT_FOUND&&find_calls==1);
 puts("PASS: production _DSD exact names, prefix order, bounded nonterminated names, malformed names and cached/missing properties");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--baseline-source', type=Path)
    args = parser.parse_args()
    path = args.baseline_source or ROOT / 'sys/dev/acpica/acpi.c'
    source = path.read_text()
    match = re.search(r'(?m)^acpi_device_get_prop\(', source)
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    function = 'static ACPI_STATUS\n'+source[match.start():end]
    with tempfile.TemporaryDirectory(prefix='acpi-property-') as temp:
        path = Path(temp) / 'control.c'
        path.write_text(PREFIX+function+TEST)
        binary = Path(temp) / 'control'
        subprocess.run([args.cc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-sign-compare','-fsanitize=address,undefined',str(path),'-o',str(binary)],check=True)
        subprocess.run([str(binary)]+(['baseline'] if args.baseline_source else []),check=True)


if __name__ == '__main__':
    main()
