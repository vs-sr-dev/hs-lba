-- m10_vecwatch.lua — catch whoever erases the exception table.
--
-- The wedged machine has 0xFFFFFFFF where the vectors should be, which is why
-- every exception turns into EXCEPTION_RI on the vector itself. A write tap on
-- the table reports the PC of the store, which is the only thing that actually
-- identifies the culprit — the table sits at the image's load address, so any
-- pointer that runs off the bottom of the world lands on it.
--
-- The CPU space is masked to 0x1fffffff, so the tap goes on the masked range.

local mem, frame, hits = nil, 0, 0
local tap

_G.m10vw = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
    local cpu = manager.machine.devices[":maincpu"]
    tap = mem:install_write_tap(0x00090000, 0x000903ff, "vecwatch",
      function(offset, data, mask)
        hits = hits + 1
        if hits <= 12 then
          print(string.format("[%8.4fs] write %08X <- %08X (mask %08X) from PC=%08X",
                emu.time(), offset, data, mask, cpu.state["CURPC"].value))
        end
        return data
      end)
    print("tap installed")
  end

  if frame > 400 then
    print(string.format("writes seen: %d   vec[0x200]=%08X", hits,
          mem:read_u32(0xa0090200)))
    manager.machine:exit()
  end
end)
