--
-- SPDX-License-Identifier: BSD-2-Clause
--
-- Daemonarchy boot splash: a short, visual-only animated wordmark and mascot
-- drawn once before the loader menu when loader_brand="daemonarchy".
--
--   daemonarchy_splash="YES"     animate (default)
--   daemonarchy_splash="static"  draw the final frame only (reduced motion)
--   daemonarchy_splash="NO"      no splash (also "no", "off", "0")
--
-- UEFI has no reliable audio, so the splash is silent; the installed system
-- and installer read daemonarchy.sound from the kernel environment instead,
-- and the splash only shows its value when one is set.
--
-- The animation is bounded (at most 0.75 s of delays in total), skips at the
-- first keypress without consuming the key, so the menu's autoboot countdown
-- still sees it, and draws nothing on serial or null consoles, without color,
-- or when the menu itself is skipped. Every failure is contained: a splash
-- problem can never stop the boot.
--

local splash = {}

-- The same wordmark as the product's loader brand (brand-daemonarchy.lua).
splash.wordmark = {
	" ___                                      _",
	"|   \\ __ _ ___ _ __  ___ _ _  __ _ _ _ __| |_ _  _",
	"| |) / _` / -_) '  \\/ _ \\ ' \\/ _` | '_/ _| ' \\ || |",
	"|___/\\__,_\\___|_|_|_\\___/_||_\\__,_|_| \\__|_||_\\_, |",
	"                                              |__/",
}

-- A small daemon: horns, eyes that blink once, a grin and a pointed tail.
splash.mascot_open = {
	" /\\     /\\",
	"(  \\___/  )",
	" ) (o) (o) (",
	"(    \\_/    )",
	" `-._____.-'~>",
}
splash.mascot_shut = {
	" /\\     /\\",
	"(  \\___/  )",
	" ) (-) (-) (",
	"(    \\_/    )",
	" `-._____.-'~>",
}

splash.tagline = "summoning the daemons..."

-- Layout, in 1-based screen cells, within an 80x24 console.
splash.top = 3
splash.mascot_x = 3
splash.word_x = 20
splash.budget_us = 750000

-- Red -> magenta -> blue ("purple") across the wordmark, with the bright
-- attribute for the leading spark and the shimmer band. Only the eight ANSI
-- colors the loader terminals all support are used.
local RED, MAGENTA, BLUE, WHITE = 1, 5, 4, 7

local function setting()
	local value = loader.getenv("daemonarchy_splash")
	if value == nil then
		return "yes"
	end
	value = value:lower()
	if value == "no" or value == "off" or value == "0" or
	    value == "false" then
		return "no"
	end
	if value == "static" then
		return "static"
	end
	return "yes"
end

-- Returns nil when the splash should not draw, else "yes" or "static".
function splash.mode()
	local core = require("core")
	local color = require("color")
	if loader.getenv("loader_brand") ~= "daemonarchy" then
		return nil
	end
	local mode = setting()
	if mode == "no" then
		return nil
	end
	if core.isMenuSkipped() then
		return nil
	end
	-- autoboot_delay=-1 asks for an immediate boot: do not slow it.
	if (loader.getenv("autoboot_delay") or "") == "-1" then
		return nil
	end
	if color.disabled or not color.isEnabled() then
		return nil
	end
	local console = loader.getenv("console") or ""
	if core.isSerialConsole() or console:find("nullconsole") ~= nil or
	    console:find("spinconsole") ~= nil then
		return nil
	end
	-- The splash fits an 80x24 text screen; refuse a known smaller one.
	local cols = tonumber(loader.getenv("screen.textcols") or "")
	local rows = tonumber(loader.getenv("screen.textrows") or "")
	if (cols ~= nil and cols < 80) or (rows ~= nil and rows < 24) then
		return nil
	end
	return mode
end

local CSI = "\027["

-- The loader's Lua has no table or math library (stand/liblua leaves out
-- ltablib.c and lmathlib.c), so lists are joined by hand.
local function join(list)
	local text = ""
	for _, part in ipairs(list) do
		text = text .. part
	end
	return text
end

local function at(x, y)
	return CSI .. y .. ";" .. x .. "H"
end

local function fg(value, bright)
	if bright then
		return CSI .. "1;3" .. value .. "m"
	end
	return CSI .. "22;3" .. value .. "m"
end

local function reset()
	return CSI .. "0m"
end

-- The color of wordmark column c (0-based) when settled.
local function band(c)
	if c < 18 then
		return RED
	elseif c < 36 then
		return MAGENTA
	end
	return BLUE
end

local function sound_text()
	local sound = loader.getenv("daemonarchy.sound")
	if sound == nil then
		return ""
	end
	sound = sound:lower()
	if sound ~= "on" and sound ~= "off" and sound ~= "quiet" then
		return ""
	end
	return "   sound: " .. sound
end

-- One wordmark frame. reveal: columns shown (nil = all); shine: column of
-- the bright shimmer band (nil = none).
function splash.wordmark_frame(reveal, shine)
	local out = {}
	for i, line in ipairs(splash.wordmark) do
		local parts = { at(splash.word_x, splash.top + i - 1) }
		local last
		for c = 0, #line - 1 do
			local ch = line:sub(c + 1, c + 1)
			local style
			if reveal ~= nil and c >= reveal then
				ch = " "
				style = ""
			elseif reveal ~= nil and c >= reveal - 2 then
				style = fg(WHITE, true)
			elseif shine ~= nil and c >= shine and c < shine + 4 then
				style = fg(WHITE, true)
			else
				style = fg(band(c), c >= 36)
			end
			if style ~= last and style ~= "" then
				parts[#parts + 1] = style
				last = style
			end
			parts[#parts + 1] = ch
		end
		out[#out + 1] = join(parts)
	end
	return join(out) .. reset()
end

function splash.mascot_frame(shape)
	local out = {}
	for i, line in ipairs(shape) do
		out[#out + 1] = at(splash.mascot_x, splash.top + i - 1)
		-- Horns and tail red, face magenta, eyes bright white.
		if i == 3 then
			out[#out + 1] = fg(MAGENTA, false) .. line:gsub("[o%-]",
			    function(e) return fg(WHITE, true) .. e ..
				fg(MAGENTA, false) end)
		elseif i == 1 or i == 5 then
			out[#out + 1] = fg(RED, true) .. line
		else
			out[#out + 1] = fg(MAGENTA, false) .. line
		end
	end
	return join(out) .. reset()
end

function splash.tagline_frame(count)
	local text = splash.tagline:sub(1, count or #splash.tagline)
	return at(splash.word_x + 4, splash.top + #splash.wordmark + 1) ..
	    fg(MAGENTA, false) .. text .. reset() ..
	    (count == nil and (fg(BLUE, true) .. sound_text() .. reset()) or "")
end

-- The frames in order: {text, delay_us}. The final frame is complete.
function splash.frames(animate)
	local frames = {}
	local width = 0
	for _, line in ipairs(splash.wordmark) do
		if #line > width then
			width = #line
		end
	end
	if animate then
		-- 1. Sweep: the wordmark appears left to right behind a spark.
		local step = 6
		for reveal = step, width + step, step do
			frames[#frames + 1] = { splash.wordmark_frame(reveal), 20000 }
		end
		-- 2. The daemon appears with its eyes shut, then the shimmer.
		frames[#frames + 1] = { splash.mascot_frame(splash.mascot_shut), 40000 }
		for shine = 0, width, 10 do
			frames[#frames + 1] = { splash.wordmark_frame(nil, shine), 20000 }
		end
		-- 3. It opens its eyes, blinks once, and the tagline types out.
		frames[#frames + 1] = { splash.wordmark_frame(nil, nil) ..
		    splash.mascot_frame(splash.mascot_open), 100000 }
		frames[#frames + 1] = { splash.mascot_frame(splash.mascot_shut), 60000 }
		frames[#frames + 1] = { splash.mascot_frame(splash.mascot_open), 20000 }
		for count = 8, #splash.tagline - 1, 8 do
			frames[#frames + 1] = { splash.tagline_frame(count), 15000 }
		end
	end
	frames[#frames + 1] = { CSI .. "H" .. CSI .. "J" ..
	    splash.wordmark_frame(nil, nil) ..
	    splash.mascot_frame(splash.mascot_open) ..
	    splash.tagline_frame(nil), 150000 }
	return frames
end

-- Draw the splash. Returns the total delay spent, in microseconds.
function splash.draw(mode)
	local frames = splash.frames(mode == "yes")
	local final = frames[#frames]
	frames[#frames] = nil
	local spent = 0
	local key = false
	printc(CSI .. "H" .. CSI .. "J" .. CSI .. "?25l")
	for _, frame in ipairs(frames) do
		-- Any key ends the animation at once and stays queued for the
		-- menu, which reads it during its autoboot countdown.
		if io.ischar() then
			key = true
			break
		end
		if spent + frame[2] + final[2] > splash.budget_us then
			break
		end
		printc(frame[1])
		loader.delay(frame[2])
		spent = spent + frame[2]
	end
	-- The complete splash always lands, then holds briefly unless a key
	-- is waiting; the menu clears it when it draws.
	printc(final[1])
	if not key and not io.ischar() then
		loader.delay(final[2])
		spent = spent + final[2]
	end
	printc(reset() .. CSI .. "?25h")
	return spent
end

function splash.run()
	local ok, result = pcall(function()
		local mode = splash.mode()
		if mode == nil then
			return 0
		end
		return splash.draw(mode)
	end)
	if not ok then
		-- Never let the splash stop the boot; leave a sane terminal.
		pcall(printc, CSI .. "0m" .. CSI .. "?25h")
		return nil, result
	end
	return result
end

return splash
