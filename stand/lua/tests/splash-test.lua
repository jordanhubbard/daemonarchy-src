-- Exercise stand/lua/splash.lua against stub loader modules:
--   lua5.4 stand/lua/tests/splash-test.lua   (from the source root)
-- The real loader on a real console is native evidence; this checks the
-- time budget, key skipping, fallbacks and failure containment.
package.path = "stand/lua/?.lua;" .. package.path

local function scenario(env, opts)
	opts = opts or {}
	local out, delayed, keys_read = {}, 0, 0
	local key_after = opts.key_after -- delay count after which a key waits
	local delays = 0
	loader = {
		getenv = function(name) return env[name] end,
		delay = function(us) delayed = delayed + us; delays = delays + 1 end,
	}
	printc = function(text)
		if opts.printc_error then error("console gone") end
		out[#out + 1] = text
	end
	io.ischar = function() return key_after ~= nil and delays >= key_after end
	io.getchar = function() keys_read = keys_read + 1; return 32 end
	package.loaded.splash = nil
	package.loaded.core = {
		isMenuSkipped = function()
			return (env.beastie_disable or ""):lower() == "yes"
		end,
		isSerialConsole = function()
			local c = env.console or ""
			return c:find("comconsole") ~= nil or c:find("userboot") ~= nil
		end,
	}
	local enabled = (env.loader_color or "yes"):lower() ~= "no"
	package.loaded.color = { disabled = not enabled,
	    isEnabled = function() return enabled end }
	local splash = require("splash")
	local spent, err = splash.run()
	return table.concat(out), delayed, spent, err, keys_read
end

local base = { loader_brand = "daemonarchy", console = "efi" }
local function with(extra)
	local env = {}
	for k, v in pairs(base) do env[k] = v end
	for k, v in pairs(extra) do env[k] = v end
	return env
end

-- Animated: bounded, complete final frame, cursor restored.
local text, delayed, spent = scenario(with({}))
assert(delayed > 300000 and delayed <= 750000, "budget " .. delayed)
assert(spent == delayed)
assert(text:find("summoning the daemons...", 1, true), "tagline")
local plain = text:gsub("\027%[[%d;?]*[A-Za-z]", "")
assert(plain:find("(o) (o)", 1, true) and plain:find("(-) (-)", 1, true), "blink")
assert(text:sub(-12):find("\027%[%?25h"), "cursor restored")
assert(not text:find("\027%[38;2"), "never 24-bit color")
assert(not text:find("\a", 1, true), "never a bell")
print(string.format("animated: %d us of delays, %d bytes", delayed, #text))

-- A key ends the animation at once and is left for the menu.
local _, d2, _, _, read = scenario(with({}), { key_after = 3 })
assert(d2 <= 3 * 20000 and read == 0, "key skip " .. d2)
-- A key already waiting: final frame only, no delay.
local t3, d3 = scenario(with({}), { key_after = 0 })
assert(d3 == 0 and t3:find("summoning", 1, true))

-- Static (reduced motion): only the final frame and one short hold.
local t4, d4 = scenario(with({ daemonarchy_splash = "static" }))
assert(d4 == 150000 and not t4:gsub("\027%[[%d;?]*[A-Za-z]", ""):find("(-) (-)", 1, true), "static " .. d4)

-- Sound setting shown only for known values.
assert(scenario(with({ ["daemonarchy.sound"] = "quiet" })):find("sound: quiet", 1, true))
assert(not scenario(with({ ["daemonarchy.sound"] = "LOUD!" })):find("sound:", 1, true))

-- Nothing at all on the fallbacks.
for _, extra in ipairs({
	{ daemonarchy_splash = "NO" }, { daemonarchy_splash = "off" },
	{ loader_brand = "freebsd" }, { loader_color = "NO" },
	{ console = "comconsole" }, { console = "efi,comconsole" },
	{ console = "nullconsole" }, { console = "userboot" },
	{ beastie_disable = "YES" }, { autoboot_delay = "-1" },
	{ ["screen.textcols"] = "60" },
}) do
	local t, d = scenario(with(extra))
	assert(t == "" and d == 0, "fallback drew something")
end
local brandless = { console = "efi" }
assert(scenario(brandless) == "")

-- A failing console never escapes as an error.
local _, _, s5, err = scenario(with({}), { printc_error = true })
assert(s5 == nil and err ~= nil)
print("PASS: splash bounded, key-skippable, static mode, fallbacks silent, errors contained")
