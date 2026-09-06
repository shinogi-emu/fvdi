# Raster conversion, remapping, dithering and scaling

Independent implementations of documented NVDI interfaces. No NVDI or MagiC
implementation code was copied. NVDI 5.03 was tested only through public VDI
calls using the user's existing binaries.

## Supported subset

| Interface | Support |
| --- | --- |
| `vr_transfer_bits` source | Memory xRGB32 (`PX_PREF32`), RGB565 (`PX_MATRIX16`), RGB555 (`PX_PREF15`), or byte-indexed `PX_PREF8`. RGB16 includes reversed byte order. |
| RGB16 destination | Memory RGB565/RGB555, including reversed byte order; null destination for supported RGB16 screen component maps. |
| Indexed destination | Memory `PX_PREF8`; null destination for native interleaved 1/2/4/8-plane screens. |
| New transfer modes | Copy (0), replace (32), with optional error diffusion (128). |
| Colour tables | Current/default tables and entries, setters, change-sensitive IDs, fresh IDs, create/delete, index-to-pixel values and recognized workstation pixel-format queries. |
| Inverse tables | Workstation-owned RGB inverse maps at 3/4/5 bits per channel, with explicit deletion and workstation cleanup. |
| Legacy scaling | Bit 15 in `vro_cpyfm`/`vrt_cpyfm`: nearest memory-source scaling, all 16 Boolean operations or all four monochrome-expansion modes. |

Direct-colour reductions use centred bilinear interpolation with eight
fractional bits; enlargement uses nearest neighbours. Indexed reductions
without active dithering sample indices. RGB16 components expand by bit
replication. This four-sample reduction filter is not an area filter for
severe photographic shrinking, and is not claimed pixel-identical to NVDI.

Equal/subset palettes bypass dithering. Exact matches compare all 16 component
bits, prefer the same index, then the first exact destination match. Duplicate
indices survive equal-palette copies. Source palettes below 256 entries require
a preflight of the entire requested source rectangle before output.

An omitted indexed palette resolves to the device palette at the same indexed
depth, otherwise fVDI's default for that depth. Internal destination inverse
tables are cached and rebuilt after palette changes. Explicit destination
palettes require matching explicit inverse tables. The old unpaletted,
native-size `PX_PREF8` copy remains a raw byte copy in modes 0/32 without dither.

Legacy `vro_cpyfm` sources have the workstation's depth; `vrt_cpyfm` sources
are monochrome. Standard separated-plane source MFDBs are accepted. Destinations
are native memory MFDBs or the screen. Host references cover packed 8/16/24/32
and interleaved 1/2/4/8-bit workstations; real-driver tests cover RGB565 and
interleaved 1/2/4/8 planes.

This does **not** advertise complete new-raster/scaling capability bits in
`vq_extnd`. Screen sources, arbitrary layouts, 24/32-bit new-transfer
destinations, arithmetic/colourized/transparent new-transfer modes and full
NVDI compatibility remain outside this subset. Other pairs retain the old
handler. `vr_trnfm` remains a layout conversion, not a general true-colour
scaler. Pixel-format queries return zero for unrecognized component layouts
instead of falsely reporting xRGB32.

## Bounds, storage and fast paths

Rectangles are inclusive; GCBITMAP bounds are exclusive. Padded byte strides
and nonzero/negative bitmap origins work. Multi-byte pixels require even
addresses/strides; byte indices may be odd. Memory rectangles must be in
bounds and ignore workstation clipping. Screen clipping preserves sampling
phase. Pass the screen as a null destination, not an explicit memory bitmap.

Invalid bounds/strides for supported pairs, unsupported modes, overlapping pixel
buffers, invalid source indices and unavailable staging storage refuse the
operation without output. Callers must own valid storage for their descriptions.
Screen sources and in-place scaling are refused. Diffusion carries the original
rectangle's continuous error stream through clipping and strip boundaries;
separate calls start independent streams. Allocation failures precede output,
although existing cached tables may remain.

- Fused conversion/scaling: no frame-sized intermediate, floating point,
  production 64-bit arithmetic or per-pixel coordinate division.
- Constant-mask RGB32-to-RGB565 loops, native-width shortcuts and repeated
  nearest-row reuse. Identically packed RGB565 avoids unpack/repack; native
  rows use `memcpy`. RGB555 unused bits remain normalized.
- Indexed-to-RGB16 pre-packs 512 bytes in a separate helper, off the RGB32
  stack. Indexed remapping uses 1,280 bytes of palette/remap workspace and
  no persistent remap cache.
- Nondithered memory transfers need no per-call scratch allocation. Dithering
  uses one `6 * destination_width` error row (1,920 bytes at width 320).
- Screen output borrows an existing fVDI pool block and calls the driver blit.
  RGB16 batches rows/chunks wide rows. Indexed output presents planar rows;
  its byte row shares the block when possible, otherwise it allocates width
  bytes. Diffusion still requires its error row.
- Legacy scaling uses one pool block and reuses staged enlargement rows.
- Planar pixel reads inspect plane words directly, without a temporary blit.

Raster traps and default-table setters use fVDI's existing 8 KB VDI work stack.
Nested calls already on it retain their current stack pointer. EmuTOS's 2 KB
supervisor stack is too small for the indexed palette/filter/blit chain.
Existing fVDI serialization/work-stack conventions still apply; this does not
make fVDI reentrant.

With this checkout's MiNT GCC 4.6.4 68020 engine options, the transfer,
inverse-table, colour-table and legacy-scaler objects contain 16,248 text
bytes in total. Their data/BSS adds 1,552 bytes, mostly the reused default
palette. The linked engine is 560,198 bytes in this build; these figures
depend on compiler options and the existing FreeType configuration.

Inverse lookup chooses the nearest RGB8 palette colour at each cell centre
using squared distance; ties choose the first entry. It is an approximation,
not an exact search for every unquantized RGB value. Explicit palette edits,
including low component bits, invalidate the snapshot. Opaque inverse IDs are
workstation-owned, never recycled, and checked without dereferencing supplied
IDs. Created colour tables are real mutable pointers and cannot be used after
deletion. Closing a workstation reclaims both kinds of resource.

M68k inverse allocation, excluding allocator overhead:
`16 + 8 * palette_entries + grid_bytes`.

| Bits/channel | Grid bytes | Total with 256 colours |
| --- | ---: | ---: |
| 3 | 512 | 2,576 |
| 4 | 4,096 | 6,160 |
| 5 | 32,768 | 34,832 |

```c
dst.ctab = palette;
dst.itab = v_create_itab(handle, palette, 4);
if (dst.itab) {
    /* Reuse while palette entries remain unchanged. */
    vr_transfer_bits(handle, &src, &dst, sr, dr, T_REPLACE | T_DITHER_MODE);
    v_delete_itab(handle, dst.itab);
    dst.itab = 0;
}
```

Bitplane palette updates use documented ST/STE/TT/Falcon XBIOS calls and track
requested/quantized colours without probing unrelated hardware. Physical
colours retain hardware quantization limits. Defaults reuse fVDI's TOS palette.

## Verification and review

```sh
make -C fvdi/tests check
make -C fvdi/engine CPU=020
make -C fvdi/drivers/bitplane CPU=020
make -C fvdi/drivers/16_bit CPU=020
make -C fvdi/tests guest GUEST_CPU=060
```

Host ASan/UBSan checks 16,472 RGB16 cases, 1,442 indexed image cases and 2,560
legacy combinations, plus refusals, allocation failures, lifecycle and complete
inverse grids. Independent references use 64-bit coordinates/weighted sums,
brute-force palette searches and two wide diffusion rows. Guards cover pixel
storage, staging, clipping, shadows, small blocks and odd byte storage.
Allocator wrappers verify balance; LeakSanitizer is disabled for sandbox
compatibility. Make prints the retained temporary test directory.

`REFCHECK.PRG` runs these references with the m68k data model, with serial
progress/assertions. `RASTER.PRG` tests real VDI traps in a disposable boot:
physical workstation before AES, or `--aes` to register, open a virtual
workstation, lock screen updates and hide the cursor. It overwrites screen
areas and releases resources on exit.

Real tests cover the normal Shinogi MiNT/XaAES `m68060` stack at 1280x720
RGB565 and Hatari ST/STE/TT at 1/2/4/8 planes. They include palette/index
ordering, clipped diffusion, indexed markers, pixel/pen queries, all legacy
modes and readback. Legacy buffers have allocation guards. GEMDOS allocation
probes after planar phases catch corruption invisible in pixel comparisons.
Standalone references also pass on `m68060`.

Changed raster/colour C code compiles warning-free for 68000/020/060/ColdFire;
the assembly hooks also assemble for 68000. These are not physical hardware
tests. ColdFire is a C compile check, not a complete link or guest run.

The separate review corrected supervisor-stack overflow, an interior-pointer
free during negative-palette promotion, shared-palette mutation on allocation
failure, planar pixel/pen query faults, incorrect table lengths and false
pixel-format reporting. Optimizations separate the indexed palette stack,
share pooled row storage, bypass identical RGB565 repacking and read planar
pixels directly. The previously failing marker-plus-allocation control passes
with the work-stack fix.

### Disposable installation

For Shinogi, copy the normal installation to a temporary tree. Replace only
its `AUTO/FVDI.PRG`, `GEMSYS/16_BIT.SYS` and root `RASTER.PRG`. Keep
`AUTO/060SP.PRG`, MiNT, fonts and desktop settings. Add
`run c:\RASTER.PRG --aes` to its `MINT/1-19-CUR/XAAES/XAAES.CNF`. Use Shinogi's
`tools/run-stack.sh` with `KEEP_TREE=1`, temporary `STACK_TREE`/`STACK_OUT`,
`SHINOGI_CPU=m68060`, `SHINOGI_ELF=.../emutos-virt-060.elf`, `RES=1280x720`,
`NET=1`, `BOOT_WAIT=60`. Validate with:

```sh
make -C fvdi/tests check-guest-log GUEST_LOG=/absolute/path/to/serial.log
```

Use the matching current RGB16 driver: an older copy predating its full-colour
component fix fails the CTAB pixel-value assertion. The normal 060 compatibility
handler is required. Run REFCHECK in a separate AUTO boot: the observed setup
did not resume AUTO scanning after REFCHECK exited.

For planar tests build `guest GUEST_CPU=020`. In a disposable Hatari tree put
`FVDI.PRG` then `RASTER.PRG` in AUTO, `BITPLANE.SYS` in GEMSYS, and configure:

```text
booted
cookie nvdi = $0501
01r BITPLANE.SYS
```

Use an EmuTOS 1.4 512K ROM and Hatari `--cpulevel 3 --fpu 68882 --cpu-exact
false --compatible false --addr24 false --memsize 14 --fast-forward true
--sound off --rs232-in /dev/null --rs232-out /temporary/serial.log`.
The test runtime needs the FPU setting; raster code is integer-only. ST
high/medium/low give 1/2/4 planes; TT low gives 8. For two/eight planes,
install the built `MODE2.PRG`/`MODE8.PRG` as `AUTO/00MODE.PRG` BEFORE fVDI.
Use an ST/STE colour monitor for MODE2, TT with a VGA monitor for MODE8.
These setup programs change resolution and are for disposable boots only.
Hatari's desktop `--tos-res` setting alone did not change the mode seen by
the AUTO tests. Require the actual `PLANAR SCREEN BENCH depth=N` log entry.
For one plane, use a monochrome monitor without a mode-setup program; for
four planes, use ST/STE low resolution without one. Allow enough VBLs for
the benchmark or stop on its final marker. Validate with `check-planar-log`.

## Timings and limitations

`BENCH.PRG` compares real memory VDI calls with an independent two-pass nearest
scaler plus RGB565 conversion. Allocation is outside timing; pixels must match
before timing. Cross-VDI input uses bit-replicated RGB5 green because the tested
NVDI 5.03 path quantizes green through five bits. fVDI retains full RGB565
precision; normal regression input retains all green bits.

Same Hatari STE/68030/68882 settings and binary; three samples of eight calls,
guest 200 Hz ticks:

| Transfer | fVDI | NVDI 5.03 |
| --- | ---: | ---: |
| 320x200 to 320x200 | 3178 / 3179 / 3179 | 5671 / 5671 / 5671 |
| 160x100 to 320x200 | 2301 / 2301 / 2301 | 6187 / 6186 / 6187 |
| 197x113 to 320x200 | 2579 / 2579 / 2578 | 6372 / 6372 / 6372 |

These paths took about 37–56% of NVDI's time. Adding RGB16 sources changed
the common RGB32 measurements by under 0.2%. This is emulator evidence for
these cases, not general NVDI or physical-Atari throughput. NVDI's disposable
tree used its existing 5.03 Atari drivers, not the ARAnyM driver; config files
needed CRLF endings. Validate all nine samples with `check-bench-log`.

Normal Shinogi 68060 MiNT/XaAES, complete RGB565 screen path, 128 calls:

| Transfer | Fused + screen | Two-pass reference + screen blit |
| --- | ---: | ---: |
| 320x200 to 320x200 | 14 | 22 |
| 160x100 to 320x200 | 13 | 23 |
| 197x113 to 320x200 | 13 | 22 |

These are short, multitasked measurements. The two-pass reference needs a
256,000-byte xRGB intermediate; the fused path does not. Do not compare these
absolute ticks with Hatari or infer physical 040/060 differences.

End-to-end four-plane Hatari: RGB32 160x100 to 320x200, diffusion, planar
packing and screen blit took 7,135 ticks fused versus 8,787 for indexed-memory
conversion followed by a simple per-pixel planar reference and blit (one call
each, warmed inverse tables). Screen readback matches. That reference is not
the unknown application's implementation or an NVDI planar benchmark.

## Integration boundary and sources

Engine/driver support and disposable installation are in scope here. The
application containing the original “TC frame → vrt_cpyfm / vr_trnfm” caller
has not been identified in Shinogi/Beads. Its repository/caller must be supplied
before an application hookup, application-level comparison or shipping rollout
can be completed. Shipping installations and upstream remotes were untouched.

References: local `atari-docs/tos.hyp/gem/vdi/raster/vr_transfer_bits.ui`, its
colour-table/bitmap documentation, and the legacy bit-15 description in
`Atari_ST_Sources/Docs/nvdiguid/NVDIGUID.TXT`. The
[NVDI 5 addendum](https://tho-otto.m68k.eu/hypview/hypview.cgi?charset=UTF-8&url=%2Fhyp%2Fnvdi_5.hyp)
is online. These establish the interface, not full implementation coverage.
