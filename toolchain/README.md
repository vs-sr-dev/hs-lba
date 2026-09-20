# toolchain/ — GCC 4.2.1 for S+core, from Sunplus

The port is built with the **official Sunplus toolchain**, not with a modern
GCC and a resurrected `score` backend. The sixteen "score-elf GCC bugs"
documented around the homebrew scene are artefacts of that resurrected
backend (built big-endian and byte-swapped afterwards); the official compiler
has none of them — `tests/torture/` re-tests every one of them and
`torture.dis` is the evidence.

`toolchain/gnu/` is **not in the repository**: 138 MB of binaries that are
Sunplus' to distribute. Get them from the SDK's own installer.

## Extracting it

The installer is `Tools/S+core IDE-V2.6.1.exe` in the
[HyperScan-SPG29x-SDK](https://github.com/ppcasm/HyperScan-SPG29x-SDK). It is
an InstallShield package, and there is no need to run it:

```sh
# 1. unpack the self-extractor (Windows; produces Disk1/data1.cab etc.)
"S+core IDE-V2.6.1.exe" /extract_all:ide

# 2. unpack the cabinet (unshield: apt install unshield, or brew/msys2)
unshield -d out x ide/Disk1/data1.cab

# 3. the 'gnu' directory inside the output is the toolchain
mv out/<...>/gnu toolchain/gnu
```

The layout the Makefile expects:

```
toolchain/gnu/bin/score-elf-gcc.exe        GCC 4.2.1
toolchain/gnu/bin/score-elf-objcopy.exe    (and the rest of binutils, gdb)
toolchain/gnu/score-elf/lib/mel/           newlib, little-endian multilib
```

Flags are the SDK's own: `-mscore7 -mel -Os`. **`-mel` is little-endian**,
which is what the console is; the ELF comes out native and there is no
byte-swap step.

## Two things to know

- The `make.exe` that ships in `gnu/bin` is GNU Make 3.79 and does not
  understand order-only prerequisites. Use MSYS2's `make` (see the README).
- `#pragma pack` is not implemented on this target. GCC prints a warning and
  lays the struct out with natural alignment anyway — and the `-w` that a
  1994 codebase needs hides the warning. `__attribute__((packed))` works;
  `compat/watcom_compat.h` wraps it as `PORT_PACKED`.
