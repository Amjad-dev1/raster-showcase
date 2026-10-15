# Code samples

Selected files from Raster's source. Each is shown as it is in the project, apart from
comment punctuation; two are excerpts and say so at the top. They include project headers
(`PixelBuffer`, `RectI`, math helpers) that are not part of this repository, so read them
rather than build them.

The comments are deliberately long. They record *why* the code is the way it is, usually
because a simpler version was tried first and failed in a specific, measurable way.

---

### [`gpu-brush-shader/brush_dab.hlsl`](gpu-brush-shader/brush_dab.hlsl)
The pixel shader that stamps every dab of a GPU stroke. A whole dab (centre, radius,
roundness, angle, hardness, colour, grain placement) rides in its vertices, so a stroke
segment is a single draw call with no constant-buffer traffic.

Look at `SampleCoverage`: texture taps are done by hand with `Load` instead of the
hardware sampler, so the GPU result matches the CPU renderer's float bilinear exactly.

### [`cpu-dab-renderer/`](cpu-dab-renderer)
The CPU renderer for brushes that must read the canvas (wet mixing, blend modes, alpha lock).

- `StampRows` solves each row's covered span of the rotated ellipse as a quadratic,
  instead of testing every pixel of the bounding box.
- `RenderBatch` splits a batch of dabs into horizontal bands and lets each thread stamp
  *every* dab in order, clipped to its own rows. Because a write depends only on the pixel
  being written, the output is bit-identical to one thread, which the tests check.

### [`undo-snapshots/StrokeSnapshot.h`](undo-snapshots/StrokeSnapshot.h)
Undo for a brush stroke without copying the layer. The layer is divided into 128 px tiles,
and a tile is copied the first time a dab is about to write into it. `Restore` lets the
brush engine put a region back mid-stroke, which is what the retro taper needs.

### [`stroke-stabilizer/`](stroke-stabilizer)
Line smoothing as a one-pole filter whose coefficient is corrected for the time between
samples, so a setting means the same thing on a 133 Hz tablet, a 1000 Hz pen and a mouse.
A hard leash caps how far the mark may fall behind the nib, so the lag is bounded by
construction. Fast strokes track the pen; slow, deliberate ones are smoothed.

### [`retro-taper/BrushEngine_RetroTaper.cpp`](retro-taper/BrushEngine_RetroTaper.cpp) (excerpt)
A tapered stroke has to know where it ends before it can draw its tail, and the obvious
answer, holding dabs back until they are safely behind the pen, makes the line visibly
trail the nib. Here every dab lands immediately at full width. When the stroke ends,
`RetroTaper` restores the tail region from the undo snapshot and re-stamps every dab that
reaches into it, clipped to that region, with the taper it has now earned. On the GPU the
buffer is simply cleared and the stroke replayed. Each recorded point keeps its
random-number state, so scattered and jittered brushes replay exactly.

### [`color-harmony/`](color-harmony)
Colour theory with no dependency on the UI. Two wheel models: the digital HSV wheel and
Itten's RYB pigment wheel, where blue opposes orange rather than yellow. The RYB remap is
a 24-point piecewise-linear table, not an offset, because the artist's wheel spends a third
of its circumference on red, orange and yellow where HSV spends a sixth. Nine harmony
schemes, tint/tone/shade variations, value studies and a warm-cool axis.

### [`tests/brush_engine_checks.cpp`](tests/brush_engine_checks.cpp) (excerpt)
Part of the self-test suite:

- the retro taper must produce **exactly** the same pixels as the delay line it replaced,
  across several random seeds;
- banded multithreaded rendering must be bit-identical to single-threaded, for glazed,
  blend-mode, erase and wet strokes;
- eight shipped brushes drawn on the GPU and on the CPU must match pixel for pixel,
  within 64/65535 (they measure within 25).

Each of these was verified to fail when the bug it guards against was put back.
