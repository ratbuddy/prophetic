-- MAME autoboot script: trace the V55 (":maincpu") between two emulated times into a file.
-- Env: KP_TRACE_T0, KP_TRACE_T1 (seconds), KP_TRACE_OUT (file). Needs -debug -debugger none.
local t0 = tonumber(os.getenv("KP_TRACE_T0") or "11.49")
local t1 = tonumber(os.getenv("KP_TRACE_T1") or "11.60")
local out = os.getenv("KP_TRACE_OUT") or "v55.log"
local state = 0
emu.register_periodic(function()
  local t = manager.machine.time:as_double()
  if state == 0 and t >= t0 then
    manager.machine.debugger:command("trace " .. out .. ",maincpu,noloop")
    state = 1
  elseif state == 1 and t >= t1 then
    manager.machine.debugger:command("trace off,maincpu")
    state = 2
  end
end)
