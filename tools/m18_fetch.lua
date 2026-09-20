-- m18_fetch.lua — what does the disc read at the moment the game freezes?
--
-- The symptom, as described from the couch: pressing jump gives an instant,
-- clean wind-up, then a freeze of a few tenths of a second, then the jump plays
-- with its "boing". Every sound effect the game has not played before does it;
-- the second time it does not. So the freeze is a *fetch*, it happens before
-- the sound rather than during it, and it is over by the time anything is
-- audible.
--
-- Two sessions were spent assuming that fetch was SAMPLES.HQR. Preloading the
-- whole sample archive into RAM did not remove it, which either means the
-- preload did not do what it claimed or means the fetch was never samples —
-- and nothing measured could tell those apart, because every counter we had
-- was per-subsystem (cd sectors, wave plays) and the question is per-file.
--
-- So src/fs.c now charges every read to the directory entry that asked for it,
-- in the two units that price a read here:
--
--   sectors   what cd.c delivered. MAME's servo charges nothing for a seek, so
--             this is the honest half of the cost — it is bandwidth, and it is
--             the same number on real hardware.
--   ticks     50 Hz ticks spent *inside* fs_read. The tick is an interrupt, so
--             it keeps counting while the main loop blocks; this is therefore
--             frames the game did not draw. It is the freeze, in the units the
--             player sees it in.
--   opens     an HQR cache miss opens the archive twice — Size_HQR walks the
--             offset table to learn the entry size, then HQR_Get walks the
--             identical path again to read it. Two opens per miss is that
--             duplication showing up as a number instead of as a reading of
--             HQ_RESS.C.
--
-- Read the table bottom-up: the file with the ticks is the file to fix.

local S = dofile(assert(os.getenv("HSLBA_SYMS"), "set HSLBA_SYMS to build/syms.lua"))

local TRACE = 0xa0ef0000
local CON   = TRACE + 0x1000

local REPORT_EVERY = tonumber(os.getenv("HSLBA_REPORT") or "10")   -- seconds

-- No input injection. The blind walk-in the earlier harnesses used cannot reach
-- the thing being measured: the freeze happens on effects the game has not
-- played *yet*, so it takes someone deliberately doing new things — jumping,
-- changing behaviour, hitting something — to trigger one. So this script only
-- watches, and the controller stays yours.

local mem, faulted, con_read = nil, false, 0
local handles_gone = false
local last_seq, next_report = 0, REPORT_EVERY
local prev = nil          -- counters at the previous report, for per-window deltas

local function u32(a) return mem:read_u32(a) end
local function sym(n)
  local a = S[n]
  return a and mem:read_u32(a) or -1
end

-- The directory as fs.c holds it: 16 bytes of name then lba/size/sum, 28 bytes
-- a slot. Read once and cached — it is fixed from fs_mount() onwards.
local names = nil
local function dir()
  if names then return names end
  local n = sym("n_entries")
  if n <= 0 or n > 64 then return nil end
  names = {}
  for i = 0, n - 1 do
    local p, s = S["entries"] + i * 28, {}
    for k = 0, 15 do
      local c = mem:read_u8(p + k)
      if c == 0 then break end
      s[#s + 1] = string.char(c)
    end
    names[i] = table.concat(s)
  end
  return names
end

local function name_of(i)
  local d = dir()
  if not d or not d[i] then return string.format("#%d", i) end
  return d[i]
end

-- The engine's own printf, which nothing has been reading. MALLOC.C prints
-- "ERROR: MemoryNotAlloc" on a failed allocation and PERSO.C prints the three
-- pool sizes; both go past unseen without this.
local function drain()
  if u32(CON) ~= 0xC04501E1 then return end
  local total = u32(CON + 4)
  local size = 0x4000 - 16
  if total - con_read > size then con_read = total - size end
  local out = {}
  while con_read < total do
    out[#out + 1] = string.char(mem:read_u8(CON + 16 + (con_read % size)))
    con_read = con_read + 1
  end
  if #out > 0 then io.write(table.concat(out)); io.flush() end
end

-- ---- HQ_Mem ---------------------------------------------------------------
--
-- The other thing that stops the game, and the one the console said out loud
-- the first time anyone read it: "Not Enough Memory: Body.HQR in HQ_Mem".
--
-- HQM is a bump allocator over a single 400,000-byte block (PERSO.C), a figure
-- inherited from the DOS build. Everything a scene needs shares it: the grid
-- (BufMap), the block table (TabBlock), the brick masks (BufferMaskBrick), and
-- then every character body loaded on demand — and nothing is given back until
-- HQM_Free_All() at a scene change. So it fills monotonically while you stand
-- in one room, and punching someone who has not been drawn yet is exactly the
-- event that asks it for the last of it.
--
-- The engine tracks its own high-water mark in UsedHQMemory, but only under
-- DEBUG_TOOLS, which is not defined. The same number is Size_HQM_Memory minus
-- Size_HQM_Free, so it costs nothing to keep from out here. What matters is the
-- split: how much was gone *before* any body loaded (the scene's own fixed
-- cost) against how much the bodies are adding. A pool that is merely too small
-- and one that is leaking look identical from the failure message.
local hqm_peak, hqm_base, hqm_bodies = 0, nil, 0

local function hqm_watch()
  local total = sym("Size_HQM_Memory")
  if total <= 0 then return end
  local used = total - sym("Size_HQM_Free")
  local nb = sym("NbBodys")

  -- The scene's fixed cost, captured while no body has been loaded yet.
  if nb == 0 then hqm_base, hqm_bodies = used, 0
  elseif hqm_base then hqm_bodies = used - hqm_base end

  -- `//`, not `/`. Lua 5.4 divides to a float and string.format's %d refuses a
  -- float that is not integral — which throws inside the notifier, where the
  -- error goes to stderr and the callback silently stops for that frame. That
  -- is precisely the failure this harness family has been bitten by before.
  if used > hqm_peak + 8192 then
    hqm_peak = used
    print(string.format(
      "[%6.2fs] HQ_Mem %d/%d KB used (%d%%) — scene %d KB + %d bodies %d KB",
      emu.time(), used // 1024, total // 1024, used * 100 // total,
      (hqm_base or 0) // 1024, nb, hqm_bodies // 1024))
  end
end

-- Per-entry arrays in fs.c, 4 bytes a slot.
local function arr(name, i) return u32(S[name] + i * 4) end

local function snapshot()
  local n, t = sym("n_entries"), {}
  for i = 0, n - 1 do
    t[i] = { o = arr("fs_opens", i), r = arr("fs_reads", i),
             s = arr("fs_sectors", i), k = arr("fs_ticks", i) }
  end
  return t
end

-- Deltas over the last window, not totals. With a person at the controller the
-- useful question is "what did *that* cost" — a table of running totals buries
-- one jump under the boot sequence. A window with nothing in it prints one line
-- and gets out of the way.
local function report()
  local now = snapshot()
  local rows = {}
  for i, c in pairs(now) do
    local b = prev and prev[i] or { o = 0, r = 0, s = 0, k = 0 }
    local d = { i = i, o = c.o - b.o, r = c.r - b.r,
                s = c.s - b.s, k = c.k - b.k }
    if d.s > 0 or d.k > 0 then rows[#rows + 1] = d end
  end
  prev = now

  table.sort(rows, function(a, b) if a.k ~= b.k then return a.k > b.k end
                                  return a.s > b.s end)

  if #rows == 0 then
    print(string.format("[%6.2fs] disc idle          (TimerRef %d, presents %d)",
      emu.time(), sym("TimerRef"), sym("PORT_presents")))
    return
  end

  print(string.format(
    "\n[%6.2fs] disc traffic in the last %ds       (TimerRef %d, presents %d)",
    emu.time(), REPORT_EVERY, sym("TimerRef"), sym("PORT_presents")))
  print("           file          opens  reads   sectors     KB   ticks   ms lost")
  local tk, tks = 0, 0
  for _, d in ipairs(rows) do
    tk, tks = tk + d.k, tks + d.s
    print(string.format("           %-12s %6d %6d %9d %6d %7d %9d",
      name_of(d.i), d.o, d.r, d.s, d.s * 2, d.k, d.k * 20))
  end
  print(string.format(
    "           %-12s %6s %6s %9d %6d %7d %9d",
    "TOTAL", "", "", tks, tks * 2, tk, tk * 20))
end

_G.m18 = emu.add_machine_frame_notifier(function()
  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
  end

  drain()
  hqm_watch()

  -- Eight handles, and running out is total rather than gradual: from that
  -- moment nothing in the game can open a file, and the screen shows a room
  -- with pieces missing instead of an error. Announced once, loudly.
  if not handles_gone and sym("fs_open_fails") > 0 then
    handles_gone = true
    print(string.format(
      "[%6.2fs] *** OUT OF FILE HANDLES — every open fails from here on ***",
      emu.time()))
    -- Naming the holders is the whole point. Knowing the handles ran out only
    -- says a leak exists; the eight names say which code leaked, and the first
    -- time this fired it cost a separate investigation to find out. struct
    -- handle is { int used; int index; unsigned int pos; } — 12 bytes a slot.
    for h = 0, 7 do
      local p = S["handles"] + h * 12
      print(string.format("           handle %d: used=%d  %s  pos=%d",
        h, u32(p), name_of(u32(p + 4)), u32(p + 8)))
    end
  end

  -- A new record for the longest single read. Printed as it happens, because a
  -- once-per-frame poller would otherwise miss a stall that opened and closed
  -- between two of its looks — which is exactly the shape of the thing being
  -- hunted.
  local seq = sym("fs_worst_seq")
  if seq > last_seq then
    last_seq = seq
    local k = sym("fs_worst_ticks")
    print(string.format(
      "[%6.2fs] *** longest read yet: %s — %d ticks (%d ms), %d sectors (%d KB)",
      emu.time(), name_of(sym("fs_worst_index")), k, k * 20,
      sym("fs_worst_sectors"), sym("fs_worst_sectors") * 2))
  end

  -- Reported whether or not gameplay has started. Boot and the intro are a
  -- different read pattern — long sequential runs, which the prefetch already
  -- covers — but printing them is what tells a slow boot apart from a wedged
  -- machine, and the last run could not tell those two apart at all.
  if emu.time() >= next_report then
    next_report = emu.time() + REPORT_EVERY
    report()
  end

  -- A fault parks in intmsg() (src/irq.c) with the cause and PC still readable.
  local fault = u32(TRACE + 4 * 14)
  if fault ~= 0 and not faulted then
    faulted = true
    print(string.format("[%6.2fs] *** FAULT %08X (cause %d) at PC %08X ***",
      emu.time(), fault, fault & 0x1f, u32(TRACE + 4 * 15)))
  end
end)
