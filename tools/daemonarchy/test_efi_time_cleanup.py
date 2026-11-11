#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise the original ARM32/RISC-V EFI timer against strict firmware stubs."""
from pathlib import Path
import json
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / "stand/efi/libefi/time_event.c"

EFI = r"""
#ifndef FIXTURE_EFI_H
#define FIXTURE_EFI_H
#include <stdint.h>
typedef void *EFI_EVENT;
typedef uintptr_t EFI_STATUS;
#define EFIAPI
#define EVT_TIMER 0x80000000U
#define EVT_NOTIFY_SIGNAL 0x200U
#define TPL_CALLBACK 8
#define TimerPeriodic 1
#define TimerCancel 0
#define EFI_FIXTURE_ERROR ((EFI_STATUS)1 << (sizeof(EFI_STATUS) * 8 - 1))
#define EFI_ERROR(x) (((EFI_STATUS)(x) & EFI_FIXTURE_ERROR) != 0)
struct fixture_services {
    EFI_STATUS (*CreateEvent)(unsigned, unsigned,
        void (*)(EFI_EVENT, void *), void *, EFI_EVENT *);
    EFI_STATUS (*SetTimer)(EFI_EVENT, int, uint64_t);
    EFI_STATUS (*CloseEvent)(EFI_EVENT);
};
extern struct fixture_services *BS;
#endif
"""

FIXTURE = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "efi.h"
static unsigned creates, arms, cancels, closes, live;
static unsigned fail_create, fail_arm;
static char token;
static void (*tick)(EFI_EVENT, void *);
static EFI_STATUS create(unsigned type, unsigned priority,
    void (*callback)(EFI_EVENT, void *), void *context, EFI_EVENT *event)
{
    creates++;
    assert(!live && context == NULL);
    assert(type == (EVT_TIMER | EVT_NOTIFY_SIGNAL) && priority == TPL_CALLBACK);
    if (fail_create) { *event = NULL; return EFI_FIXTURE_ERROR; }
    tick = callback; live = 1; *event = &token; return 0;
}
static EFI_STATUS arm(EFI_EVENT event, int kind, uint64_t interval)
{
    assert(event == &token && live);
    if (kind == TimerPeriodic) {
        arms++; assert(interval == 10000000);
        return fail_arm ? EFI_FIXTURE_ERROR : 0;
    }
    assert(kind == TimerCancel && interval == 0); cancels++; return 0;
}
static EFI_STATUS close_event(EFI_EVENT event)
{
    assert(event == &token && live);
    closes++; live = 0; return 0;
}
static struct fixture_services services = { create, arm, close_event };
struct fixture_services *BS = &services;
#include SOURCE_INCLUDE
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "before-init")) {
        efi_time_fini(); assert(!creates && !arms && !cancels && !closes);
    } else if (!strcmp(argv[1], "repeat")) {
        efi_time_init(); efi_time_init();
        assert(creates == 1 && arms == 1 && live);
        efi_time_fini(); efi_time_fini();
        assert(cancels == 1 && closes == 1 && !live);
    } else if (!strcmp(argv[1], "create-failure")) {
        fail_create = 1; efi_time_init(); efi_time_fini();
        assert(creates == 1 && !arms && !closes && !live);
    } else if (!strcmp(argv[1], "arm-failure")) {
        fail_arm = 1; efi_time_init(); efi_time_fini();
        assert(creates == 1 && arms == 1 && !cancels && closes == 1 && !live);
    } else if (!strcmp(argv[1], "retry")) {
        fail_create = 1; efi_time_init();
        fail_create = 0; efi_time_init(); efi_time_fini();
        efi_time_init(); efi_time_fini();
        assert(creates == 3 && arms == 2 && cancels == 2 && closes == 2 && !live);
    } else if (!strcmp(argv[1], "tick")) {
        efi_time_init(); tick(&token, NULL); tick(&token, NULL); tick(&token, NULL);
        time_t value = 0;
        assert(fixture_time(&value) == 3 && value == 3 && fixture_getsecs() == 3);
        efi_time_fini(); assert(!live);
    } else return 64;
    return 0;
}
"""

class EfiTimerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest("C compiler unavailable for original EFI unit")
        cls.temp = tempfile.TemporaryDirectory(prefix="efi-timer-")
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(cls.temp.name)
        (root / "efi.h").write_text(EFI)
        (root / "efilib.h").write_text('#include "efi.h"\n')
        (root / "fixture.c").write_text(
            FIXTURE.replace("SOURCE_INCLUDE", json.dumps(str(SOURCE)))
        )
        cls.binary = root / "fixture"
        result = subprocess.run(
            [*compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", "-Dtime=fixture_time",
             "-Dgetsecs=fixture_getsecs", "-I", str(root),
             str(root / "fixture.c"), "-o", str(cls.binary)],
            capture_output=True, text=True, timeout=60
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def check_case(self, case):
        result = subprocess.run([str(self.binary), case],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cleanup_before_initialization(self):
        self.check_case("before-init")

    def test_repeated_initialization_and_cleanup(self):
        self.check_case("repeat")

    def test_failed_creation_never_arms_or_closes_null(self):
        self.check_case("create-failure")

    def test_failed_arming_closes_once(self):
        self.check_case("arm-failure")

    def test_failed_start_can_retry_and_restart(self):
        self.check_case("retry")

    def test_timer_callback_advances_both_time_interfaces(self):
        self.check_case("tick")

if __name__ == "__main__":
    unittest.main()
