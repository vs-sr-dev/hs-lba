-- m22_scenes.lua — frame rate against how much is on screen.
--
-- m21_profile.lua measures one scene: Twinsen's cell, which contains exactly
-- one actor. That is the wrong end of the question. What matters is the slope —
-- what a second, fifth, tenth actor costs — and a harness cannot play far
-- enough into the game to find a crowd.
--
-- It does not have to. `NewCube` is the engine's own "change scene" request:
-- MainLoop tests it every iteration and calls ChangeCube(), which with
-- FlagChgCube == 0 drops Twinsen at that scene's own start position
-- (CubeStartX/Y/Z). Writing it from outside is the same request a trigger zone
-- makes, so any scene on the disc can be visited directly and measured.
--
-- Twinsen is deliberately left standing still. His own dirty rectangle is then
-- almost nothing and what is left is what the *rest* of the scene costs, which
-- is the number being looked for.
--
--   make
--   HSLBA_SYMS=build/syms.lua hyprscan.exe hyprscan \
--       -rompath "<roms>;build" -cdrom build/hslba.iso \
--       -memc build/card_blank.bin -quickload build/HYPER.EXE \
--       -autoboot_script tools/m22_scenes.lua -autoboot_delay 0 \
--       -nothrottle -video none -seconds_to_run 1200
--
-- HSLBA_SCENES   comma-separated scene numbers (default: a spread over the disc)
-- HSLBA_SETTLE   seconds to let a scene load and its opening script run (6)
-- HSLBA_MEASURE  seconds to measure each scene for (10)
--
-- A scene whose frame counter never moves is reported as stalled rather than
-- silently averaged in: not every scene is enterable in the game state a fresh
-- new-game leaves behind, and a scene sitting in a dialogue is not a
-- measurement of anything.

local S = dofile(assert(os.getenv("HSLBA_SYMS"), "set HSLBA_SYMS to build/syms.lua"))

local SETTLE  = tonumber(os.getenv("HSLBA_SETTLE") or "6")
local MEASURE = tonumber(os.getenv("HSLBA_MEASURE") or "10")

local SCENES = {}
for s in (os.getenv("HSLBA_SCENES") or "0,1,2,3,4,8,12,20,32,40,52,60,70,80,90,100,110"):gmatch("[^,]+") do
  SCENES[#SCENES + 1] = tonumber(s)
end

local CON = 0xa0ef0000 + 0x1000

local COUNTERS = {
  "PORT_frames", "PORT_rects", "PORT_pixels",
  "PORT_cls_pixels", "PORT_objs", "PORT_obj_pixels", "PORT_frames_idle",
}

local mem, ports, frame, con_read = nil, nil, 0, 0
local phase, mark, idx = "intro", nil, 0
local base, rows = {}, {}
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

local function hold(port, name, on)
  local p = ports[port]
  local f = p and p.fields[name]
  if f then f:set_value(on and 1 or 0) end
end

local function count(name)
  local a = S[name]
  return a and mem:read_u32(a) or 0
end

local function report()
  drain()
  print("")
  print("m22_scenes: frame rate against scene contents")
  print("")
  print("  scene   fps   objs/f   objbox px   cls px   present px   headroom")
  print("  -----------------------------------------------------------------")
  for _, r in ipairs(rows) do
    if r.stalled then
      print(string.format("  %5d   %s", r.scene, "stalled — no MainLoop iterations"))
    else
      print(string.format("  %5d %5.1f   %6.2f   %9.0f %8.0f %12.0f %8.1f %%",
        r.scene, r.fps, r.objs, r.objpx, r.clspx, r.present, r.idle))
    end
  end

  -- The slope is the answer. Fitting time-per-frame against objects per frame
  -- across scenes gives the marginal cost of one more actor, which is exactly
  -- what "does it hold up when the screen fills" is asking.
  --
  -- Scenes with nothing in them are excluded. Not every number on the disc is
  -- a playable scene reachable from a fresh new game: several load and then
  -- render no objects at all, and they sit at the regulator's 50 fps with 99%
  -- headroom. Left in, they are (0 objects, fast) points that drag the line
  -- down until the fitted slope comes out NEGATIVE — which is how the first
  -- run of this harness reported that actors make the game faster.
  local n, sx, sy, sxx, sxy = 0, 0, 0, 0, 0
  for _, r in ipairs(rows) do
    if not r.stalled and r.fps > 0 and r.objs >= 0.9 and r.idle < 50.0 then
      local x, y = r.objs, 1000.0 / r.fps
      n, sx, sy, sxx, sxy = n + 1, sx + x, sy + y, sxx + x * x, sxy + x * y
    end
  end
  if n >= 3 and (n * sxx - sx * sx) ~= 0 then
    local slope = (n * sxy - sx * sy) / (n * sxx - sx * sx)
    local base_ms = (sy - slope * sx) / n
    print("")
    print(string.format("  fit over %d scenes: %.2f ms per frame + %.2f ms per object",
      n, base_ms, slope))
    print(string.format("  => at 50 fps (20 ms) the budget is about %.0f objects",
      slope > 0 and (20.0 - base_ms) / slope or 0))
  end
  io.flush()
  manager.machine:exit()
end

local function start_scene()
  idx = idx + 1
  if idx > #SCENES then
    report()
    return
  end
  mem:write_u16(S["FlagChgCube"], 0)
  mem:write_u16(S["NewCube"], SCENES[idx])
  phase, mark = "settle", emu.time()
  print(string.format("[%6.2fs] scene %d", emu.time(), SCENES[idx]))
end

_G.m22 = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
    ports = manager.machine.ioport.ports
  end
  drain()

  if phase == "intro" then
    hold(":ctrl0:IN.1", "Green", (frame % PERIOD) < PRESS)
    if count("PORT_frames") > 2 then
      hold(":ctrl0:IN.1", "Green", false)
      phase, mark = "warm", emu.time()
      print(string.format("[%6.2fs] first scene is up", emu.time()))
    end

  elseif phase == "warm" then
    if emu.time() - mark > 5 then start_scene() end

  elseif phase == "settle" then
    if emu.time() - mark > SETTLE then
      for _, c in ipairs(COUNTERS) do base[c] = count(c) end
      phase, mark = "measure", emu.time()
    end

  elseif phase == "measure" then
    if emu.time() - mark >= MEASURE then
      local dt = emu.time() - mark
      local d = {}
      for _, c in ipairs(COUNTERS) do d[c] = count(c) - base[c] end
      local f = d.PORT_frames
      if f < 5 then
        rows[#rows + 1] = { scene = SCENES[idx], stalled = true }
      else
        rows[#rows + 1] = {
          scene   = SCENES[idx],
          fps     = f / dt,
          objs    = d.PORT_objs / f,
          objpx   = d.PORT_obj_pixels / f,
          clspx   = d.PORT_cls_pixels / f,
          present = d.PORT_pixels / f,
          idle    = 100.0 * d.PORT_frames_idle / f,
        }
      end
      start_scene()
    end
  end
end)
