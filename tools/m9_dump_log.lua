-- m9_dump_log.lua — copy the engine's Log buffer out of the machine.
--
-- The screen shows the right picture with the rows displaced, and Screen_X /
-- Screen_Y read back as 640x480, so the fault is either in what the engine put
-- into Log or in how the blit walks it. Dumping Log and rendering it on the
-- host separates the two: if the dump is a clean 640x480 image, the blit is
-- wrong; if the dump is already sheared, the engine's own load path is.

local TRACE = 0xa0ef0000
local LOG   = 0xa00d124c
local OUT   = os.getenv("HSLBA_DUMP") or "log_dump.bin"
local AT    = tonumber(os.getenv("HSLBA_FRAME") or "900")

local mem, frame = nil, 0

_G.m9d = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then mem = manager.machine.devices[":maincpu"].spaces["program"] end

  if frame == AT then
    local log = mem:read_u32(LOG)
    print(string.format("[%6.2fs] dumping Log=%08X -> %s", emu.time(), log, OUT))
    local f = assert(io.open(OUT, "wb"))
    local chunk = {}
    for i = 0, 640 * 480 - 1 do
      chunk[#chunk + 1] = string.char(mem:read_u8(log + i))
      if #chunk == 4096 then f:write(table.concat(chunk)); chunk = {} end
    end
    if #chunk > 0 then f:write(table.concat(chunk)) end
    f:close()
    manager.machine.video:snapshot()
    print("dump complete")
    manager.machine:exit()
  end
end)
