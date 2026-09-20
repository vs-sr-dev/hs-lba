-- m6_timer_test.lua — verify the interrupt-driven platform layer under MAME.
--
-- Three things have to be true and none of them are visible on screen:
--   * TimerRef advances at exactly 50 Hz of *emulated* time (the engine's
--     clock; anything else silently changes game speed)
--   * the vblank interrupt fires once per displayed frame and the flip loop
--     keeps up with it
--   * no interrupt lands on a vector with no handler, and no exception reaches
--     the fault vector
--
--   hyprscan.exe hyprscan -rompath "<roms>;build" -quickload build/HYPER.EXE \
--       -autoboot_script tools/m6_timer_test.lua -autoboot_delay 0 \
--       -nothrottle -video none -seconds_to_run 30

local mem, ports
local frame = 0
local t0, s0
local SETTLE = 60      -- frames to let everything start before sampling
local WINDOW = 900     -- frames to measure over (30 s: enough that
                       -- +/-1 tick of counter quantisation is under 0.05%)

local function trace(i) return mem:read_u32(0xa0a00000 + i * 4) end

local function sample()
  return {
    t       = emu.time(),
    frames  = trace(1),
    timeref = trace(2),
    ticks   = trace(3),
    fps     = trace(4),
    vblank  = trace(5),
    irq     = trace(6),
    spur    = trace(7),
    buttons = trace(8),
    to      = trace(9),
    fault   = trace(14),
    faultpc = trace(15),
  }
end

_G.m6_sub = emu.add_machine_frame_notifier(function()
  frame = frame + 1

  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
    ports = manager.machine.ioport.ports
  end

  local marker = trace(0)

  if trace(14) ~= 0 then
    print(string.format("!! FAULT: cause %08X at PC %08X", trace(14), trace(15)))
    manager.machine:exit()
    return
  end

  if marker ~= 0xaa000004 then
    if frame > 600 then
      print(string.format("!! never reached the frame loop; marker=%08X", marker))
      manager.machine:exit()
    end
    return
  end

  -- Hold a direction for the second half of the window, to prove input still
  -- works now that the loop is interrupt-paced.
  if frame > SETTLE + WINDOW / 2 then
    ports[":ctrl0:IN.3"].fields["Analog X"]:set_value(255)
    ports[":ctrl0:IN.1"].fields["Green"]:set_value(1)
  end

  if frame == SETTLE then
    s0 = sample()
    t0 = s0.t
  elseif frame == SETTLE + WINDOW then
    local s = sample()
    local dt = s.t - t0

    local tick_hz   = (s.timeref - s0.timeref) / dt
    local isr_hz    = (s.ticks   - s0.ticks)   / dt
    local vblank_hz = (s.vblank  - s0.vblank)  / dt
    local frame_hz  = (s.frames  - s0.frames)  / dt

    print("")
    print(string.format("measured over %.3f s of emulated time", dt))
    print(string.format("  TimerRef      %8.3f Hz   (want 50.000)", tick_hz))
    print(string.format("  timer ISR     %8.3f Hz", isr_hz))
    print(string.format("  vblank IRQ    %8.3f Hz   (NTSC field rate)", vblank_hz))
    print(string.format("  frames drawn  %8.3f Hz", frame_hz))
    print(string.format("  NbFramePerSecond reported by the engine: %d", s.fps))
    print("")
    print(string.format("  interrupts taken %d, spurious %d, I2C timeouts %d",
          s.irq - s0.irq, s.spur, s.to))
    print(string.format("  pad buttons while held: %04X", s.buttons))

    local ok = true
    local function check(name, cond)
      if not cond then ok = false; print("  FAIL: " .. name) end
    end
    check("TimerRef within 0.5% of 50 Hz", math.abs(tick_hz - 50) < 0.25)
    -- 29.97 and not 59.94: the TVE runs interlaced, so a vblank is a whole
    -- NTSC frame of two fields. Progressive would double this, but at QVGA
    -- MAME's non-interlaced path reads only even framebuffer rows and doubles
    -- them, which would cost half the vertical resolution.
    check("vblank at the NTSC frame rate", vblank_hz > 29 and vblank_hz < 31)
    check("a frame per vblank", math.abs(frame_hz - vblank_hz) < 1.0)
    check("no spurious interrupts", s.spur == 0)
    check("no I2C timeouts", s.to == 0)
    check("green reads back while held", (s.buttons & 0x8000) ~= 0)
    print(ok and "\nALL CHECKS PASSED" or "\nCHECKS FAILED")

    manager.machine:exit()
  end
end)
