-- SPDX-License-Identifier: BSD-2-Clause
-- Copyright (c) 2026 Daemonarchy contributors.
--
-- Menu support for the reset-surviving boot trace.  The menu entry turns on
-- debug.boottrace for one boot.  loader.conf may name a preset to read first
-- (boottrace_conf), an alternate kernel (boottrace_kernel) and a label
-- (boottrace_label), so an operator never types tunables at the prompt.

local config = require("config")
local core = require("core")
local boottrace = {}

function boottrace.available()
	return core.isUEFIBoot() and loader.has_command("boottrace")
end

function boottrace.label()
	return loader.getenv("boottrace_label") or "with boot trace"
end

function boottrace.boot(argstr)
	local conf = loader.getenv("boottrace_conf")
	if conf ~= nil and conf ~= "" then
		config.readConf(conf)
	end
	local kernel = loader.getenv("boottrace_kernel")
	if kernel ~= nil and kernel ~= "" then
		config.selectKernel(kernel)
	end
	loader.setenv("debug.boottrace", "1")
	print("Boot trace enabled: the kernel records each startup step " ..
	    "in NVRAM.")
	print("If this boot hangs or resets, boot this medium again to " ..
	    "see where it stopped.")
	core.boot(argstr)
end

local function delay_ms(ms)
	if loader.delay ~= nil then
		loader.delay(ms * 1000)
	end
end

-- Unattended iteration loop (boottrace_loop="YES" in loader.conf).  Start
-- the wired debug console from the netdebug.* settings, give a controller
-- boottrace_controller_wait seconds to send this iteration's settings (it
-- reads the previous trace with "boottrace report" and ends with "boot"),
-- and otherwise boot the default trace preset.  Returns "prompt" when the
-- controller (or a local key) took over the loader prompt.
function boottrace.loop()
	local wait = tonumber(loader.getenv("boottrace_controller_wait") or "25")
	local up = loader.has_command("netdebug") and
	    loader.perform("netdebug auto") == 0
	if up then
		print("boottrace loop: waiting " .. wait .. " s for the controller" ..
		    " (any key gives the local prompt)")
		local deadline = loader.time() + wait
		while loader.time() < deadline do
			if io.ischar() then
				loader.setenv("autoboot_delay", "NO")
				print("boottrace loop: controller attached; " ..
				    "awaiting its commands")
				return "prompt"
			end
			delay_ms(50)
		end
		print("boottrace loop: no controller answered")
	else
		print("boottrace loop: wired debug console unavailable")
	end
	print("boottrace loop: booting the default trace preset " ..
	    (loader.getenv("boottrace_conf") or "(none)"))
	boottrace.boot("-v")
	-- Only reached when the boot itself failed.
	loader.setenv("autoboot_delay", "NO")
	return "prompt"
end

-- Stick workflow: file the previous trace on the stick's DATRACE volume,
-- stage the next queued experiment and boot it.  The settings applied, in
-- order: kernel trace + verbose + panic defaults, then the queue entry's
-- lines (name=value, unset name, read-conf /path), which may override them.
function boottrace.cycle()
	loader.perform("boottrace cycle")
	local result = loader.getenv("boottrace_cycle")
	if result ~= "boot" then
		return result
	end
	loader.setenv("debug.boottrace", "1")
	loader.setenv("boot_verbose", "YES")
	loader.setenv("debug.trace_on_panic", "1")
	loader.setenv("debug.debugger_on_panic", "0")
	-- Reset after a panic so an unattended stick moves on; the panic
	-- message is in the trace.  A queue entry may override this.
	loader.setenv("kern.panic_reboot_wait_time", "15")
	local count = tonumber(loader.getenv("boottrace_q_count") or "0")
	for i = 1, count do
		local line = loader.getenv("boottrace_q_" .. i) or ""
		local name, value = line:match("^([%w%._%-]+)%s*=%s*(.-)%s*$")
		local unset = line:match("^unset%s+([%w%._%-]+)%s*$")
		local conf = line:match("^read%-conf%s+(%S+)%s*$")
		if name ~= nil then
			value = value:gsub('^"(.*)"$', "%1")
			loader.setenv(name, value)
		elseif unset ~= nil then
			loader.unsetenv(unset)
		elseif conf ~= nil then
			config.readConf(conf)
		else
			print("boottrace: ignoring queue line: " .. line)
		end
		loader.unsetenv("boottrace_q_" .. i)
	end
	local kernel = loader.getenv("kernel")
	if kernel ~= nil and kernel ~= "" then
		config.selectKernel(kernel)
	end
	print("Hardware trace: booting experiment " ..
	    (loader.getenv("boottrace_q_id") or "?"))
	core.boot("-v")
	loader.setenv("autoboot_delay", "NO")
	return "failed"
end

-- Called by loader.lua after loader.conf is read.
function boottrace.startup()
	-- The unattended cycle belongs to the hardware trace image only.
	if loader.getenv("boottrace_autocycle") == "YES" and
	    loader.getenv("boottrace_stick") == "YES" then
		local delay = tonumber(loader.getenv("boottrace_autocycle_delay") or "5")
		print("Hardware trace: running the next queued experiment in " ..
		    delay .. " s; press any key for the menu.")
		local deadline = loader.time() + delay
		while loader.time() < deadline do
			if io.ischar() then
				io.getchar()
				return nil
			end
			delay_ms(50)
		end
		-- Stopped or failed: stay at the prompt with the reason visible.
		boottrace.cycle()
		return "prompt"
	end
	local loop = loader.getenv("boottrace_loop") == "YES"
	local wait = loop and "0" or (loader.getenv("boottrace_wait") or "60")
	loader.perform("boottrace startup " .. wait)
	if loop then
		return boottrace.loop()
	end
	return nil
end

return boottrace
