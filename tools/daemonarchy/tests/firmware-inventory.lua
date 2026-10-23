-- SPDX-License-Identifier: BSD-2-Clause
-- Standalone bring-up medium: deliberately contains no kernel or installer.
require("cli")
require("netdebug")
loader.setenv("autoboot_delay", "NO")
print("Daemonarchy 1.3.1 firmware inventory")
print("No kernel or installer is present; internal disks are not modified.")
loader.perform("netdebug devices")
print("Record the NIC and UNDI status above. This is not a kernel connection.")
print("Use reboot to restart, or netdebug setup for loader-only networking.")
