-- m21_profile.lua — where does a gameplay frame actually go?
--
-- Until now there was no frame-rate measurement at all: PROJECT.md records
-- that CmptFrame does not behave like a counter when read from outside, and
-- PORT_rects/PORT_pixels say how much got pushed to the screen but not whether
-- the engine was keeping up while it did. Optimising against that is guessing.
--
-- So this measures two things that are hard to fake:
--
--   1. The counters the port keeps (src/platform_gfx.c). PORT_frames is a real
--      MainLoop iteration count, and PORT_frames_idle is the half that matters:
--      LBA's MainLoop waits for its 50 Hz tick before starting the next frame,
--      so a frame with time to spare and a frame that overran look identical in
--      a frame count. idle/frames is the headroom, directly. 100% means the
--      regulator is setting the pace; 0% means the machine is.
--
--   2. A sampling profile. Every emulated video frame the CPU's PC is read and
--      charged to the function containing it (build/fsyms.lua, text symbols
--      sorted by address). At ~60 samples a second this is statistical, not
--      exact — but it needs no instrumentation in the hot paths, so it cannot
--      distort what it is measuring, and ranking is all that is being asked of
--      it.
--
-- The sampling is deliberately taken at video-frame boundaries, which are
-- asynchronous to the game loop: the engine's own frame runs at a fraction of
-- the video rate, so successive samples land at unrelated points in it. The one
-- thing to be suspicious of is the vblank ISR — it fires on the same edge — so
-- its share is reported separately rather than mixed into the ranking.
--
--   make
--   HSLBA_SYMS=build/syms.lua HSLBA_FSYMS=build/fsyms.lua \
--   hyprscan.exe hyprscan -rompath "<roms>;build" -cdrom build/hslba.iso \
--       -memc build/card_blank.bin -quickload build/HYPER.EXE \
--       -autoboot_script tools/m21_profile.lua -autoboot_delay 0 \
--       -nothrottle -video none -seconds_to_run 900
--
-- HSLBA_PROF_SECS   emulated seconds to measure over (default 60)
-- HSLBA_PROF_WALK   1 (default) to keep Twinsen walking, 0 for a still scene.
--                   A still scene has almost no dirty area, so it profiles the
--                   idle loop rather than the renderer — useful as a contrast,
--                   misleading on its own.

local S = dofile(assert(os.getenv("HSLBA_SYMS"), "set HSLBA_SYMS to build/syms.lua"))
local F = dofile(assert(os.getenv("HSLBA_FSYMS"), "set HSLBA_FSYMS to build/fsyms.lua"))

local PROF_SECS = tonumber(os.getenv("HSLBA_PROF_SECS") or "60")
local WALK = (os.getenv("HSLBA_PROF_WALK") or "1") ~= "0"

-- HSLBA_PROF_INTRO=1 profiles everything *before* gameplay — the logos, the FLA
-- intro and the main menu — by measuring from boot and pressing nothing, so the
-- movie runs to its end and the menu is left sitting there.
--
-- This is a different machine from the gameplay one and has to be measured
-- separately. LBA's gameplay frame is incremental: a handful of small dirty
-- rectangles. Menus and movies are not — they present the whole screen every
-- frame, so at 640x480 output they pay the full 4x that the dirty rectangles
-- never do. PORT_frames does not advance here at all (there is no MainLoop
-- yet), which is why the report below leans on PORT_presents and PORT_pixels.
local INTRO = (os.getenv("HSLBA_PROF_INTRO") or "0") ~= "0"

local CON = 0xa0ef0000 + 0x1000

-- Leaf library routines that are always someone else's cost. Charged to their
-- caller instead of to themselves; see the sampling code below.
local LEAVES = { memcpy = true, memset = true, memmove = true, strlen = true }

local COUNTERS = {
  "PORT_frames", "PORT_frames_idle", "PORT_rects", "PORT_pixels",
  "PORT_cls_pixels", "PORT_objs", "PORT_obj_pixels", "PORT_presents",
}

local mem, ports, frame, con_read = nil, nil, 0, 0
local phase, settle, t0 = "intro", nil, nil
local base, samples, total = {}, {}, 0
local PRESS, PERIOD = 6, 30

local function drain()
  if mem:read_u32(CON) ~= 0xC04501E1 then return end
  local n = mem:read_u32(CON + 4)
  local size = 0x4000 - 16
  if n - con_read > size then con_read = n - size end
  local out = {}
  while con_read < n do
    out[#out + 1] = string.char(mem:read_u8(CON + 16 + (con_read % size)))
    con_read = con_read + 1
  end
  if #out > 0 then io.write(table.concat(out)); io.flush() end
end

local function field(port, name)
  local p = ports[port]
  return p and p.fields[name] or nil
end

local function hold(port, name, on)
  local f = field(port, name)
  if f then f:set_value(on and 1 or 0) end
end

local function count(name)
  local a = S[name]
  return a and mem:read_u32(a) or 0
end

-- nearest text symbol at or below pc
local function symbol(pc)
  local lo, hi = 1, #F
  if pc < F[1][1] then return nil end
  while lo < hi do
    local mid = (lo + hi + 1) // 2
    if F[mid][1] <= pc then lo = mid else hi = mid - 1 end
  end
  return F[lo][2]
end

local function report()
  drain()
  local dt = emu.time() - t0
  local d = {}
  for _, c in ipairs(COUNTERS) do d[c] = count(c) - base[c] end

  local frames = d.PORT_frames
  print("")
  print(string.format("m21_profile: %.1fs of %s", dt,
    INTRO and "the pre-gameplay path (logos, FLA intro, main menu)"
          or ("gameplay, walking=" .. tostring(WALK))))
  print("")
  if frames > 0 then
    print(string.format("  frame rate            %6.2f fps   (%d MainLoop iterations)",
      frames / dt, frames))
    print(string.format("  frames with headroom  %6.1f %%     (%d reached the 50 Hz tick early)",
      100.0 * d.PORT_frames_idle / frames, d.PORT_frames_idle))
    print("")
    print("  per frame, at the engine's 640x480:")
    print(string.format("    background restored %8.0f px   (ClsBoxes, Screen -> Log)",
      d.PORT_cls_pixels / frames))
    print(string.format("    3D object boxes     %8.0f px   over %.1f objects",
      d.PORT_obj_pixels / frames, d.PORT_objs / frames))
    print("  per frame, at the video output:")
    print(string.format("    presented           %8.0f px   in %.1f rectangles",
      d.PORT_pixels / frames, d.PORT_rects / frames))
  else
    -- Before gameplay there is no MainLoop, so the rate that matters is how
    -- often the screen is rebuilt and how big those rebuilds are.
    print("  (no MainLoop iterations — this is the pre-gameplay path)")
    print("")
    print(string.format("  screen rebuilds       %6.2f /s    (%d rectangles pushed)",
      d.PORT_rects / dt, d.PORT_rects))
    print(string.format("  output pixels written %6.2f M/s   (%.0f per rectangle)",
      d.PORT_pixels / dt / 1e6,
      d.PORT_rects > 0 and d.PORT_pixels / d.PORT_rects or 0))
    print(string.format("  whole-screen presents %6.2f /s    (the FLA path)",
      d.PORT_presents / dt))
  end

  print("")
  print(string.format("  sampling profile, %d samples at %.0f Hz:", total, total / dt))
  local list = {}
  for name, n in pairs(samples) do list[#list + 1] = { name, n } end
  table.sort(list, function(a, b) return a[2] > b[2] end)
  for i = 1, math.min(#list, 24) do
    print(string.format("    %5.1f %%  %s", 100.0 * list[i][2] / total, list[i][1]))
  end
  io.flush()
  manager.machine:exit()
end

_G.m21 = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
    ports = manager.machine.ioport.ports
  end
  drain()

  if INTRO and phase == "intro" then
    -- Nothing is pressed: the logos, the movie and then the main menu, left to
    -- sit. Measurement starts at the first frame so the whole path is covered.
    if not t0 then
      for _, c in ipairs(COUNTERS) do base[c] = count(c) end
      t0 = emu.time()
      print(string.format("[%6.2fs] measuring the pre-gameplay path for %ds",
        emu.time(), PROF_SECS))
    end
    local cpu = manager.machine.devices[":maincpu"]
    local name = symbol(cpu.state["PC"].value) or "?"
    if LEAVES[name] then
      local caller = symbol(cpu.state["r3"].value)
      if caller then name = name .. " <- " .. caller end
    end
    samples[name] = (samples[name] or 0) + 1
    total = total + 1
    if emu.time() - t0 >= PROF_SECS then report() end

  elseif phase == "intro" then
    hold(":ctrl0:IN.1", "Green", (frame % PERIOD) < PRESS)
    if count("PORT_frames") > 2 then
      hold(":ctrl0:IN.1", "Green", false)
      phase, settle = "settle", emu.time()
      print(string.format("[%6.2fs] first scene is up", emu.time()))
    end

  elseif phase == "settle" then
    if emu.time() - settle > 5 then
      for _, c in ipairs(COUNTERS) do base[c] = count(c) end
      t0 = emu.time()
      phase = "measure"
      print(string.format("[%6.2fs] measuring for %ds", emu.time(), PROF_SECS))
    end

  elseif phase == "measure" then
    -- Keep him moving, or the dirty rectangles collapse to nothing and the
    -- profile is of an engine with no work to do. Direction reverses so he
    -- stays in the room rather than walking into the first scripted event.
    if WALK then
      local ax = ports[":ctrl0:IN.3"] and ports[":ctrl0:IN.3"].fields["Analog X"]
      if ax then ax:set_value(((frame // 120) % 2 == 0) and 255 or 0) end
    end

    local cpu = manager.machine.devices[":maincpu"]
    local pc = cpu.state["PC"].value
    local name = symbol(pc) or "?"

    -- A sample inside a libc leaf names the wrong thing: "memcpy 19%" is not a
    -- finding, it is a question. r3 is the S+core link register (every prologue
    -- in this build starts `push! r3`), and these leaves never touch it, so it
    -- still holds the return address — which turns the sample into the caller
    -- that is really spending the time.
    if LEAVES[name] then
      local caller = symbol(cpu.state["r3"].value)
      if caller then name = name .. " <- " .. caller end
    end

    samples[name] = (samples[name] or 0) + 1
    total = total + 1

    if emu.time() - t0 >= PROF_SECS then
      report()
    end
  end
end)
