# Compute coverage rasterizer — first rung

A GPU path rasterizer for `eacp-ui` that computes antialiasing coverage
analytically in a compute kernel, rather than approximating it with
multisampling or avoiding it with stencil-then-cover.

This document plans the **cut-down version only**: per-pixel direct
evaluation, no tiling, no binning. It is deliberately the smallest thing that
produces a real artifact to judge, and it needs no changes to `eacp-gpu`.

## Why this shape first

The full architecture (Vello / piet-gpu) is a six-stage compute pipeline:
flatten, transform, bin, prefix-sum-allocate, coarse raster, fine raster. Three
of those stages need GPU features `eacp-gpu` does not have — atomics,
threadgroup memory with barriers, and indirect dispatch — each of which is a
non-trivial addition to a shared module.

The rasterization *math* is the easy part. So the first rung skips the
pipeline entirely and evaluates coverage per pixel, directly. It gets the
quality question answered — is analytic coverage visibly better than what we
have, and better than CoreGraphics? — before anything is spent on the
machinery that makes it scale.

## What already exists

Verified present, no work needed:

- `Frame::beginCompute()` records onto the **same command buffer** as the
  frame's render passes, ordered by the queue with no fences. The kernel can
  feed the very pass that draws its output, in one frame.
- `ComputePass::dispatch(width, height)` and `threadPosition()` for a 2D grid,
  with the generated bounds guard already handling the rounded-up dispatch.
- `ComputeProgram` with `InputBuffer` (read) and `WritableTexture2D` (write).
- `InputBuffer::operator[](const UInt&)` — dynamic indexing at an index the
  kernel computed. Reads are scalar `Float`, so a 4-float segment is four
  reads; fine, and it keeps the buffer layout trivial.
- EDSL control flow: `loop(condition, body)`, `breakLoop()`, `continueLoop()`,
  `ifThen(condition, then, else)`, and mutable `var(0.f)` locals.
- `GPUWidgets::Path` **already flattens curves and arcs to polylines** and
  exposes them via `getSubPaths()`. The CPU half of stage one is written.
- The `ComputeImage` demo already proves the exact pattern: a kernel paints a
  texture, the next pass samples it.

## Explicitly not in scope

- Tiling, binning, prefix sums, indirect dispatch. That is rung three.
- Atomics or threadgroup memory. If a design needs either, it belongs to a
  later rung, not this one.
- Fixing conflation **between separate draws**. Coverage accumulation removes
  conflation *within* a path; two abutting widgets are two draws and will
  still seam. Pixel snapping and merging abutting geometry remain the answer
  there, and are unaffected by any of this.
- Stroking. Strokes become fills via an offsetting pass; out of scope until
  fills are proven.

## Design

### CPU side

1. `GPUWidgets::Path` flattens to polylines (already done).
2. Walk the sub-paths, emitting directed segments as flat floats
   `[x0, y0, x1, y1, …]` into one `Buffer` with `Storage` usage. Closing
   segments are emitted explicitly so every sub-path is a closed loop.
3. Compute the path's bounding box in device pixels, expanded by one pixel.
4. Upload, dispatch over the bbox, draw the result.

### The kernel

One thread per pixel. Each thread walks every segment and accumulates that
segment's signed contribution to **its own pixel only** — the same trapezoid
arithmetic FreeType's cell rasterizer does, evaluated per pixel instead of
accumulated across a scanline.

Working in pixel-local coordinates where the pixel is the unit box:

```
winding = 0
i = 0
loop(i < segmentCount):
    read a = (segments[4i],   segments[4i+1])
    read b = (segments[4i+2], segments[4i+3])
    convert to pixel-local: a -= pixelCorner, b -= pixelCorner

    // Horizontal segments contribute nothing
    ifThen(a.y != b.y):
        direction = b.y > a.y ? +1 : -1
        y0 = clamp(min(a.y, b.y), 0, 1)
        y1 = clamp(max(a.y, b.y), 0, 1)
        dy = y1 - y0                      // vertical extent inside this pixel

        ifThen(dy > 0):
            t  = (0.5*(y0+y1) - a.y) / (b.y - a.y)
            x  = clamp(a.x + t*(b.x - a.x), 0, 1)
            winding += direction * dy * (1 - x)
    i += 1

coverage = fillRule == NonZero ? min(abs(winding), 1)
                               : evenOddFold(winding)
write(output, threadPosition(), float4(colour.rgb, colour.a * coverage))
```

**Accuracy note.** Sampling `x` at the midpoint of the clipped span is a very
good approximation, not the exact integral — it diverges slightly only where an
edge crosses the pixel's left or right boundary *within* the same pixel row.
Still far above any MSAA sample count. The exact form integrates the clamped
`x` over the span piecewise; it is more arithmetic in the same loop and can be
swapped in later without changing anything around it. Start approximate,
measure, decide.

### Integration with `ui::Graphics`

The kernel writes coverage into a **shared atlas texture**, not a texture per
path. Paths then draw as textured quads sampling their own sub-rect, which
means several paths can go out in one batch instead of forcing a texture bind
and a batch break each.

That does mean `ShapeBatch` needs a textured variant of its instance — a UV
sub-rect and a texture bind — which is the one piece of design here that
touches committed code. Alternative considered and rejected for now: a second
batcher for textured quads, which would reintroduce the ordering problem the
glyph queue already has.

Cost model to be honest about up front: `O(bbox pixels × segments)`. A 64×64
icon with 200 segments is 800k segment-pixel tests — nothing. A full-screen
path with 10k segments is ~20 billion — hopeless. This rung is for UI-scale
paths and the plan should not pretend otherwise.

## Phases

Each phase ends with something runnable and judged before the next starts.

**1. Kernel in isolation.** A `Apps/GPU/PathCoverage` demo: one hard-coded
path, one dispatch, sample the coverage texture full-screen. No `eacp-ui`
involvement. *Verify:* a star or donut renders with smooth edges and the
correct fill rule; even-odd and nonzero visibly differ on a self-intersecting
path.

**2. Quality comparison.** Same path rendered three ways side by side —
this kernel, `GPUWidgets::PathView` (ear-clip + MSAA), and
`Graphics::Path` through CoreGraphics into an `Image`. *Verify:* screenshot,
zoom in on a curve. This is the phase that decides whether any of the rest is
worth doing, and it is cheap to reach.

**3. Atlas + batching.** Coverage into a shared atlas, textured instance in
`ShapeBatch`, several paths in one batch. *Verify:* the `ComponentTree` demo's
batch-break count does not rise when paths are added.

**4. `ui::Graphics::fillPath`.** The public API, plus a widget that uses it
(a rotary knob with an arc indicator is the honest test — it is what the
widget set actually lacks). *Verify:* knob renders, batches, and the count
still holds.

## Risks

- **Scalar-only buffer reads.** Four `InputBuffer` reads per segment. Likely
  fine; if the loop turns out read-bound, packing segments into a texture and
  using `fetch` is the fallback.
- **Loop length is uniform across the dispatch.** Every thread walks every
  segment, including threads far outside the path. The bbox bounds this, but a
  path with a large bbox and few segments wastes work. Mitigation if needed:
  scissor the dispatch to sub-rects on the CPU — which is CPU-side binning, and
  is rung two anyway.
- **Unknown: EDSL loop codegen quality.** `loop()` re-tests its condition
  rather than hoisting (per the `ShaderBuilder` note). Worth reading the
  generated MSL once in phase 1 rather than assuming.
- **Compute-to-render in one frame** is documented as ordered without fences,
  but this is the first consumer in `eacp-ui`. Phase 1 proves it or finds out.

## Rungs above this one

Recorded so the first rung is not designed into a corner:

**Rung 2 — CPU-side binning.** Bin segments into tiles on the CPU, upload
per-tile lists and offsets, kernel reads only its own tile's list. Recovers
most of the asymptotics of the real thing and still needs **no atomics**,
because the CPU does the appending. Probably where this should stop for a UI.

**Rung 3 — full GPU pipeline.** Needs atomics, threadgroup memory with
barriers, and indirect dispatch added to `eacp-gpu` first. Only worth it for
many complex overlapping paths — a DAW canvas of live automation curves, a map
view, a vector editor, or SVG document rendering. Note that an `SVG` module
already exists in the tree, so that last one is less hypothetical than it
sounds.
