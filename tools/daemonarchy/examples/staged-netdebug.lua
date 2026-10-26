-- SPDX-License-Identifier: BSD-2-Clause
-- Example script for `loader-console.py --tftp-root DIR --lua net0:/staged-netdebug.lua`.
-- The loader's `include` runs it with its own Lua interpreter (luaL_dofile),
-- so it may use only the standard loader Lua API.
local function require_env(name)
	local value = loader.getenv(name)
	if value == nil then
		error("staged script needs " .. name)
	end
	return value
end

local boot_id = require_env("netdebug.boot_id")
loader.setenv("staged.netdebug", "ran")
loader.setenv("boot_verbose", "YES")
print("netdebug-stage: staged-lua boot_id=" .. boot_id)
if loader.getenv("staged.module") ~= nil then
	local result = loader.command("load", loader.getenv("staged.module"))
	print("staged module load result " .. tostring(result))
end
