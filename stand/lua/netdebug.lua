-- SPDX-License-Identifier: BSD-2-Clause
-- Copyright (c) 2026 Daemonarchy contributors.

local cli = require("cli")
local core = require("core")
local netdebug = {}

local function prompt(label, default, valid)
	while true do
		local value = ""
		printc(label .. (default ~= "" and " [" .. default .. "]" or "") .. ": ")
		while true do
			local ch = io.getchar()
			if ch == 27 or ch == 3 then
				print("")
				return nil
			elseif ch == core.KEY_ENTER or ch == 10 then
				print("")
				break
			elseif ch == core.KEY_BACKSPACE or ch == core.KEY_DELETE then
				if #value > 0 then
					value = value:sub(1, -2)
					printc("\008 \008")
				end
			elseif ch >= 32 and ch <= 126 and #value < 255 then
				value = value .. string.char(ch)
				printc(string.char(ch))
			end
		end
		if value == "" then value = default end
		if valid(value) then return value end
		print("Invalid value; try again, or press Esc to cancel.")
	end
end

local function ipv4(value)
	local a, b, c, d = value:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
	if not a then return false end
	for _, part in ipairs({a, b, c, d}) do
		if #part > 3 or tonumber(part) > 255 or
		    (#part > 1 and part:sub(1, 1) == "0") then return false end
	end
	-- Reject unspecified, multicast and reserved destinations.
	return tonumber(a) > 0 and tonumber(a) < 224 and tonumber(a) ~= 127
end

function netdebug.setup()
	if not core.isUEFIBoot() or not loader.has_command("netdebug") then
		print("This loader does not provide wired network debugging.")
		print("Use a Daemonarchy UEFI image with the debugging loader.")
		return 1
	end
	loader.setenv("autoboot_delay", "NO")
	print("Daemonarchy wired network debugging")
	print("Connect Ethernet to a trusted network; Wi-Fi is not supported here.")
	print("This console ends at kernel handoff; it cannot capture early kernel resets.")
	print("Use distinct static IPv4 addresses on the same link. Esc cancels.")
	loader.command("netdebug", "devices")
	print("If no interfaces were listed, press Esc; firmware must expose the NIC.")
	print("The shared key file must already be on the boot medium (not typed here).")
	local fields = {
		{"Firmware NIC number", "netdebug.interface", "0", function(v)
			return v:match("^%d+$") ~= nil and #v <= 3
		end},
		{"This machine's IPv4 address", "netdebug.ip", "", ipv4},
		{"Controller's IPv4 address", "netdebug.peer", "", ipv4},
		{"Shared key file", "netdebug.keyfile", "/boot/debug.key", function(v)
			return v:sub(1, 1) == "/" and #v <= 255
		end},
	}
	local values = {}
	for i, field in ipairs(fields) do
		values[i] = prompt(field[1], loader.getenv(field[2]) or field[3], function(v)
			return field[4](v) and (i ~= 3 or v ~= values[2])
		end)
		if values[i] == nil then
			print("Setup cancelled; any existing debugging session is unchanged.")
			return 1
		end
	end
	-- Pass parsed arguments directly: paths must never become loader commands.
	local result = loader.command("netdebug", "start", table.unpack(values))
	if result ~= 0 then
		print("Network debugging could not start: " ..
		    (loader.command_error() or "loader refused activation"))
		print("Check the wired NIC, addresses and shared key file; retry with netdebug setup.")
		return result
	end
	for i, field in ipairs(fields) do loader.setenv(field[2], values[i]) end
	loader.command("netdebug", "status")
	print("Waiting at the loader prompt for the controller; automatic boot is paused.")
	print("Use netdebug stop to revoke access, boot to continue, or menu to return.")
	return 0
end

function cli.netdebug(...)
	local argv = {...}
	if #argv == 2 and argv[2] == "setup" then return netdebug.setup() end
	return loader.command(...)
end

return netdebug
