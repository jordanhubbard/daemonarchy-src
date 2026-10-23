-- SPDX-License-Identifier: BSD-2-Clause
-- Run with a Lua interpreter: lua loader-debug-setup.lua SOURCE_ROOT
local root = assert(arg[1], "source root required")
local original_print = print
local env, calls, output, input, available, uefi, start_result
local cli = {}
local core = {KEY_ENTER = 13, KEY_BACKSPACE = 8, KEY_DELETE = 127}
core.isUEFIBoot = function() return uefi end
package.loaded.cli = cli
package.loaded.core = core
loader = {
	getenv = function(k) return env[k] end,
	setenv = function(k, v) env[k] = v end,
	has_command = function() return available end,
	command_error = function() return "cannot read shared key" end,
	command = function(...)
		local args = {...}
		calls[#calls + 1] = args
		if args[2] == "start" then return start_result end
		return 0
	end,
}
print = function(s) output[#output + 1] = (s or "") .. "\n" end
printc = function(s) output[#output + 1] = s end
io.getchar = function()
	assert(#input > 0, "unexpected prompt")
	local ch = input:byte(1)
	input = input:sub(2)
	return ch
end
local setup = assert(loadfile(root .. "/stand/lua/netdebug.lua"))()
local function reset(keys)
	env, calls, output = {}, {}, {}
	input, available, uefi, start_result = keys or "", true, true, 0
end
local function contains(s)
	return table.concat(output):find(s, 1, true) ~= nil
end
local function starts()
	local found = {}
	for _, call in ipairs(calls) do
		if call[2] == "start" then found[#found + 1] = call end
	end
	return found
end

-- Successful interactive flow pauses boot, uses separate argv and preserves
-- the kernel coverage warning. Defaults never invent network addresses.
reset("\r192.0.2.10\r192.0.2.20\r\r")
assert(cli.netdebug("netdebug", "setup") == 0)
local call = assert(starts()[1])
assert(#call == 6 and call[3] == "0" and call[4] == "192.0.2.10")
assert(call[5] == "192.0.2.20" and call[6] == "/boot/debug.key")
assert(env.autoboot_delay == "NO" and env["netdebug.peer"] == "192.0.2.20")
assert(contains("ends at kernel handoff") and contains("automatic boot is paused"))

-- Unsupported loaders and BIOS leave boot settings untouched.
for _, mode in ipairs({"unsupported", "bios"}) do
	reset()
	available, uefi = mode ~= "unsupported", mode ~= "bios"
	assert(setup.setup() ~= 0 and #calls == 0 and next(env) == nil)
end

-- Esc and Ctrl-C at every prompt never activate or change saved settings.
local prefixes = {"", "\r", "\r192.0.2.10\r", "\r192.0.2.10\r192.0.2.20\r"}
for _, prefix in ipairs(prefixes) do
	for _, cancel in ipairs({string.char(27), string.char(3)}) do
		reset(prefix .. cancel)
		env["netdebug.ip"] = "192.0.2.99"
		assert(setup.setup() ~= 0 and #starts() == 0)
		assert(env["netdebug.ip"] == "192.0.2.99")
	end
end

-- Invalid addresses, identical peers, NIC input and editing are retried.
reset("bad\r1\b0\r999.1.1.1\r0.0.0.0\r224.0.0.1\r192.0.2.10\r192.0.2.10\r192.0.2.20\r\r")
assert(setup.setup() == 0 and #starts() == 1 and contains("Invalid value"))
assert(starts()[1][3] == "0")

-- Failed activation never claims pairing or saves an unusable profile.
reset("\r192.0.2.10\r192.0.2.20\r\r")
start_result = 1
assert(setup.setup() == 1 and contains("cannot read shared key"))
assert(env["netdebug.ip"] == nil and not contains("Waiting at the loader prompt"))

-- Key paths remain one argument, even with loader syntax and spaces.
local path = '/boot/key; boot "other"'
reset("\r192.0.2.10\r192.0.2.20\r" .. path .. "\r")
assert(setup.setup() == 0 and starts()[1][6] == path and #starts()[1] == 6)

-- Existing netdebug subcommands still dispatch to the native implementation.
reset()
assert(cli.netdebug("netdebug", "stop") == 0)
assert(#calls == 1 and calls[1][2] == "stop")

-- The actual menu exposes the shortcut only for Daemonarchy UEFI, and
-- returns to the prompt after setup rather than booting implicitly.
for i, name in ipairs({"MENU_ENTRY", "MENU_CAROUSEL_ENTRY", "MENU_SUBMENU",
    "MENU_RETURN", "MENU_SEPARATOR"}) do core[name] = i end
core.KEYSTR_ESCAPE = string.char(27)
core.isSingleUserBoot = function() return false end
package.loaded.color = {highlight = function(s) return s end}
for _, name in ipairs({"config", "screen", "drawer"}) do package.loaded[name] = {} end
package.loaded.netdebug = setup
local menu = assert(loadfile(root .. "/stand/lua/menu.lua"))()
local entry = menu.welcome.all_entries.netdebug
reset(string.char(27))
env.loader_brand = "daemonarchy"
assert(entry.visible() and entry.alias[1] == "n")
local found = false
for _, item in ipairs(menu.welcome.entries()) do
	if item == entry then found = true end
end
assert(found and entry.entry_type == core.MENU_RETURN)
assert(menu.handlers[entry.entry_type](menu.welcome, entry) == false)
assert(#starts() == 0)
env.loader_brand = "beastie"
assert(not entry.visible())
env.loader_brand, uefi = "daemonarchy", false
assert(not entry.visible())
original_print("loader debug setup: all interaction checks passed")
