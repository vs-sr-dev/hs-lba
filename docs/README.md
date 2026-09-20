# docs/

Two kinds of thing live here, and only one of them is in the repository.

**Ours:** `mame-card-resp-idx.patch` — a write-up of an emulation defect in
MAME's HyperScan RFID card (the response index is a `uint8_t` and a `RALL`
answer is 1114 bits long), with the one-line fix. It is reproduced in
`mame/0003-hyperscan-card-resp-idx.patch` in `patch -p1` form; the file here
keeps the explanation and the before/after numbers.

**Sunplus':** the S+core7 programmer's manual (`S+core7_pro.pdf`), the S+core
IDE help file (`S+core IDE.chm`) and the SJProbe user guide. They are in the
`Documentation/` and `Tools/` directories of the
[HyperScan-SPG29x-SDK](https://github.com/ppcasm/HyperScan-SPG29x-SDK) and
are gitignored here. The CPU manual is worth having open: the ISA facts the
port relies on (the hardware loop counter `sr0`, the CEH/CEL result pair,
the `mfcr`/`mtcr` coprocessor registers) all come from it.
