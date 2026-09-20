-- m14_voice.lua — do the spoken lines load and play?
--
-- The voice path has four places to fail and they look identical from the
-- outside (silence), so this reports each one separately rather than a verdict:
--
--   FlagSpeak    MESSAGE.C's master switch. Set by InitLanguage() only if
--                LBA.CFG names a LanguageCD *and* Wave_Driver_Enable is up. If
--                this is 0 nothing else below will ever be non-zero.
--   FdNar        the open .VOX. Non-zero means InitSpeak() found the archive —
--                i.e. fs.c matched "VOX\EN_xxx.VOX" down to its basename.
--   MaxVoice     entries in the offset table it read out of the file header.
--                Non-zero means the archive is not just present but parsed.
--   voices       WavePlay() calls the driver accepted on SPEAK_SAMPLE (0x1234),
--                the one handle MESSAGE.C speaks through. This is the only
--                counter that separates a voice from a footstep: `plays` keeps
--                climbing on sound effects whether or not dialogue works.
--   vfail        why the driver last refused a voice: 1 no output rate, 2 the
--                buffer did not parse as a VOC, 3 zero rate/length, 4 no free
--                channel. 2 is the interesting one — it would mean the LZSS
--                decompress into BufSpeak produced something that is not a VOC.
--
-- Voices are also the first genuinely random-access read this port does: each
-- line is a seek into a 6 MB archive. `seeks` is here to price that.
--
-- Walk-in is m12's: pulse green until MainLoop turns over, then hold the stick
-- so the scene runs. Green doubles as the dialogue's "next page", so once a
-- conversation starts this keeps it moving.

local S = dofile(assert(os.getenv("HSLBA_SYMS"), "set HSLBA_SYMS to build/syms.lua"))

local TRACE = 0xa0ef0000
local CON   = TRACE + 0x1000

local RUN_FOR = tonumber(os.getenv("HSLBA_SECONDS") or "150")

-- HSLBA_OBSERVE=1: keep the counters and the milestone log, but do not drive
-- the pad and never exit. The blind walk-in below reaches the intro narration
-- and no further — an actual conversation needs someone to walk to one, and it
-- cannot be reached while the script is holding the stick.
local OBSERVE = os.getenv("HSLBA_OBSERVE") == "1"

local mem, ports, frame, con_read = nil, nil, 0, 0
local pressing = true
local last = {}
local announced = {}

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

-- Report a milestone the first time it happens, with the time, so the log
-- shows the order things came up in rather than only the end state.
local function once(key, fmt, ...)
  if announced[key] then return end
  announced[key] = true
  print(string.format("[%6.2fs] " .. fmt, emu.time(), ...))
end

_G.m14 = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
    ports = manager.machine.ioport.ports
  end

  if pressing and frames() > 2 then
    pressing = false
    print(string.format("[%6.2fs] MainLoop running — walking. FlagSpeak=%d Language=%d LanguageCD=%d",
      emu.time(), u32("FlagSpeak"), u32("Language"), u32("LanguageCD")))
  end

  if not OBSERVE then
    local green = field(":ctrl0:IN.1", "Green")
    if green then
      green:set_value((pressing and (frame % PERIOD) < PRESS) and 1 or 0)
    end
    local ay = field(":ctrl0:IN.2", "Analog Y")
    if ay then
      ay:set_value(pressing and 0x7f or WALK_Y)
    end
  end

  drain()

  -- Milestones, checked every frame so the first one is timed properly.
  if u32("FdNar") ~= 0 then
    once("fd", "VOX archive OPEN (FdNar=%08X, MaxVoice=%d) — fs.c matched the basename",
      u32("FdNar"), u32("MaxVoice"))
  end
  -- Every voice, not just the first: while someone is playing, the useful log
  -- is one line per spoken line, with what it cost to fetch.
  local v = u32("wave_voice_plays")
  if v > (last.voices or 0) then
    last.voices = v
    print(string.format("[%6.2fs] voice #%d: %d Hz, %d samples (%.2f s)  seeks=%d stalls=%d",
      emu.time(), v, u32("wave_voice_rate"), u32("wave_voice_len"),
      u32("wave_voice_len") / math.max(1, u32("wave_voice_rate")),
      u32("cd_seek_count"), u32("cd_stall_count")))
  end

  -- MaxVoice changes when InitSpeak() switches archive — the per-island file
  -- swap, which the blind walk-in never reaches.
  local mv = u32("MaxVoice")
  if mv ~= (last.maxvoice or -1) and mv > 0 then
    last.maxvoice = mv
    print(string.format("[%6.2fs] VOX archive now has %d entries (InitSpeak switched file)",
      emu.time(), mv))
  end

  if u32("wave_voice_fail") ~= 0 then
    once("vfail", "voice REFUSED by the driver, reason=%d", u32("wave_voice_fail"))
  end

  if frame % 120 == 0 then
    local t = emu.time()
    print(string.format(
      "[%6.2fs] FlagSpeak=%d FdNar=%08X MaxVoice=%3d | voices=%d vfail=%d rate=%d len=%d | plays=%d drops=%d fills=%d | seeks=%d stalls=%d over=%d",
      t, u32("FlagSpeak"), u32("FdNar"), u32("MaxVoice"),
      u32("wave_voice_plays"), u32("wave_voice_fail"),
      u32("wave_voice_rate"), u32("wave_voice_len"),
      u32("wave_plays"), u32("audio_drops"), u32("audio_fills"),
      u32("cd_seek_count"), u32("cd_stall_count"), u32("cd_overrun_count")))
    last.t = t

    if t >= RUN_FOR and not OBSERVE then
      print(string.format("[%6.2fs] done", t))
      manager.machine:exit()
    end
  end
end)
