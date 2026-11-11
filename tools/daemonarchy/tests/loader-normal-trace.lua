-- SPDX-License-Identifier: BSD-2-Clause
-- Actual startup dispatcher; no EFI or persistence acceptance implied.
local root = assert(arg[1], 'source root required')
local env, commands, fail = {}, {}, false
package.preload.config = function()
    return {readConf=function() error('normal startup read an experiment preset') end}
end
package.preload.core = function()
    return {isUEFIBoot=function() return true end,
        boot=function() error('normal startup forced a boot') end}
end
loader = {
    getenv=function(name) return env[name] end,
    setenv=function() error('startup changed a tunable') end,
    unsetenv=function() error('startup removed a tunable') end,
    has_command=function() return true end,
    time=function() return 0 end,
    delay=function() error('normal startup waited for experiment controls') end,
    perform=function(command)
        commands[#commands+1] = command
        if command == 'boottrace cycle' then env.boottrace_cycle='stop' end
        return fail and 1 or 0
    end,
}
local trace = dofile(root .. '/stand/lua/boottrace.lua')
env = {['daemonarchy.trace.enable']='YES', boottrace_autocycle='YES',
    boottrace_stick='YES', boottrace_loop='YES', boottrace_conf='/experiments'}
assert(trace.startup() == nil)
assert(#commands == 1 and commands[1] == 'boottrace observe')
commands = {}; fail = true
assert(trace.startup() == nil)
assert(#commands == 1 and commands[1] == 'boottrace observe')
commands = {}; fail = false
env = {['daemonarchy.trace.enable']='NO', boottrace_wait='0'}
assert(trace.startup() == nil)
assert(#commands == 1 and commands[1] == 'boottrace startup 0')
commands = {}
env = {['daemonarchy.trace.enable']='NO', boottrace_autocycle='YES',
    boottrace_stick='YES', boottrace_autocycle_delay='0'}
assert(trace.startup() == 'prompt')
assert(#commands == 1 and commands[1] == 'boottrace cycle')
print('normal trace startup: 4 controls passed')
