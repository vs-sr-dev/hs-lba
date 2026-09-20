-- m5_input_test.lua — drive the HyperScan pad from MAME and assert on what the
-- driver in src/input.c actually read back out of the I2C master.
--
-- Injects a known input, holds it for a few frames, then samples the TRACE
-- block the milestone mirrors its decoded state into. Screenshots prove
-- nothing; this proves the bytes made it across the bus.
--
--   hyprscan.exe hyprscan -rompath "<roms>;build" -quickload build/HYPER.EXE \
--       -autoboot_script tools/m5_input_test.lua -autoboot_delay 0 \
--       -nothrottle -video none -seconds_to_run 60
--
-- Note the `_G.m5_sub =` on the notifier at the bottom: the subscription object
-- cancels the callback when collected, so dropping it makes the script look
-- like it silently dies after one frame.

local mem, ports
local frame = 0
local step = 1
local settle = 0
local held = {}
local results = {}
local dumped = false
local bench = {}
local last_marker, marker_t = 0, 0

local TRACE = 0xa0a00000

local function trace(i) return mem:read_u32(TRACE + i * 4) end

-- name -> {port tag, field name}
local PAD = {
  green  = { ":ctrl0:IN.1", "Green" },
  red    = { ":ctrl0:IN.1", "Red" },
  yellow = { ":ctrl0:IN.1", "Yellow" },
  blue   = { ":ctrl0:IN.0", "Blue" },
  start  = { ":ctrl0:IN.0", "Start" },
  select = { ":ctrl0:IN.0", "Select" },
  ls     = { ":ctrl0:IN.0", "Left Shoulder" },
  rs     = { ":ctrl0:IN.0", "Right Shoulder" },
  lt     = { ":ctrl0:IN.0", "Left Trigger" },
  rt     = { ":ctrl0:IN.0", "Right Trigger" },
  ay     = { ":ctrl0:IN.2", "Analog Y" },
  ax     = { ":ctrl0:IN.3", "Analog X" },
}

local function field(name)
  local d = PAD[name]
  if not d then return nil end
  local p = ports[d[1]]
  if not p then return nil end
  return p.fields[d[2]]
end

-- steps: {label, {inputs}, expectation function}
local STEPS = {
  { "idle",        {} },
  { "green",       { green = 1 } },
  { "red",         { red = 1 } },
  { "yellow",      { yellow = 1 } },
  { "blue",        { blue = 1 } },
  { "start",       { start = 1 } },
  { "select",      { select = 1 } },
  { "lt+rt",       { lt = 1, rt = 1 } },
  { "ls+rs",       { ls = 1, rs = 1 } },
  { "green+start", { green = 1, start = 1 } },
  { "stick Y=0",   { ay = 0 } },
  { "stick Y=255", { ay = 255 } },
  { "stick X=0",   { ax = 0 } },
  { "stick X=255", { ax = 255 } },
  { "idle again",  {} },
}

local HOLD = 30    -- frames to hold each input; one pad poll is ~2 ms

local function apply(inputs)
  for k, _ in pairs(PAD) do
    local f = field(k)
    if f then
      if inputs[k] ~= nil then
        f:set_value(inputs[k])
      elseif k == "ax" or k == "ay" then
        f:set_value(0x7f)
      else
        f:set_value(0)
      end
    end
  end
end

local function sample()
  local raw = trace(4)
  return {
    polls   = trace(1),
    buttons = trace(2),
    dpad    = trace(3),
    r0      = raw & 0xff,
    r1      = (raw >> 8) & 0xff,
    r2      = (raw >> 16) & 0xff,
    r3      = (raw >> 24) & 0xff,
    chk     = trace(5),
    ax      = trace(6),
    ay      = trace(7),
    xfer    = trace(8),
    to      = trace(9),
  }
end

local function s8(v)
  v = v & 0xffffffff
  if v >= 0x80000000 then return v - 0x100000000 end
  return v
end

local function dpad_str(d)
  return ((d & 1) ~= 0 and "U" or ".") .. ((d & 2) ~= 0 and "D" or ".")
      .. ((d & 4) ~= 0 and "L" or ".") .. ((d & 8) ~= 0 and "R" or ".")
end

local function tick()
  frame = frame + 1

  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
    ports = manager.machine.ioport.ports
  end

  if not dumped then
    print("=== ioport fields ===")
    for tag, port in pairs(ports) do
      for fname, _ in pairs(port.fields) do
        print(string.format("  %-16s %s", tag, fname))
      end
    end
    print("=== marker " .. string.format("%08X", trace(0)) .. " ===")
    dumped = true
  end

  -- Time the two benchmark phases by the slope of the transfer counter, so
  -- attaching part-way into a phase still measures it correctly.
  if step == 1 and settle == 0 then
    local marker = trace(0)

    if marker == 0xaa000003 or marker == 0xaa000004 then
      local key = (marker == 0xaa000003) and "single" or "full"
      local b = bench[key]
      if not b then
        bench[key] = { t0 = emu.time(), n0 = trace(1) }
      else
        b.t1, b.n1 = emu.time(), trace(1)
      end
      return
    end

    if marker ~= 0xaa000002 or trace(1) < 4 then
      if frame > 1800 then
        print("!! milestone never reached the viewer loop; marker=" ..
              string.format("%08X", marker) .. " polls=" .. trace(1))
        manager.machine:exit()
      end
      return
    end

    if not bench.printed then
      bench.printed = true
      local function rate(b)
        if not b or not b.t1 or b.n1 <= b.n0 then return nil end
        return (b.t1 - b.t0) / (b.n1 - b.n0) * 1e6
      end
      local one, five = rate(bench.single), rate(bench.full)
      print("")
      if one and five then
        print(string.format("bus: %.0f us/byte, %.0f us per 5-byte pad read"
              .. "  (%.1f%% of a 16.7 ms frame)", one, five, five / 16700 * 100))
      else
        print("bus timing: phase not sampled (one=" .. tostring(one)
              .. " five=" .. tostring(five) .. ")")
      end
    end
  end

  local s = STEPS[step]
  if not s then return end

  if settle == 0 then
    print(string.format("[f%d] step %d: %s", frame, step, s[1]))
    apply(s[2])
    held = s[2]
  else
    apply(held)   -- analog fields autocentre; keep driving them
  end

  settle = settle + 1
  if settle >= HOLD then
    local r = sample()
    r.label = s[1]
    results[#results + 1] = r
    settle = 0
    step = step + 1

    if step > #STEPS then
      print("")
      print("=== m5 controller readback ===")
      print(string.format("%-13s %-6s %-5s %8s %8s %5s %5s",
            "input", "btn", "dpad", "raw01", "raw23", "ax", "ay"))
      for _, x in ipairs(results) do
        print(string.format("%-13s %04X   %s  %02X %02X    %02X %02X   %+4d  %+4d",
              x.label, x.buttons, dpad_str(x.dpad),
              x.r0, x.r1, x.r2, x.r3, s8(x.ax), s8(x.ay)))
      end
      local last = results[#results]
      print("")
      print(string.format("transfers %d   timeouts %d   polls %d",
            last.xfer, last.to, last.polls))
      print(string.format("poll rate: %.1f polls/s over %d frames",
            last.polls / (frame / 60.0), frame))
      manager.machine:exit()
    end
  end
end

local dead = false
-- The subscription must be kept alive: dropping it lets the GC cancel the
-- callback, which looks exactly like the script silently dying after one frame.
_G.m5_sub = emu.add_machine_frame_notifier(function()
  if dead then return end
  local ok, err = pcall(tick)
  if not ok then
    dead = true
    print("!! lua error: " .. tostring(err))
    manager.machine:exit()
  end
end)
