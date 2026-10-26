-- SPDX-License-Identifier: BSD-2-Clause
-- Portable fixture: run the example staged script with dofile, the function
-- the loader's include uses, against a recorded loader API.  This is not EFI
-- acceptance; the native VM case runs it in the real loader.
local root = assert(arg[1], "source root required")
local env = { ["netdebug.boot_id"] = "00112233445566778899aabbccddeeff",
	["staged.module"] = "/boot/kernel/if_em.ko" }
local commands, printed = {}, {}
loader = {
	getenv = function(name) return env[name] end,
	setenv = function(name, value) env[name] = value; return 0 end,
	command = function(...) commands[#commands + 1] = table.concat({...}, " "); return 0 end,
}
local real_print = print
print = function(...) printed[#printed + 1] = table.concat({...}, " ") end
dofile(root .. "/tools/daemonarchy/examples/staged-netdebug.lua")
print = real_print
assert(env["staged.netdebug"] == "ran", "script did not set its marker")
assert(env["boot_verbose"] == "YES")
assert(commands[1] == "load /boot/kernel/if_em.ko", commands[1])
assert(printed[1] == "netdebug-stage: staged-lua boot_id=00112233445566778899aabbccddeeff",
	printed[1])
env["netdebug.boot_id"] = nil
assert(not pcall(dofile, root .. "/tools/daemonarchy/examples/staged-netdebug.lua"),
	"missing identity must fail the include")
print("staged Lua fixture: ok")
