-- m10_vecdump.lua — is the exception table still the exception table?
--
-- The wedge is EXCEPTION_RI (reserved instruction, cause 9) with EPC pointing
-- at general_vec itself, which means the CPU cannot execute the vector. Reads
-- both the uncached alias the code uses and the masked address MAME's own
-- quickload writes through, plus a known-good code address, so "the table was
-- erased" is distinguishable from "the Lua read is looking in the wrong place".

local mem, frame = nil, 0

local WATCH = {
  { "vec 0xa00901fc", 0xa00901fc },
  { "vec 0xa0090200", 0xa0090200 },
  { "vec 0xa0090300", 0xa0090300 },
  { "vec 0x000901fc", 0x000901fc },
  { "vec 0x00090200", 0x00090200 },
  { "entry a0091000", 0xa0091000 },
}

local function report(tag)
  local out = {}
  for _, w in ipairs(WATCH) do
    out[#out + 1] = string.format("%s=%08X", w[1], mem:read_u32(w[2]))
  end
  print(string.format("[%8.4fs] %-6s %s", emu.time(), tag, table.concat(out, "  ")))
end

_G.m10v = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then mem = manager.machine.devices[":maincpu"].spaces["program"] end

  if frame == 1 or frame == 2 or frame == 30 or frame == 120 or frame == 400 then
    report("f" .. frame)
  end
  if frame > 400 then manager.machine:exit() end
end)
