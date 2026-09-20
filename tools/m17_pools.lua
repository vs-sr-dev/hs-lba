-- m17_pools.lua — how big are the resource caches, and do they still thrash?
--
-- PERSO.C sizes the sprite, sample and animation caches from Malloc(-1), which
-- on DOS answered "how much memory is free". The port answered 0, so every
-- pool fell to its floor — 50K sprites, 200K samples, 100K animations — on a
-- console with about twelve megabytes going spare. A cache at a twentieth of
-- its intended size does not degrade gracefully: it evicts something that is
-- still in use, and the game either re-reads it off the CD (the hitch on every
-- new sound) or carries on without it (an animation that never plays).
--
-- Expected after the fix, with the engine's own clamps still applied on top:
--
--     SpriteMem   400,000   (was 50,000)
--     SampleMem 4,500,000   (was 200,000)   — SAMPLES.HQR is 2.4 MB on disc
--     AnimMem     300,000   (was 100,000)   — the shipped DOS figure
--
-- AnimMem stays at 300K because that is the cap the game shipped with, not
-- because it is all we can afford. If animations still thrash at that size the
-- cap is the next thing to raise — but raising it before knowing would confuse
-- a fix with an improvement.
--
-- `seeks` is the thrash detector: with the caches right, a long stretch of
-- gameplay should stop generating new CD activity once a scene is warm.

local S = dofile(assert(os.getenv("HSLBA_SYMS"), "set HSLBA_SYMS to build/syms.lua"))

-- HEAP_END from src/memmap.h: sbrk stops here, so free = HEAP_END - heap_brk.
local HEAP_END = 0xa0ef0000

local mem, frame = nil, 0
local last = {}
local pools = nil

local function u32(n)
  local a = S[n]
  return a and mem:read_u32(a) or -1
end

_G.m17 = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then mem = manager.machine.devices[":maincpu"].spaces["program"] end

  -- Print on every *change*, not once. Restarting a game re-runs the init that
  -- sizes these, and the interesting question is whether the second and third
  -- pass get the same answer: Malloc(-1) reports what is left, so pools that
  -- are never freed make each restart allocate less than the last until
  -- something does not fit.
  local key = string.format("%d/%d/%d", u32("SpriteMem"), u32("SampleMem"), u32("AnimMem"))
  if u32("SampleMem") > 0 and key ~= pools then
    pools = key
    local brk = u32("heap_brk")
    print(string.format(
      "[%6.2fs] pools sized: Sprite=%d Sample=%d Anim=%d  |  heap_brk=%08X used=%.2f MB free=%.2f MB",
      emu.time(), u32("SpriteMem"), u32("SampleMem"), u32("AnimMem"),
      brk, (brk - 0xa0090000) / 1048576, (HEAP_END - brk) / 1048576))
    print(string.format(
      "           pool handles: Sprite=%08X Sample=%08X Anim=%08X Inv=%08X",
      u32("HQRPtrSpriteExtra"), u32("HQR_Samples"), u32("HQR_Anims"),
      u32("InventoryObj")))
  end

  if frame % 120 == 0 then
    local t   = emu.time()
    local sk  = u32("cd_seek_count")
    local sec = u32("cd_sector_count")
    local pl  = u32("wave_plays")
    local brk = u32("heap_brk")
    local dsec, dpl = sec - (last.sec or sec), pl - (last.pl or pl)

    -- The microfreeze question, stated so it can be answered by reading:
    -- sounds played in this window, against sectors the CD had to deliver.
    -- A sample that came out of the cache costs nothing; a sample that was
    -- evicted and re-read shows up here as disc activity landing in the same
    -- window as the play. Once SAMPLES.HQR is resident the disc should go
    -- quiet and stay quiet while `plays` keeps climbing.
    local verdict = ""
    if dpl > 0 then
      verdict = (dsec == 0) and "   <- sounds from cache, disc idle"
                             or string.format("   <- %d sectors read while sounds played", dsec)
    end

    print(string.format(
      "[%6.2fs] cd: %d seeks (+%d)  %d sectors (+%d)  |  plays=%d (+%d) voices=%d drops=%d  |  free %.2f MB%s",
      t, sk, sk - (last.sk or sk), sec, dsec, pl, dpl,
      u32("wave_voice_plays"), u32("audio_drops"),
      (HEAP_END - brk) / 1048576, verdict))
    last.t, last.sk, last.sec, last.pl = t, sk, sec, pl
  end

  -- A fault parks in intmsg() (src/irq.c) with the cause and PC where they can
  -- still be read out of a wedged machine. A garbage screen is usually that,
  -- and saying so beats guessing from what the screen looked like.
  local fault = mem:read_u32(0xa0ef0000 + 4 * 14)   -- TRACE_FAULT, src/trace.h
  if fault ~= 0 and not last.fault then
    last.fault = true
    print(string.format("[%6.2fs] *** FAULT %08X (cause %d) at PC %08X ***",
      emu.time(), fault, fault & 0x1f, mem:read_u32(0xa0ef0000 + 4 * 15)))
  end
end)
