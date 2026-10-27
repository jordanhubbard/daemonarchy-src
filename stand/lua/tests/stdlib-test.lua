-- The loader's Lua opens only base, package, string and its own modules
-- (stand/common/interp_lua.c); table, math, os, coroutine, utf8 and debug
-- are absent, so code that calls them fails only on a real console.
--   lua5.4 stand/lua/tests/stdlib-test.lua   (from the source root)
local missing = {"table", "math", "os", "coroutine", "utf8", "debug"}
local files = {}
local p = io.popen("ls stand/lua/*.lua")
for name in p:lines() do files[#files + 1] = name end
p:close()
assert(#files > 0, "run from the source root")
local bad = 0
for _, name in ipairs(files) do
	local n = 0
	for line in io.lines(name) do
		n = n + 1
		-- Drop string literals, then comments: only code names count.
		local code = line:gsub('"[^"]*"', '""'):gsub("'[^']*'", "''")
		code = code:gsub("%-%-.*$", "")
		for _, lib in ipairs(missing) do
			if code:find("%f[%w_]" .. lib .. "%.[%a_]") then
				print(string.format("%s:%d: uses %s.*: %s", name, n, lib, line))
				bad = bad + 1
			end
		end
	end
end
if bad > 0 then os.exit(1) end
print(string.format("PASS: %d loader Lua files use no missing standard library", #files))
