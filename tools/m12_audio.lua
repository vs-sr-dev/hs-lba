-- m12_audio.lua — walk into the game and watch the sound driver work.
--
-- Same entry as m10: green is pulsed until MainLoop turns over (green is
-- F_SPACE, which is both "skip this pause" and the menu's confirm, and "New
-- game" is already selected), then the stick is held so the scene keeps
-- moving and the ambience timer keeps firing samples.
--
-- What it reports is the pair that actually says whether audio is alive:
--
--   fills   — halves the mixer has produced. This is the interrupt working.
--             At 22 kHz stereo with an 8 KB buffer a half is 1024 frames,
--             so this must climb at ~21.5 Hz of *emulated* time. Slower means
--             interrupts are being lost, not that the game is slow.
--   skips   — halves left stale because cd_streaming was set. A load ticking
--             this is expected; gameplay ticking it is not.
--   plays   — WavePlay calls the driver accepted, with the last one's handle,
--             pitched rate and length, so a silent run can be told apart from
--             a run where nothing was ever asked for.
--   drops   — WavePlay refused for want of a free voice.
--
-- The actual waveform is not checked here: MAME is started with -wavwrite and
-- the recording is measured afterwards. A counter can only prove the machinery
-- turns over, not that it turns over on anything audible.

local S = dofile(assert(os.getenv("HSLBA_SYMS"), "set HSLBA_SYMS to build/syms.lua"))

local TRACE = 0xa0ef0000
local CON   = TRACE + 0x1000

local RUN_FOR = tonumber(os.getenv("HSLBA_SECONDS") or "60")

local mem, ports, frame, con_read = nil, nil, 0, 0
local pressing = true
local last = {}

local PRESS, PERIOD = 6, 30
local WALK_Y = 0xe0   -- above centre is "up"; see PORT_REVERSE in src/input.c

local cf_hi, cf_prev = 0, nil
local function frames()
  local v = mem:read_u16(S["CmptFrame"])
  if cf_prev and v < cf_prev then cf_hi = cf_hi + 0x10000 end
  cf_prev = v
  return cf_hi + v
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

local function u32(name)
  local a = S[name]
  return a and mem:read_u32(a) or -1
end

_G.m12 = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
    ports = manager.machine.ioport.ports
  end

  local gf = frames()

  if pressing and gf > 2 then
    pressing = false
    print(string.format("[%6.2fs] MainLoop running (CmptFrame=%d) — walking", emu.time(), gf))
    print(string.format("[%6.2fs] Wave_Driver_Enable=%d SamplesEnable=%d out_rate=%d Hz",
      emu.time(), u32("Wave_Driver_Enable"), u32("SamplesEnable"), u32("audio_out_rate")))
  end

  local green = field(":ctrl0:IN.1", "Green")
  if green then
    green:set_value((pressing and (frame % PERIOD) < PRESS) and 1 or 0)
  end
  local ay = field(":ctrl0:IN.2", "Analog Y")
  if ay then
    ay:set_value(pressing and 0x7f or WALK_Y)
  end

  drain()

  if frame % 120 == 0 then
    local t  = emu.time()
    local f  = u32("audio_fills")
    local sk = u32("audio_skips")
    local pl = u32("wave_plays")
    local dr = u32("audio_drops")
    local pr = u32("PORT_presents")
    local dt = t - (last.t or 0)
    local df = f - (last.f or f)
    local dp = pr - (last.pr or pr)

    -- The movie's real frame rate. Its pacing loop waits whole 50 Hz ticks
    -- (50/ImageCadence = 4 at cadence 12, so 12.5/s), which means a few per
    -- cent of extra CPU does not slow it down — it drops it to the next whole
    -- tick, 10/s. That step is the judder, and this is where it shows.
    local sec = u32("cd_sector_count")
    local ds = sec - (last.sec or sec)
    print(string.format("[%6.2fs] presents=%d (%.1f/s)  cd: %d sectors (%.0f KB/s) %d seeks %d overruns",
      t, pr, dt > 0 and dp / dt or 0, sec,
      dt > 0 and ds * 2 / dt or 0, u32("cd_seek_count"), u32("cd_overrun_count")))
    last.pr, last.sec = pr, sec

    print(string.format(
      "[%6.2fs] fills=%d (%.1f/s) cd-busy=%d cdlost=%d plays=%d drops=%d  last: h=%04X %dHz %d smp vol=%d/%d",
      t, f, dt > 0 and df / dt or 0, sk, u32("cd_overrun_count"), pl, dr,
      u32("wave_last_handle"), u32("wave_last_freq"), u32("wave_last_len"),
      (u32("wave_last_vol") >> 16) & 0xffff, u32("wave_last_vol") & 0xffff))

    last.t, last.f = t, f

    if t >= RUN_FOR then
      print(string.format("[%6.2fs] done", t))
      manager.machine:exit()
    end
  end
end)
