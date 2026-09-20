-- m20_behaviour.lua — do the shoulders cycle the behaviour, and does the menu
-- stay away?
--
-- Two claims, and only the first is the obvious one:
--
--   1. L1/R1 step Comportement through C_NORMAL..C_DISCRET and wrap, one
--      behaviour per press, in both directions.
--   2. MenuComportement() never runs. That is the whole reason the shoulders
--      post to a mailbox instead of synthesising a held F_CTRL, and it is
--      invisible in a screenshot taken a frame later — the menu would flash
--      and be gone.
--
-- Claim 2 is checked without looking at pixels: MenuComportement() sets
-- `Island = 255` for as long as it is up (GAMEMENU.C:621, restored at the end),
-- so a single sample of Island at 255 is the menu, and nothing else is. F_CTRL
-- in `Fire` is watched too, since that is the input that opens the menu.
--
-- Red *is* bound to F_CTRL, on purpose — that menu doubles as the game's only
-- status screen. This harness simply never presses it, so both counts staying
-- at zero says the shoulders do not go through the menu. It does not say the
-- menu is unreachable, and it is not meant to.
--
--   make
--   HSLBA_SYMS=build/syms.lua hyprscan.exe hyprscan \
--       -rompath "<roms>;build" -cdrom build/hslba.iso \
--       -quickload build/HYPER.EXE \
--       -autoboot_script tools/m20_behaviour.lua -autoboot_delay 0 \
--       -nothrottle -video none -seconds_to_run 900
--
-- Budget the same four minutes m19_save.lua does: nothing can be pressed until
-- the intro is over and the first scene is playable.

local S = dofile(assert(os.getenv("HSLBA_SYMS"), "set HSLBA_SYMS to build/syms.lua"))

local TRACE = 0xa0ef0000
local CON   = TRACE + 0x1000

local C_NAME = { [0] = "NORMAL", "SPORTIF", "AGRESSIF", "DISCRET", "PROTOPACK" }

local mem, ports, frame, con_read = nil, nil, 0, 0

-- phases: "intro" -> "settle" -> "right" -> "left" -> done
local phase, settle = "intro", nil
local step, pressed_at, seen = 0, nil, nil
local fails, menu_seen, ctrl_seen = {}, 0, 0

local PRESS, PERIOD = 6, 30

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

local function hold(port, name, on)
  local f = field(port, name)
  if f then f:set_value(on and 1 or 0) end
end

local function comport() return mem:read_u16(S["Comportement"]) end

local function fail(msg)
  fails[#fails + 1] = msg
  print("  FAIL  " .. msg)
end

local function report()
  drain()
  print("")
  print(string.format("m20_behaviour: %s  (%.2fs emulated)",
                      #fails == 0 and "PASS" or (#fails .. " FAILED"), emu.time()))
  print(string.format("  behaviour menu frames (Island==255)  %d", menu_seen))
  print(string.format("  frames with F_CTRL in Fire           %d", ctrl_seen))
  io.flush()
  manager.machine:exit()
end

-- One press per call, then wait for the engine to act on it before checking.
-- The mailbox is posted on the pad's rising edge, so the button has to be let
-- go between steps or nothing further happens — which is itself the property
-- that stops a held shoulder from spinning through the ring at 50 Hz.
local function cycle(button, dir, count, next_phase)
  local down = (frame % PERIOD) < PRESS
  hold(":ctrl0:IN.0", button, down)

  if down then
    if not pressed_at then
      pressed_at = comport()
    end
  elseif pressed_at and (frame % PERIOD) == (PERIOD - 1) then
    local now = comport()
    local want = (pressed_at + dir) % 4
    if now ~= want then
      fail(string.format("%s press %d: %s -> %s, expected %s",
        button, step + 1, C_NAME[pressed_at] or pressed_at,
        C_NAME[now] or now, C_NAME[want] or want))
    else
      print(string.format("[%6.2fs] %s: %-8s -> %s", emu.time(), button,
        C_NAME[pressed_at], C_NAME[now]))
    end
    pressed_at = nil
    step = step + 1
    if step >= count then
      step = 0
      phase = next_phase
    end
  end
end

_G.m20 = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
    ports = manager.machine.ioport.ports
  end

  drain()

  -- watched every frame of the run, not just during the presses
  if mem:read_u16(S["Island"]) == 255 then menu_seen = menu_seen + 1 end
  if (mem:read_u16(S["Fire"]) & 4) ~= 0 then ctrl_seen = ctrl_seen + 1 end

  if phase == "intro" then
    -- Green is F_SPACE: skips the intro and confirms "New game", which is the
    -- entry already selected. The engine wants a release between presses.
    hold(":ctrl0:IN.1", "Green", (frame % PERIOD) < PRESS)
    if mem:read_u16(S["CmptFrame"]) > 2 then
      hold(":ctrl0:IN.1", "Green", false)
      phase, settle = "settle", emu.time()
      print(string.format("[%6.2fs] first scene is up", emu.time()))
    end

  elseif phase == "settle" then
    -- let the scene's own opening script finish; a behaviour change is refused
    -- unless Twinsen is in MOVE_MANUAL, and that is not true the instant the
    -- scene appears
    if emu.time() - settle > 5 then
      seen = comport()
      print(string.format("[%6.2fs] starting from %s", emu.time(), C_NAME[seen] or seen))
      if seen ~= 0 then
        print("         (not NORMAL — the ring is still checked relative to it)")
      end
      phase = "right"
    end

  elseif phase == "right" then
    -- four presses: the fourth has to wrap back to where it started
    cycle("Right Shoulder", 1, 4, "left")

  elseif phase == "left" then
    cycle("Left Shoulder", -1, 4, "done")

  elseif phase == "done" then
    if comport() ~= seen then
      fail(string.format("ended on %s, started on %s",
        C_NAME[comport()] or comport(), C_NAME[seen] or seen))
    end
    if menu_seen > 0 then
      fail(string.format("the behaviour menu was on screen for %d frames", menu_seen))
    end
    if ctrl_seen > 0 then
      fail(string.format("F_CTRL was set for %d frames", ctrl_seen))
    end
    report()
  end
end)
