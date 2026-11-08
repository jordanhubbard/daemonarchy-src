/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef PINCTRL_TEST_IF_H
#define PINCTRL_TEST_IF_H
int pinctrl_test_configure(device_t, phandle_t);
#define FDT_PINCTRL_CONFIGURE(dev, xref) pinctrl_test_configure(dev, xref)
#endif
