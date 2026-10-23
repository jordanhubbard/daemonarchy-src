#!/usr/bin/env python3
"""Compile the actual amd64 EFI clock against frozen/failing firmware controls."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2] / 'stand/efi/libefi/time.c')
parser.add_argument('--expect-broken', action='store_true')
args = parser.parse_args()
header = r'''
#ifndef TEST_EFI_H
#define TEST_EFI_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define EFIAPI
#define OUT
#define EFI_ERROR(s) ((s) != 0)
#define EVT_TIMER 0x80000000
#define EVT_NOTIFY_SIGNAL 0x200
#define TPL_CALLBACK 8
#define EFI_UNSPECIFIED_TIMEZONE 2047
#define EFI_TIME_ADJUST_DAYLIGHT 1
#define TimerCancel 0
#define TimerPeriodic 1
typedef unsigned long EFI_STATUS;
typedef void *EFI_EVENT;
typedef struct { uint16_t Year; uint8_t Month,Day,Hour,Minute,Second; uint32_t Nanosecond; int16_t TimeZone; uint8_t Daylight; } EFI_TIME;
typedef struct { int unused; } EFI_TIME_CAPABILITIES;
typedef void (*EFI_EVENT_NOTIFY)(EFI_EVENT, void *);
typedef struct { EFI_STATUS (*GetTime)(EFI_TIME *,EFI_TIME_CAPABILITIES *); } EFI_RUNTIME_SERVICES;
typedef struct { EFI_STATUS (*CreateEvent)(uint32_t,unsigned long,EFI_EVENT_NOTIFY,void *,EFI_EVENT *); EFI_STATUS (*SetTimer)(EFI_EVENT,int,uint64_t); EFI_STATUS (*CloseEvent)(EFI_EVENT); } EFI_BOOT_SERVICES;
extern EFI_RUNTIME_SERVICES *RS;
extern EFI_BOOT_SERVICES *BS;
#endif
'''
harness = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "efi.h"
#define time tested_time
#define getsecs tested_getsecs
#include SOURCE_PATH
#undef time
#undef getsecs
static EFI_EVENT_NOTIFY callback;
static int rtc_error,create_error,set_error,closed,cancelled,created,armed;
static EFI_TIME firmware_time = { .Year=2026,.Month=10,.Day=22,.Hour=12,.TimeZone=EFI_UNSPECIFIED_TIMEZONE };
static EFI_STATUS get_time(EFI_TIME *t,EFI_TIME_CAPABILITIES *c) { (void)c; if (rtc_error) return 1; *t=firmware_time; return 0; }
static EFI_STATUS create_event(uint32_t type,unsigned long tpl,EFI_EVENT_NOTIFY notify,void *context,EFI_EVENT *event) {
 assert(type==(EVT_TIMER|EVT_NOTIFY_SIGNAL)); assert(tpl==TPL_CALLBACK); assert(context==NULL);
 created++; if (create_error) return 1; callback=notify; *event=(void *)1; return 0;
}
static EFI_STATUS set_timer(EFI_EVENT event,int type,uint64_t trigger) {
 assert(event==(void *)1); if (type==TimerCancel) { cancelled++; armed=0; return 0; }
 assert(type==TimerPeriodic && trigger==10000000); if (set_error) return 1; armed=1; return 0;
}
static EFI_STATUS close_event(EFI_EVENT event) { assert(event==(void *)1); closed++; armed=0; return 0; }
static EFI_RUNTIME_SERVICES runtime = { get_time };
static EFI_BOOT_SERVICES boot = { create_event,set_timer,close_event };
EFI_RUNTIME_SERVICES *RS=&runtime;
EFI_BOOT_SERVICES *BS=&boot;
static void tick(void) { assert(callback && armed); callback((void *)1,NULL); }
int main(int argc,char **argv) {
 assert(argc==2);
 if (!strcmp(argv[1],"rtc-error")) rtc_error=1;
 if (!strcmp(argv[1],"create-error")) create_error=1;
 if (!strcmp(argv[1],"set-error")) set_error=1;
 efi_time_init();
 if (create_error || set_error) {
  assert(created==1); assert(closed==(set_error?1:0)); assert(!armed);
  assert(tested_time(NULL)!=0); efi_time_fini(); assert(closed==(set_error?1:0)); return 0;
 }
 time_t before=tested_time(NULL),stored;
 assert(callback && armed);
 if (rtc_error) assert(before==0);
 if (!strcmp(argv[1],"backwards")) firmware_time.Year=2000;
 tick(); tick(); tick();
 assert(tested_time(&stored)==before+3 && stored==before+3);
 assert(tested_getsecs()==before+3);
 efi_time_init(); assert(created==1); /* idempotent */
 efi_time_fini(); assert(closed==1 && cancelled==1 && !armed);
 efi_time_fini(); assert(closed==1 && cancelled==1);
 if (!strcmp(argv[1],"reinit")) { efi_time_init(); assert(created==2); tick(); efi_time_fini(); assert(closed==2 && cancelled==2); }
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efi-time-test-') as name:
    work = Path(name)
    (work / 'efi.h').write_text(header)
    (work / 'efilib.h').write_text('#include "efi.h"\n')
    (work / 'test.c').write_text(harness.replace('SOURCE_PATH', '"' + str(args.source.resolve()) + '"'))
    binary = work / 'test'
    subprocess.run(['cc', '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror', '-I', str(work), str(work/'test.c'), '-o', str(binary)], check=True)
    cases = ('frozen', 'rtc-error', 'backwards', 'reinit', 'create-error', 'set-error')
    if args.expect_broken:
        result = subprocess.run([str(binary), 'frozen'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert result.returncode != 0, 'old implementation unexpectedly passed frozen RTC control'
        print('PASS: original clock reproduces frozen-RTC countdown failure')
    else:
        for case in cases:
            subprocess.run([str(binary), case], check=True)
            print('PASS:', case)
