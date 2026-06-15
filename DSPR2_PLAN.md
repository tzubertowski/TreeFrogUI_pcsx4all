# DSPr2 acceleration plan: pcsx4all gpu_unai (SF3000 / 74Kc)

## TLDR

Hand-write MIPS DSP ASE Rev2 (DSPr2) inner loops for the gpu_unai software
rasterizer to speed up PS1 rendering on the 74Kc. Only the renderer is worth
it. Profile first, because the rasterizer may be memory-bandwidth bound, in
which case DSPr2 (an ALU speedup) buys nothing. Phase 1 is a go/no-go gate.

## Why DSPr2

- Device CPU is MIPS 74Kc + DSPr2 ASE. SIMD-within-a-register: packed 8/16-bit
  ops, saturating arithmetic, byte-by-halfword multiply, multiply-accumulate
  into 4 DSP accumulators.
- `-mdspr2` is already passed in `Makefile.sf3000`. The compiler can auto-emit
  some DSP ops, so hand intrinsics only win where it does not.
- No pcsx4all fork hand-uses DSPr2. This is original work, the only untapped
  headroom left for this device.

## Reality check (do not skip)

1. The 74Kc software rasterizer is likely **memory-bandwidth bound**:
   framebuffer writes plus texture reads dominate. DSPr2 speeds the ALU stage.
   If memory-bound, the ALU win hides behind stalls and net gain is near zero.
2. Therefore Phase 1 (profile) is a hard gate. If the profile says
   bandwidth-bound, stop and report. Do not write asm on faith.
3. Best realistic case: ~10-25% GPU-side speedup on fill-heavy scenes if
   ALU-bound. Could be ~0% if bandwidth-bound.

## Architecture precedent

`#ifdef __arm__` ALU blocks already exist in the renderer:

- `src/gpu/gpu_unai/gpu_inner.h:41`
- `src/gpu/gpu_unai/gpu_command.h`
- `src/gpu/gpu_unai/gpu_raster_sprite.h`
- also in `src/gte.c`, `src/spu/spu_pcsxrearmed/spu.c`

Pattern to follow: add `#ifdef __mips_dsp` sibling blocks alongside the ARM
ones, with the existing C/SWAR path as fallback. Isolated, reversible, safe.

## Hot path (confirmed from code)

Per-pixel functions called inside every polygon/sprite/span inner loop
(`gpuPixelSpanFn<CF>` and the raster templates):

1. `gpuBlending<>()` — `src/gpu/gpu_unai/gpu_inner_blend.h:40`. RGB555 blend
   via SWAR bit-trick (manual mask to add channels without overflow). Called
   per transparent pixel. Highest call frequency.
2. `gpuLightingTXT()` / `gpuLightingRGB()` — per-pixel gouraud color modulate,
   3-channel multiply.
3. `gpuPixelSpanFn` gouraud span fill — per-pixel interpolate plus store.

## Targets ranked by ROI

| # | Target | DSPr2 ops | Win | Effort |
|---|--------|-----------|-----|--------|
| 1 | gpuBlending (RGB555) | `addq_s.ph` / `subq_s.ph` paired-halfword saturating add/sub, replaces SWAR plus free saturation | High, most called | Med |
| 2 | Lighting / modulate | `muleu_s.ph.qbl` / `muleu_s.ph.qbr` byte-by-halfword saturating multiply | Med-high | Med |
| 3 | 2-pixel span fill | process 2 pixels/iter packed in 32 bits, amortize unpack/repack | Med | Med |
| 4 | SPU mix | `dpaq` / `maq` MAC into ac0-3 | Low, audio not the bottleneck | Med |
| 5 | GTE | DSP MAC | Low, already recompiled (`rec_gte.cpp.h`) | High |

Skip 4 and 5 unless the profile says otherwise.

## Key technical catch: RGB555 packing

RGB555 is packed in 16 bits. DSPr2 packed ops want bytes (`.qb`) or
paired-halfwords (`.ph`). Two strategies:

- **A: unpack 555 to three 16-bit lanes in a `.ph` pair, op, repack.**
  Clean saturation, but unpack/repack overhead eats the gain on single pixels.
- **B: process 2 pixels at once** (two 555 values in one 32-bit word) so the
  unpack cost amortizes. This is where DSPr2 actually beats SWAR. Favor
  span and sprite fills over single-pixel call sites.

## Phases

1. **Profile (go/no-go gate).** Instrument frame time plus per-function cycle
   counts (74Kc perf counters, or coarse `dbg_log` timing) on 2-3 real games:
   one heavy 3D textured title, one 2D sprite-heavy title. Confirm GPU fill is
   the bottleneck and which function dominates.
   Gate: if memory-bound, stop and report. DSPr2 will not help.
2. **Disasm check.** objdump the current `-mdspr2` build's `gpuBlending` and
   lighting functions. See if GCC already auto-vectorized. Only hand-write
   where it did not.
3. **Prototype gpuBlending DSPr2.** Add `#ifdef __mips_dsp` variant in
   `gpu_inner_blend.h`, strategy B (2-pixel). Keep the SWAR fallback. A/B
   benchmark the same scene.
4. **Decision.** If win >= ~10%, extend to lighting and span fills. If not,
   stop, document, revert.
5. **Validate.** Pixel-exact diff vs SWAR output (saturation edge cases
   differ), regression-test the chosen games.

## Status

### Phase 2 (disasm check): DONE, ahead of schedule

objdump of the shipping `-mdspr2` binary: **zero** DSPr2 ASE instructions in
the whole binary (checked addq.ph, subq.ph, muleu, dpaq, shll.ph, pick.qb, and
~20 more). GCC's auto-vectorizer emits nothing from the DSP ASE here. Meaning:
full headroom, but GCC will not help, every DSPr2 win must be hand-written.
Side note: 23.5k `lw` + 13.7k `sw` dominate the instruction mix, a weak static
hint consistent with the bandwidth-bound risk. Phase 1 confirms or kills it.

### Phase 1 (profiling harness): BUILT, awaiting device run

Added `src/dspr2_prof.h`, a zero-cost (when off) profiler gated on
`-DDSPR2_PROFILE`. Wired into:
- `gpulib_if.cpp` do_cmd_list: PROF_GPU_BEGIN/END around the rasterizer.
- `port.c` video_flip: PROF_FRAME_TICK per displayed frame.
- `Makefile.sf3000`: added `EXTRA_CFLAGS` hook.

Builds clean with the toolchain (globals linked, format string present). Profile
binary staged at `sdcard/cubegm/pcsx4all` (this is the profiling build, not the
shipping one, revert with a normal rebuild after testing).

Every 120 frames it logs to stderr (captured into `/mnt/sdcard/log.txt`):

```
DSPR2_PROF: frame=Xms gpu=Yms gpu_share=Z% fps=F calls/frame=C
```

Build command:
`make -f Makefile.sf3000 EXTRA_CFLAGS=-DDSPR2_PROFILE -j$(nproc)`

### Phase 3 (prototype): NOT DONE — stopped at the gate. DSPr2 abandoned.

Device profile (Spyro/THPS3, heavy textured 3D): GPU raster = ~16-19ms,
~45-53% of frame time, lag frames 38-52ms (19-28fps). GPU is the single
biggest bucket. Looked promising.

But reading the textured-polygon inner loop (`gpu_inner.h`) killed it. Per lit
textured pixel:
- texture fetch `pTxt[u0 & u0_mask]` (8bpp = double indirection `CBA_[pTxt[...]]`)
- lighting `gpuLightingTXT` = 3 `LightLUT[]` table reads (NOT arithmetic)
- blend = SWAR ALU, but only on transparent pixels (subset)
- framebuffer store

That is ~4-5 memory accesses and almost no arithmetic per pixel. The path is
memory-bound. DSPr2 accelerates ALU; there is almost no ALU here to accelerate.
The lighting is LUT reads (untouchable by DSPr2), the cost is gathers + LUT +
store. The only DSPr2-amenable ALU is the blend, which is a transparent-pixel
subset, non-batchable (per-pixel texel gather is sequential), and on 5-bit
channels needs unpack/repack that likely loses to the existing tight SWAR.

**Verdict: this is the bandwidth-bound stop condition. DSPr2 abandoned.**
The ~50% GPU frame share is memory traffic, not arithmetic DSPr2 can speed.

Device reference numbers (heavy gameplay):

| Game  | GPU share | GPU ms  | frame ms (lag) | note |
|-------|-----------|---------|----------------|------|
| Spyro | 48-53%    | 16-17ms | 33ms+          | GPU-heavier, memory-bound textured path |
| THPS3 | 30-44%    | 7-15ms  | 22-41ms        | less GPU-bound; worst frame 41ms/24fps had GPU only 11ms (27%), so ~30ms was non-GPU (CPU/GTE/SPU) |

THPS3 in particular is CPU/GTE-bound, not GPU-bound. Its lever is the R3000
recompiler / GTE, a separate investigation from graphics.

### Real levers for these games (out of DSPr2 scope)

- **DYNAREC_SKIP_DCACHE_FLUSH: DONE.** Added to `Makefile.sf3000`. ICACHE-only
  code-cache flush instead of full BCACHE syscall. Verified stable on real HW
  (no glitches across multiple games), minor speedup. Helps code-flush-heavy
  CPU-bound games like THPS3. Kept.
- **LightLUT** (GPU side): 3 dependent table reads per lit pixel. Collapsing/
  precomputing is memory-side, bigger than anything DSPr2 offered. Untried.
- Texture/CLUT access patterns, frameskip, internal-resolution tweaks. Untried.

### Instrumentation left in tree (harmless, gated)

`src/dspr2_prof.h` + the PROF hooks stay (zero-cost when `-DDSPR2_PROFILE`
unset). Makefile `EXTRA_CFLAGS` hook stays. Shipping build is profiler-free.

---

(historical) original go/no-go gate plan:

### Next: run on device (go/no-go gate)

1. Deploy staged binary to SD, boot a PS1 game.
2. Capture `gpu_share` on 2-3 titles: one heavy 3D textured (e.g. a racer or
   fighter), one 2D sprite-heavy.
3. Read it:
   - `gpu_share` high (say > ~40%) and fps below target: rasterizer is the
     bottleneck, proceed to Phase 3 (prototype gpuBlending DSPr2).
   - `gpu_share` low: CPU/GTE/SPU bound, DSPr2 on the GPU buys little. Stop.
   - For the bandwidth-vs-ALU question, Phase 3's A/B prototype is the real
     test: if hand DSPr2 blend does not move frame time, it is bandwidth-bound.
