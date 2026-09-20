-- m13_cd.lua — watch the CD prefetch, and price the read pattern for real
-- hardware.
--
-- Walk-in is m12's: green is pulsed until MainLoop turns over, then the stick
-- is held so the scene keeps moving. What is new is what gets printed.
--
-- MAME is exactly the wrong instrument for CD timing — its servo delivers at
-- 75*speed sectors/s with `m_cur_sector = LEADIN + m_seek_lba;  // TODO: seek
-- time`, so a seek is free and a load is pure bandwidth. The counters below
-- exist so the emulator can still answer the question it *is* good for: how
-- many head movements does this read pattern ask for, and did the servo manage
-- to stay ahead of the decoder. Those two numbers price the load on hardware;
-- the wall clock inside MAME does not.
--
--   sectors   handed to a caller
--   stalls    of those, how many the caller had to wait for. This is the
--             prefetch working or not: a load the servo stayed ahead of
--             reports zero, and one where it never got ahead reports one per
--             sector. Anything in between is the mech catching up.
--   seeks     head movements. A restart of a parked stream is NOT one of these
--             — that is `resumes`, and it costs a rotation, not 100-200 ms.
--   parks     times the vector-60 handler stopped the servo at the high-water
--             mark. Nonzero is the proof the interrupt is doing its job: it
--             means the servo ran ahead unattended and was bounded by
--             something other than the reader coming back.
--   overruns  sectors thrown away because the ring lapped or because the servo
--             ran while interrupts were masked. Must stay zero.
--   irq60     servo interrupts taken. A *lower bound* on sectors delivered,
--             not a count of them: MAME's CPU core ORs a pending bit, so two
--             sectors landing before the handler runs raise one interrupt.
--             Read it as liveness — if it stops climbing while sectors do, the
--             handler is being lost and `parks` is about to mean nothing.
--   fetched   sectors the mech pulled off the disc, counted from the write
--             pointer, so immune to that coalescing. Minus `sectors` it is the
--             speculative read-ahead.

local S = dofile(assert(os.getenv("HSLBA_SYMS"), "set HSLBA_SYMS to build/syms.lua"))

local TRACE = 0xa0ef0000
local CON   = TRACE + 0x1000

local RUN_FOR = tonumber(os.getenv("HSLBA_SECONDS") or "90")

-- What we are pricing against. 4x is what src/cd.c programs; the seek figure is
-- the 100-200 ms a bare mech of that era costs, taken at its optimistic end so
-- the estimate is a floor rather than a scare.
local SECTORS_PER_S = 300
local SEEK_S        = 0.15

local mem, ports, frame, con_read = nil, nil, 0, 0
local pressing = true
local last = {}

local PRESS, PERIOD = 6, 30
local WALK_Y = 0xe0

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

_G.m13 = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
    ports = manager.machine.ioport.ports
  end

  if pressing and frames() > 2 then
    pressing = false
    print(string.format("[%6.2fs] MainLoop running — walking", emu.time()))
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
    local t   = emu.time()
    local sec = u32("cd_sector_count")
    local st  = u32("cd_stall_count")
    local sk  = u32("cd_seek_count")
    local rs  = u32("cd_resume_count")
    local pk  = u32("cd_park_count")
    local ov  = u32("cd_overrun_count")
    local ir  = u32("cd_servo_irqs")
    local pr  = u32("PORT_presents")

    local dt  = t - (last.t or 0)
    local ds  = sec - (last.sec or sec)
    local dst = st - (last.st or st)
    local dp  = pr - (last.pr or pr)

    -- What the read pattern would have cost the *game* on a real mech.
    --
    -- Not every sector the servo pulled: a sector fetched while the CPU was
    -- decoding cost the game nothing, because the head was going to be idle
    -- and a seek can interrupt a read-ahead at any moment. What the game pays
    -- for is the sectors it had to sit and wait for — `stalls`, at the drive's
    -- delivery rate — plus every head movement. That is the critical path, and
    -- it is exactly the quantity the prefetch sets out to shrink.
    --
    -- `spec` is the other half of the picture: sectors the mech pulled that
    -- nobody asked for. Free in wall-clock, not free in wear or in power, and
    -- the number to watch if the high-water mark is ever raised.
    local fe   = u32("cd_fetched_total")
    local hw   = st / SECTORS_PER_S + sk * SEEK_S
    local spec = fe - sec

    print(string.format(
      "[%6.2fs] sectors=%d (%.0f KB/s)  stalls=%d (+%d, %.0f%%)  seeks=%d  resumes=%d  parks=%d  over=%d  irq60=%d  bank=%d",
      t, sec, dt > 0 and ds * 2 / dt or 0, st, dst,
      ds > 0 and 100 * dst / ds or 0, sk, rs, pk, ov, ir,
      (u32("cd_produced") - u32("cd_consumed")) // 2352))
    print(string.format(
      "[%6.2fs] presents=%d (%.1f/s)  fills=%d  waited-for: %.1f s of disc (%.1f s stalled + %d seeks)  fetched=%d (%d speculative)",
      t, pr, dt > 0 and dp / dt or 0, u32("audio_fills"), hw,
      st / SECTORS_PER_S, sk, fe, spec))

    last.t, last.sec, last.st, last.pr = t, sec, st, pr

    if t >= RUN_FOR then
      print(string.format("[%6.2fs] done", t))
      manager.machine:exit()
    end
  end
end)
