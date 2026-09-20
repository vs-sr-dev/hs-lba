-- m15_obj3d.lua — why do the inventory / holomap objects not draw?
--
-- The game world is isometric. The perspective camera (TypeProj = TYPE_3D) is
-- used by almost nothing: GAMEMENU.C's Draw3dObject(), which draws the
-- inventory items and the "you found an object" model, and HOLOMAP.C. That is
-- exactly the set reported broken, so the projection state is the first thing
-- to look at.
--
-- Nothing is instrumented for this. Every value below is already a global in
-- the translated 3D pipeline, so this reads them straight out of a running
-- machine — no rebuild, and the same numbers the host bench prints.
--
-- Reference, from tools/host3d.c on INVOBJ.HQR[0] with Draw3dObject's own
-- arguments (SetProjection(x, y, 128, 200, 200), SetFollowCamera(0,0,0,60,0,0,
-- zoom), zoom=10000):
--
--     TypeProj=0  KFactor=128  LFactorX=200  LFactorY=200
--     NbPoints=92  screen box (281,203)-(359,294)   64 polys, all on screen
--
-- So: open the inventory (or trigger a "found object") and compare. If
-- KFactor/LFactorX/LFactorY are not 128/200/200 the argument passing is wrong
-- — SetProjection takes five arguments and SetFollowCamera seven, which are
-- the only calls in the game that push any onto the stack. If they are right
-- but the projected box is not, the arithmetic is: that path is the only one
-- doing a 64-bit divide.
--
-- Run with HSLBA_OBSERVE-style hands-off: this script never touches the pad.

local S = dofile(assert(os.getenv("HSLBA_SYMS"), "set HSLBA_SYMS to build/syms.lua"))

local mem, frame = nil, 0
local last = nil

local function u32(name)
  local a = S[name]
  return a and mem:read_u32(a) or -1
end

local function i16(name)
  local a = S[name]
  if not a then return -1 end
  local v = mem:read_u16(a)
  if v >= 0x8000 then v = v - 0x10000 end
  return v
end

local function point(i)
  local a = S["List_Point"]
  local function w(o)
    local v = mem:read_u16(a + o)
    if v >= 0x8000 then v = v - 0x10000 end
    return v
  end
  return w(i * 6), w(i * 6 + 2), w(i * 6 + 4)
end

_G.m15 = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
  end

  -- Only look while the perspective camera is selected: in the isometric game
  -- these globals belong to whatever actor was drawn last and say nothing.
  if u32("TypeProj") ~= 0 then return end

  local key = string.format("%d/%d/%d/%d/%d/%d/%d/%d",
    u32("KFactor"), u32("LFactorX"), u32("LFactorY"),
    u32("XCentre"), u32("YCentre"),
    i16("NbPoints"), i16("ScreenXmin"), i16("ScreenXmax"))

  if key == last then return end
  last = key

  local n = i16("NbPoints")
  local onscreen, xmin, xmax, ymin, ymax = 0, 32767, -32768, 32767, -32768
  if n > 0 and n <= 500 then
    for i = 0, n - 1 do
      local x, y = point(i)
      if x < xmin then xmin = x end
      if x > xmax then xmax = x end
      if y < ymin then ymin = y end
      if y > ymax then ymax = y end
      if x >= 0 and x < 640 and y >= 0 and y < 480 then onscreen = onscreen + 1 end
    end
  end

  print(string.format(
    "[%6.2fs] PERSPECTIVE  KFactor=%d LFactorX=%d LFactorY=%d centre=(%d,%d)  CameraZr=%d",
    emu.time(), u32("KFactor"), u32("LFactorX"), u32("LFactorY"),
    u32("XCentre"), u32("YCentre"), u32("CameraZr")))
  print(string.format(
    "           NbPoints=%d  engine box=(%d,%d)-(%d,%d)  projected x %d..%d y %d..%d  %d/%d on screen",
    n, i16("ScreenXmin"), i16("ScreenYmin"), i16("ScreenXmax"), i16("ScreenYmax"),
    xmin, xmax, ymin, ymax, onscreen, n))

  local out = "           first vertices:"
  for i = 0, math.min(n, 6) - 1 do
    local x, y, z = point(i)
    out = out .. string.format(" (%d,%d,%d)", x, y, z)
  end
  print(out)
end)
