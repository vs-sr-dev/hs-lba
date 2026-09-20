-- m19_card.lua — watch the RFID card driver milestone.
--
-- src/m19_card.c does the work; this drains its console output and turns the
-- verdict in TRACE[0] into an exit status, so the run can be judged without
-- reading a screenshot. It is deliberately thin: unlike the profiling
-- harnesses, there is nothing here to sample over time — the milestone runs
-- once, to a conclusion, and stops.
--
-- With a card:
--   python tools/mkcard.py build/card_blank.bin
--   make MAIN=src/m19_card.c
--   hyprscan.exe hyprscan -rompath "<roms>;build" -memc build/card_blank.bin \
--       -quickload build/HYPER.EXE -autoboot_script tools/m19_card.lua \
--       -autoboot_delay 0 -nothrottle -video none -seconds_to_run 120
--
-- Without one: drop -memc. Expect "no card", cleanly and quickly. That is not
-- an optional second run — absence is the state the reader is in almost all of
-- the time, and MAME models it faithfully (is_loaded() is checked on every
-- transition), so it is worth the thirty seconds.
--
-- Afterwards, read the card back with tools/cardinfo.py — but NOT from the file
-- given to -memc. MAME persists a memcard through battery_save(), which writes
--   <mame>/nvram/<machine>/<image basename>.nv
-- and never touches the .bin, so build/card_blank.bin looks untouched no matter
-- how many saves happened. Worse, call_load() reads the .bin and then lets
-- battery_load() overwrite it from the .nv, so re-running mkcard.py does NOT
-- give you a blank card: delete the .nv as well.

local TRACE = 0xa0ef0000        -- src/memmap.h
local CON   = TRACE + 0x1000

local mem, frame, con_read = nil, 0, 0
local done = false

local function trace(i) return mem:read_u32(TRACE + i * 4) end

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

local function finish(code, why)
  done = true
  drain()
  print("")
  print(string.format("m19: %s  (%.2fs emulated)", why, emu.time()))
  print(string.format("  calibration    %d spins/us", trace(1)))
  print(string.format("  card present   %d", trace(2)))
  print(string.format("  96-byte read   %d ms", trace(3)))
  print(string.format("  programmed     %d then %d bytes", trace(4), trace(5)))
  print(string.format("  transactions   %d", trace(6)))
  print(string.format("  driver errors  %d", trace(7)))
  print(string.format("  RALL fallbacks %d  (0 means the emulator streamed the"
                      .. " whole card)", trace(8)))
  io.flush()
  manager.machine:exit()
end

_G.m19 = emu.add_machine_frame_notifier(function()
  if done then return end
  frame = frame + 1

  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
  end

  drain()

  local marker = trace(0)

  if marker == 0xaa000000 then
    finish(0, "PASS — wrote and verified the card")
  elseif marker == 0xaa000019 then
    finish(0, "no card on the reader; the absent path returned cleanly")
  elseif marker == 0xaa0000f9 then
    finish(1, "FAIL — see the log above")
  elseif mem:read_u32(TRACE + 14 * 4) ~= 0 then
    -- src/irq.c parks a fault here; without this the run just sits at MARK(1).
    print(string.format("m19: FAULT cause=%08x pc=%08x",
                        mem:read_u32(TRACE + 14 * 4),
                        mem:read_u32(TRACE + 15 * 4)))
    finish(1, "FAIL — exception")
  end

  -- The whole milestone is a few seconds of emulated time even with the
  -- 5 ms-per-byte programming delay; ten times that means it is wedged, most
  -- likely spinning in rx_sync() because nothing ever drove the line.
  if frame > tonumber(os.getenv("HSLBA_FRAMES") or "3000") then
    finish(1, "FAIL — timed out with no verdict")
  end
end)
