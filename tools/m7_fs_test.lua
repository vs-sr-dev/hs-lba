-- m7_fs_test.lua — check the stdio/filesystem/heap layer from outside.
--
-- The milestone reads every file in the disc archive through fopen/fread and
-- hashes it against the checksum tools/mkcd.py stored at build time; this
-- watches the counters it publishes and fails the run if anything mismatches.
--
--   hyprscan.exe hyprscan -rompath "<roms>;build" -cdrom build/hslba.iso \
--       -quickload build/HYPER.EXE -autoboot_script tools/m7_fs_test.lua \
--       -autoboot_delay 0 -nothrottle -video none -seconds_to_run 180

local TRACE = 0xa0ef0000        -- src/memmap.h
local mem
local frame = 0
local done = false
local last_report = -1

local function trace(i) return mem:read_u32(TRACE + i * 4) end

local function s32(v)
  v = v & 0xffffffff
  if v >= 0x80000000 then return v - 0x100000000 end
  return v
end

_G.m7_sub = emu.add_machine_frame_notifier(function()
  if done then return end
  frame = frame + 1

  if not mem then
    mem = manager.machine.devices[":maincpu"].spaces["program"]
  end

  local marker = trace(0)

  if marker == 0xaa0000f7 then
    print("!! no archive on disc (cd_init or fs_mount failed)")
    done = true
    manager.machine:exit()
    return
  end

  if trace(14) ~= 0 then
    print(string.format("!! FAULT: cause %08X at PC %08X", trace(14), trace(15)))
    done = true
    manager.machine:exit()
    return
  end

  -- Progress, so a long verify is visibly alive.
  local checked = trace(1)
  if checked ~= last_report and checked > 0 then
    last_report = checked
    print(string.format("  [%5.1fs] %d files, %d matched, %d KB read",
          emu.time(), checked, trace(2), trace(3)))
  end

  if marker == 0xaa000006 and frame > 30 then
    local files   = trace(1)
    local matched = trace(2)
    local kb      = trace(3)
    local bad     = s32(trace(4))
    local hfree   = trace(5)
    local hused   = trace(6)
    local heap_rc = s32(trace(7))
    local seek_rc = s32(trace(9))
    local seeks   = trace(10)
    local sectors = trace(11)

    print("")
    print(string.format("archive: %d files, %d KB read through stdio", files, kb))
    print(string.format("checksums matched: %d/%d", matched, files))
    print(string.format("fseek/ftell test:  %s", seek_rc == 0 and "ok"
          or ("FAIL(" .. seek_rc .. ")")))
    print(string.format("malloc 4 MB test:  %s", heap_rc == 0 and "ok"
          or ("FAIL(" .. heap_rc .. ")")))
    print(string.format("heap: %d KB used, %d KB free", hused, hfree))
    print(string.format("CD: %d seeks for %d sectors (%.1f sectors/seek)",
          seeks, sectors, sectors / math.max(seeks, 1)))
    print(string.format("     at 150 ms/seek on a real mech that is %.1f s of seeking",
          seeks * 0.150))

    local ok = true
    local function check(name, cond)
      if not cond then ok = false; print("  FAIL: " .. name) end
    end
    check("all files present", files >= 11)
    check("every checksum matched", matched == files)
    check("no mismatching file", bad == -1)
    check("fseek/ftell agree with the archive", seek_rc == 0)
    check("4 MB allocation lands inside the heap", heap_rc == 0)
    check("heap has room for the HQR working set (>10 MB)", hfree > 10 * 1024)
    -- The read-ahead run exists to keep this number small: a naive
    -- sector-at-a-time path issued ~9000 seeks for the same data.
    check("read path averages a whole run per seek", sectors / math.max(seeks, 1) > 30)
    print(ok and "\nALL CHECKS PASSED" or "\nCHECKS FAILED")

    done = true
    manager.machine:exit()
  end

  if frame > 12000 then
    print(string.format("!! timed out at marker %08X, %d files checked",
          marker, trace(1)))
    done = true
    manager.machine:exit()
  end
end)
