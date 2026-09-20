-- m9_gfx_probe.lua — read the graphics state out of the running machine.
--
-- The picture on screen is a 2x horizontal magnification with the rows sheared,
-- which is what present_rect() produces when it believes the source is 320x200
-- instead of 640x480. Rather than reason about which call path could have set
-- that, read Screen_X/Screen_Y and the mode flag directly, alongside the
-- palette, so the next step follows from a number instead of a hypothesis.
--
-- Addresses come from score-elf-nm on build/hyperscan.elf and must be refreshed
-- whenever the layout moves.

local TRACE   = 0xa0ef0000
local CON     = TRACE + 0x1000
local SCREENX = 0xa00d0fe0
local SCREENY = 0xa00d0fe2
local MCGA    = 0xa00d1138
local PALDIRTY= 0xa00d1134
local PHYS    = 0xa00d1240
local LOG     = 0xa00d124c
local PALRGB  = 0xa00f27cc
local PAL565  = 0xa00f2acc

local mem, frame, con_read = nil, 0, 0
local probes = { 600, 900, 1200, 1500, 1800, 2100 }
local next_probe = 1

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

local function probe(n)
  local sx, sy = mem:read_u16(SCREENX), mem:read_u16(SCREENY)
  print(string.format("[%6.2fs] probe %d  Screen=%dx%d  Mcga=%d PalDirty=%d  Log=%08X Phys=%08X",
        emu.time(), n, sx, sy, mem:read_u32(MCGA), mem:read_u32(PALDIRTY),
        mem:read_u32(LOG), mem:read_u32(PHYS)))

  local rgb, p565 = {}, {}
  for i = 0, 7 do
    rgb[#rgb + 1] = string.format("%02X%02X%02X",
      mem:read_u8(PALRGB + i * 3), mem:read_u8(PALRGB + i * 3 + 1), mem:read_u8(PALRGB + i * 3 + 2))
    p565[#p565 + 1] = string.format("%04X", mem:read_u16(PAL565 + i * 2))
  end
  print("            PalRGB " .. table.concat(rgb, " "))
  print("            Pal565 " .. table.concat(p565, " "))

  -- First bytes of Log: the image indices the blit is reading.
  local log = mem:read_u32(LOG)
  if log ~= 0 then
    local b = {}
    for i = 0, 15 do b[#b + 1] = string.format("%02X", mem:read_u8(log + i)) end
    print("            Log[0..15] " .. table.concat(b, " "))
  end
end

_G.m9g = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then mem = manager.machine.devices[":maincpu"].spaces["program"] end

  if mem:read_u32(TRACE + 14 * 4) ~= 0 then
    print(string.format("[%6.2fs] FAULT cause=%08X pc=%08X", emu.time(),
          mem:read_u32(TRACE + 56), mem:read_u32(TRACE + 60)))
    manager.machine:exit()
    return
  end

  drain()

  if next_probe <= #probes and frame == probes[next_probe] then
    probe(next_probe)
    manager.machine.video:snapshot()
    next_probe = next_probe + 1
  end

  if frame > probes[#probes] + 30 then manager.machine:exit() end
end)
