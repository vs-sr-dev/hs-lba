-- m8_boot_watch.lua — watch the engine boot and report where it gets to.
--
-- The engine does not return, so "did it work" has to be read from the outside:
-- the marker word, whether the 50 Hz clock is still ticking (a wedged machine
-- stops it), and the fault slots src/irq.c fills in before halting.

local TRACE = 0xa0ef0000
local CON   = TRACE + 0x1000        -- the RAM console in src/syscalls.c
local mem, frame, last = nil, 0, nil
local shot = false
local con_read = 0

local function t(i) return mem:read_u32(TRACE + i * 4) end

-- Drain whatever the engine has printed since the last call.
local function drain()
  if mem:read_u32(CON) ~= 0xC04501E1 then return end
  local total = mem:read_u32(CON + 4)
  local size = 0x4000 - 16
  if total - con_read > size then con_read = total - size end   -- wrapped
  local out = {}
  while con_read < total do
    out[#out + 1] = string.char(mem:read_u8(CON + 16 + (con_read % size)))
    con_read = con_read + 1
  end
  if #out > 0 then io.write(table.concat(out)); io.flush() end
end

_G.m8 = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then mem = manager.machine.devices[":maincpu"].spaces["program"] end

  if t(14) ~= 0 then
    print(string.format("[%6.2fs] FAULT cause=%08X pc=%08X (marker %08X)",
          emu.time(), t(14), t(15), t(0)))
    manager.machine.video:snapshot()
    manager.machine:exit()
    return
  end

  drain()

  local m = t(0)
  if m ~= last then
    last = m
    print(string.format("[%6.2fs] marker %08X  files=%d", emu.time(), m, t(2)))
  end

  if frame % 300 == 0 then
    print(string.format("[%6.2fs] marker %08X  TimerRef=%d  seeks=%d",
          emu.time(), m, t(1), t(5)))
  end

  if frame == 1800 and not shot then
    shot = true
    manager.machine.video:snapshot()
    print("snapshot taken")
  end
  if frame > 1900 then manager.machine:exit() end
end)
