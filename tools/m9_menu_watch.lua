-- m9_menu_watch.lua — boot to the main menu and photograph it.
--
-- m8 answered "does it get there"; this one answers "what does it look like".
-- The engine spends its first half-minute pulling HQRs off the disc, then
-- fades the menu backdrop up, so a single snapshot at a fixed frame is a
-- coin toss — take a series and pick the one that landed after the fade.
--
-- Also dumps the first palette entries. The menu backdrop is drawn with
-- RESS.HQR's own palette (RESS_PAL, loaded into PtrPal), so entry 0 being
-- black and the rest being spread rather than clustered is the cheap check
-- that the palette arrived intact.

local TRACE = 0xa0ef0000
local CON   = TRACE + 0x1000
local mem, frame, last = nil, 0, nil
local con_read = 0
local shots = { 900, 1350, 1800, 2100, 2400 }   -- ~30/45/60/70/80 s emulated
local next_shot = 1

local function t(i) return mem:read_u32(TRACE + i * 4) end

local function drain()
  if mem:read_u32(CON) ~= 0xC04501E1 then return end
  local total = mem:read_u32(CON + 4)
  local size = 0x4000 - 16
  if total - con_read > size then con_read = total - size end
  local out = {}
  while con_read < total do
    out[#out + 1] = string.char(mem:read_u8(CON + 16 + (con_read % size)))
    con_read = con_read + 1
  end
  if #out > 0 then io.write(table.concat(out)); io.flush() end
end

_G.m9 = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then mem = manager.machine.devices[":maincpu"].spaces["program"] end

  if t(14) ~= 0 then
    print(string.format("[%6.2fs] FAULT cause=%08X pc=%08X", emu.time(), t(14), t(15)))
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

  if next_shot <= #shots and frame == shots[next_shot] then
    manager.machine.video:snapshot()
    print(string.format("[%6.2fs] snapshot %d  TimerRef=%d seeks=%d",
          emu.time(), next_shot, t(1), t(5)))
    next_shot = next_shot + 1
  end

  if frame > shots[#shots] + 30 then manager.machine:exit() end
end)
