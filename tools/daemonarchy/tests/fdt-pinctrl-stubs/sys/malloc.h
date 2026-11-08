/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef PINCTRL_TEST_MALLOC_H
#define PINCTRL_TEST_MALLOC_H
#include <stddef.h>
void *pinctrl_test_mallocarray(size_t, size_t);
void pinctrl_test_free(void *);
#define M_TEMP 0
#define M_WAITOK 0
#define mallocarray(n, size, type, flags) pinctrl_test_mallocarray(n, size)
#define free(ptr, type) pinctrl_test_free(ptr)
#endif
