# sdk/ — the slice of the HyperScan SDK this port builds against

Twenty-eight files out of ppcasm's
[HyperScan-SPG29x-SDK](https://github.com/ppcasm/HyperScan-SPG29x-SDK)
(the Sunplus PPCSDK with HyperScan support), copied here so that the build
does not depend on a checkout of the whole SDK. The tree mirrors the
original layout so the diff against upstream is a plain `diff -r`:

```
SPG290/scorelibs/            TV, PPU, UART, NorFlash drivers; the register map
SPG290/scorelibs/IRQ/        Sys_isr.s — vector table and context save/restore
System/HyperScan/hslibs/     HS_Controller and FatFS headers (headers only used)
boot/                        hyperscan_startup.s, hyperscan_Prog.ld, resource.ld
                             (from examples/ALL/HelloWorld)
```

What is compiled: `TV.c`, `PPU.c`, `UART.c`, `NorFlash.c`, `Sys_isr.s`,
`hyperscan_startup.s`, and the linker script. Everything else is here for its
headers.

What is deliberately **not** compiled, and why, is in the Makefile — the I²C
and controller drivers deadlock, the IRQ dispatch is a 64-way switch into
empty handlers, and `libgloss.c` puts an 8 MB heap in `.bss` on top of the
framebuffers. `src/` replaces each of them.

## Local edits

Two files differ from upstream, both on purpose:

- **`SPG290/scorelibs/IRQ/Sys_isr.s`** — the context save did not cover
  CEH/CEL (the multiply/divide result pair) nor `sr0`, the hardware loop
  counter GCC uses for small copy loops; and the restore path handed `r30`/`r31`
  back holding CR/EPC. Both fixed in place, with the reasoning in the comments.
  [DEVLOG.md](../DEVLOG.md) has the story of how the first one was found.
- **`boot/hyperscan_startup.s`** — the HelloWorld startup jumps to `main`
  after clearing `.bss` itself; this one jumps to newlib's `_start` and leaves
  the cache toggling commented out.

## Licence

The SDK's own README says, in full: *"Free Software, Hell Yeah!"*. The
`scorelibs` sources carry Sunplus headers. Nothing here is claimed as ours
beyond the two edits above.
