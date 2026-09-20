-- m19_save.lua — did the engine's own autosave work?
--
-- src/card.c is proven on its own by m19_card.lua. This is the other half: the
-- save the ENGINE triggers, through fopen("AUTOSAVE.LBA") and src/save.c, in
-- code no test can call directly.
--
-- It fires ONCE per run, when the first scene loads. Every later one needs a
-- scene change, and a harness holding the stick in one direction cannot
-- reliably produce one — so this checks the first autosave and stops, rather
-- than pretending to watch a stream of them.
--
--   make
--   python tools/mkcard.py build/card_blank.bin
--   HSLBA_SYMS=build/syms.lua hyprscan.exe hyprscan \
--       -rompath "<roms>;build" -cdrom build/hslba.iso \
--       -memc build/card_blank.bin -quickload build/HYPER.EXE \
--       -autoboot_script tools/m19_save.lua -autoboot_delay 0 \
--       -nothrottle -video none -seconds_to_run 600
--
-- Budget about four minutes of emulated time: the intro alone is three and a
-- half, and green has to be pulsed through all of it.
--
-- What a failure used to look like. Before src/save.c there was nowhere on
-- this console to write, so OpenWrite() returned null and GAMEMENU.C put up a
-- modal "Error Writing Saved Game" that waits for a keypress. From outside
-- that is indistinguishable from a hang, which is why the verdict below is
-- save_ram_writes and not "the game is still running".

local S = dofile(assert(os.getenv("HSLBA_SYMS"), "set HSLBA_SYMS to build/syms.lua"))

local TRACE = 0xa0ef0000
local CON   = TRACE + 0x1000

local mem, ports, frame, con_read = nil, nil, 0, 0
local pressing, settle = true, nil

local PRESS, PERIOD = 6, 30

local COUNTERS = {
  {"save_ram_writes",      "autosaves written to RAM"},
  {"save_card_saves",      "saves programmed onto the card"},
  {"save_card_failures",   "card saves refused or failed"},
  {"save_pack_overflows",  "states too big for 96 bytes"},
  {"save_flag_exceptions", "game flags needing a full byte (high-water)"},
  {"card_transactions",    "card transactions"},
  {"card_errors",          "card parity/verify errors"},
  {"card_rall_fallbacks",  "whole-card reads done the slow way"},
  {"fs_open_fails",        "file opens that ran out of handles"},
}

local function count(name)
  local a = S[name]
  return a and mem:read_u32(a) or nil
end

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

local function field(port, name)
  local p = ports[port]
  return p and p.fields[name] or nil
end

local function report(verdict)
  drain()
  print("")
  print(string.format("m19_save: %s  (%.2fs emulated)", verdict, emu.time()))
  for _, c in ipairs(COUNTERS) do
    print(string.format("  %-22s %8s   %s", c[1],
                        tostring(count(c[1]) or "n/a"), c[2]))
  end
  io.flush()
  manager.machine:exit()
end

_G.m19s = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
    ports = manager.machine.ioport.ports
  end

  drain()

  -- CmptFrame (PERSO.C) advances once per MainLoop iteration; anything above
  -- a couple means the first scene is up and its autosave has been and gone.
  if pressing and mem:read_u16(S["CmptFrame"]) > 2 then
    pressing = false
    settle = emu.time()
    print(string.format("[%6.2fs] first scene is up", emu.time()))
  end

  -- Green is F_SPACE: both "skip this" and the menu's confirm, and "New game"
  -- is the entry already selected. The engine wants a release between presses.
  local green = field(":ctrl0:IN.1", "Green")
  if green then
    green:set_value((pressing and (frame % PERIOD) < PRESS) and 1 or 0)
  end

  -- A couple of seconds after the scene appears, so a save still in progress
  -- is not read as one that never happened. Nothing is asked of the pad from
  -- here on: walking would only march into the first scripted event.
  --
  -- HSLBA_LINGER buys time to watch it with the video on. The thing to look
  -- for is what is NOT there: the "Error Writing Saved Game" box that every
  -- run before this one put up as the first scene appeared.
  if settle and emu.time() - settle > tonumber(os.getenv("HSLBA_LINGER") or "3") then
    local writes = count("save_ram_writes") or 0

    if writes >= 1 then
      report("PASS - the engine autosaved to RAM")
    else
      report("FAIL - no autosave; the game is probably sitting on "
             .. "\"Error Writing Saved Game\"")
    end
  end

  if frame > tonumber(os.getenv("HSLBA_FRAMES") or "30000") then
    report("FAIL - never reached a scene")
  end
end)
