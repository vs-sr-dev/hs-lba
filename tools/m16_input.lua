-- m16_input.lua — follow one stick direction all the way to the engine.
--
-- Symptom: up no longer walks Twinsen, while turning and backing up still do.
-- That is either the pad read, the stick-to-direction synthesis, or the game
-- reacting to a direction it did receive — three different repairs, and from
-- outside they look the same.
--
-- So this prints the whole chain at every change, hands off the pad:
--
--   raw[2]   the byte off the I2C bus, 0x7F at rest. Up is *above* centre
--            (MAME declares the axes PORT_REVERSE — see src/input.c).
--   ay       0x7F - raw[2], so up is negative.
--   dpad     what src/input.c synthesised from ay against the dead zone.
--   Joy      what the engine sees: J_UP=1 J_DOWN=2 J_LEFT=4 J_RIGHT=8.
--
-- If Joy shows 1 while pressing up, the input is arriving and the fault is in
-- the game; if it stops earlier than that, the line where it stops names the
-- bug. TimerRef is here too because an engine that stops animating usually
-- stopped counting: it is the 50 Hz tick everything is paced against, and it
-- is already known to lose ticks under load.

local S = dofile(assert(os.getenv("HSLBA_SYMS"), "set HSLBA_SYMS to build/syms.lua"))

-- struct hs_pad: buttons, pressed, released, ax, ay, dpad, raw[5], ok
local PAD_AX, PAD_AY, PAD_DPAD, PAD_RAW, PAD_OK = 12, 16, 20, 24, 32

local mem, frame = nil, 0
local last, tick = nil, {}

local function i32(a)
  local v = mem:read_u32(a)
  if v >= 0x80000000 then v = v - 0x100000000 end
  return v
end

_G.m16 = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then mem = manager.machine.devices[":maincpu"].spaces["program"] end

  local pad = S["pad0"]
  local raw2 = mem:read_u8(pad + PAD_RAW + 2)
  local raw3 = mem:read_u8(pad + PAD_RAW + 3)
  local ay   = i32(pad + PAD_AY)
  local ax   = i32(pad + PAD_AX)
  local dpad = mem:read_u32(pad + PAD_DPAD)
  local joy  = mem:read_u16(S["Joy"])
  local fire = mem:read_u16(S["Fire"])

  local key = string.format("%d/%d/%d/%d/%d/%d/%d", raw2, raw3, ax, ay, dpad, joy, fire)
  if key ~= last then
    last = key
    local names = ""
    if joy & 1 ~= 0 then names = names .. "UP " end
    if joy & 2 ~= 0 then names = names .. "DOWN " end
    if joy & 4 ~= 0 then names = names .. "LEFT " end
    if joy & 8 ~= 0 then names = names .. "RIGHT " end
    print(string.format(
      "[%6.2fs] raw[2]=%02X raw[3]=%02X  ax=%4d ay=%4d  dpad=%X  ->  Joy=%X %s Fire=%X  ok=%d",
      emu.time(), raw2, raw3, ax, ay, dpad, joy, names, fire,
      mem:read_u32(pad + PAD_OK)))
  end

  -- Is the engine's clock still moving? A frozen animation with a frozen
  -- TimerRef is a different bug from a frozen animation with a live one.
  if frame % 60 == 0 then
    local t, tr = emu.time(), mem:read_u32(S["TimerRef"])
    if tick.t then
      local dt, dtr = t - tick.t, tr - tick.tr
      if dtr == 0 or dt > 0 and dtr / dt < 40 then
        print(string.format("[%6.2fs] TimerRef %d (+%d in %.2fs = %.1f Hz)  CmptFrame=%d",
          t, tr, dtr, dt, dt > 0 and dtr / dt or 0, mem:read_u16(S["CmptFrame"])))
      end
    end
    tick.t, tick.tr = t, tr
  end
end)
