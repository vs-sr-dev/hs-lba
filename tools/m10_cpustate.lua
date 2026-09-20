-- m10_cpustate.lua — dump the CPU's own view once the machine stops.
--
-- The wedge parks the PC on general_vec (0xA0090200) without ever reaching
-- intmsg, so the fault slots stay empty and the exception cause has to come
-- from the core itself: CR2 (ECR) says what kind of exception, CR5 (EPC) says
-- which instruction raised it, CR0 (PSR) says whether interrupts were on.

local mem, cpu, frame = nil, nil, 0
local dumped = false

_G.m10c = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if not cpu then
    cpu = manager.machine.devices[":maincpu"]
    mem = cpu.spaces["program"]
    local names = {}
    for k in pairs(cpu.state) do names[#names + 1] = k end
    table.sort(names)
    print("state registers: " .. table.concat(names, " "))
  end

  if frame == 400 and not dumped then
    dumped = true
    for _, k in ipairs({ "PC", "CURPC", "cr0", "cr2", "cr3", "cr5", "r0", "r3", "r4" }) do
      local r = cpu.state[k]
      if r then print(string.format("  %-4s = %08X", k, r.value)) end
    end
    manager.machine:exit()
  end
end)
