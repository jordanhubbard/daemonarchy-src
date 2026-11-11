/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef PINCTRL_TEST_PARAM_H
#define PINCTRL_TEST_PARAM_H
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t phandle_t;
struct device { phandle_t node; unsigned number; };
typedef struct device *device_t;
extern int bootverbose;
#endif
