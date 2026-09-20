-- m9_dump_buf.lua — copy an engine buffer out of the machine.
--
-- HSLBA_ADDR is the address of a pointer variable (Log, Screen, ...); the dump
-- is HSLBA_LEN bytes of whatever it points at, written to HSLBA_DUMP at frame
-- HSLBA_FRAME. Comparing Screen against Log tells whether a bad picture came
-- out of Load_HQR or was damaged after it.

local PTR   = tonumber(os.getenv("HSLBA_ADDR") or "0xa00d124c")
local LEN   = tonumber(os.getenv("HSLBA_LEN") or "307200")
local OUT   = os.getenv("HSLBA_DUMP") or "buf_dump.bin"
local AT    = tonumber(os.getenv("HSLBA_FRAME") or "900")

local mem, frame = nil, 0

_G.m9b = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then mem = manager.machine.devices[":maincpu"].spaces["program"] end

  if frame == AT then
    local base = mem:read_u32(PTR)
    print(string.format("[%6.2fs] [%08X] -> %08X, %d bytes -> %s",
          emu.time(), PTR, base, LEN, OUT))
    local f = assert(io.open(OUT, "wb"))
    local chunk = {}
    for i = 0, LEN - 1 do
      chunk[#chunk + 1] = string.char(mem:read_u8(base + i))
      if #chunk == 4096 then f:write(table.concat(chunk)); chunk = {} end
    end
    if #chunk > 0 then f:write(table.concat(chunk)) end
    f:close()
    print("dump complete")
    manager.machine:exit()
  end
end)
