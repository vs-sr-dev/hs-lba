-- m10_wedge.lua — why the machine stops when the pad is read from the vblank ISR.
--
-- Symptom: TimerRef freezes a few seconds in and Fire latches at its last
-- value, i.e. the CPU is stuck somewhere with interrupts masked. The candidates
-- are (a) the I2C read failing and burning its whole 200k-spin budget per byte,
-- (b) interrupts nesting into the vblank handler and running the stack out, and
-- (c) a fault. Each leaves a different fingerprint in these counters, so read
-- them rather than reason about them:
--
--   n_transfers / n_timeouts   how many byte reads were attempted, how many gave up
--   vblanks / counts[]         whether the handler is being entered repeatedly
--   PC histogram               where the CPU actually is once it stops

local S = dofile(assert(os.getenv("HSLBA_SYMS"), "set HSLBA_SYMS to build/syms.lua"))
local LIMIT = tonumber(os.getenv("HSLBA_FRAMES") or "900")
local TRACE = 0xa0ef0000

local mem, cpu, frame = nil, nil, 0
local pchist = {}
local prev = {}

_G.m10w = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
    cpu = manager.machine.devices[":maincpu"]
  end

  -- Sample the PC every frame; once the machine wedges this concentrates.
  local pc = cpu.state["PC"].value
  pchist[pc] = (pchist[pc] or 0) + 1

  if frame % 150 == 0 then
    local t   = mem:read_u32(S["TimerRef"])
    local vb  = mem:read_u32(S["vblanks"])
    local tr  = mem:read_u32(S["n_transfers"])
    local to  = mem:read_u32(S["n_timeouts"])
    local cds = S["cd_streaming"] and mem:read_u32(S["cd_streaming"]) or -1
    print(string.format(
      "[%6.2fs] TimerRef=%d(+%d) vblanks=%d(+%d) i2c=%d(+%d) timeouts=%d(+%d) cdstream=%d fault=%08X pc=%08X",
      emu.time(), t, t - (prev.t or t), vb, vb - (prev.vb or vb),
      tr, tr - (prev.tr or tr), to, to - (prev.to or to), cds,
      mem:read_u32(TRACE + 56), pc))
    prev.t, prev.vb, prev.tr, prev.to = t, vb, tr, to
  end

  if frame > LIMIT then
    local top = {}
    for k, v in pairs(pchist) do top[#top + 1] = { k, v } end
    table.sort(top, function(a, b) return a[2] > b[2] end)
    print("--- most-sampled PCs ---")
    for i = 1, math.min(8, #top) do
      print(string.format("  %08X  %d", top[i][1], top[i][2]))
    end
    manager.machine:exit()
  end
end)
