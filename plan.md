# Compute coverage rasterizer — rungs one and two

A GPU path rasterizer for `eacp-ui` that computes antialiasing coverage
analytically in a compute kernel, rather than approximating it with
multisampling or avoiding it with stencil-then-cover.

**Both rungs shipped.** This document is the record of them as built, kept
beside the rungs above so the next one is not designed from memory. Where a plan
turned out wrong the original claim is left in and corrected rather than quietly
deleted — those are the useful parts.

Rung one was the **cut-down version**: per-pixel direct evaluation, no tiling,
no binning. The smallest thing that produces a real artifact to judge. Rung two
put the binning in, and is written up at the end.

# Rung 1 — direct per-pixel evaluation

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

That worked: the quality question was answered in phase 2, off a screenshot,
before a line of `eacp-ui` was touched.

## What shipped

`eacp-gpuwidgets`

- `Path::setFlatness` — curves and arcs subdivide to a tolerance in path units
  instead of a fixed 24 segments.
- `PathRasterizer` — path in, coverage mask out. Owns its texture, or writes
  into a rect of one it does not own (`setTarget`).
- `CoverageKernel` — the kernel, shared by every rasterizer in the process.
- `CoverageShader` — a quad that paints a colour through a mask.

`eacp-ui`

- `CoverageAtlas` — one `computeWrite` texture every path in the interface
  rasterizes into.
- `PathShape` — a component's vector shape: the path, its coverage, its slot.
- `Graphics::fillPath` — the public API.
- `Knob` — a rotary control whose arc and pointer are a path.
- `ShapeBatch::fillMask` and a uv sub-rect on `ShapeInstance`.

`eacp-gpu`

- `ComputeProgram` forwards `var` / `ifThen` / `loop` / `breakLoop` /
  `continueLoop`, which `ShaderProgram` already did.
- `toUInt(Int)`.

Demos: `Apps/GPU/PathCoverage` (fill rules), `Apps/GPU/PathQuality` (the
three-way comparison), and `Apps/UI/ComponentTree` (48 knobs, batched).

## What already existed, and what the plan got wrong about it

Verified present before starting, and correct:

- `Frame::beginCompute()` records onto the **same command buffer** as the
  frame's render passes, ordered by the queue with no fences. The kernel feeds
  the very pass that draws its output, in one frame. Phase 1 proved it.
- `ComputePass::dispatch(width, height)` and `threadPosition()` for a 2D grid,
  with the generated bounds guard already handling the rounded-up dispatch.
- `ComputeProgram` with `InputBuffer` (read) and `WritableTexture2D` (write).
- `InputBuffer::read4` — a 4-float segment as one record read. Scalar
  underneath, which is fine and keeps the buffer layout trivial.
- The `ComputeImage` demo already proved the pattern: a kernel paints a
  texture, the next pass samples it.

Claimed present, and not:

- **"EDSL control flow: `loop`, `breakLoop`, `continueLoop`, `ifThen`, `var`."**
  True of `ShaderBuilder` and of `ShaderProgram`, and the GPU README lists them
  as part of the EDSL — but `ComputeProgram` did not forward any of them, so a
  kernel could not use the one facility a kernel needs most. Nor was there a
  `toUInt` to cross an `Int` loop counter into the `UInt` a buffer subscript
  takes. Both added.
- **"It needs no changes to `eacp-gpu`."** It needed those two.
- **"`GPUWidgets::Path` already flattens curves and arcs to polylines."** True,
  and useless at the sizes that matter: a fixed 24 segments made a 512px circle
  a 48-gon *inscribed* in the circle it was meant to be, half a pixel small
  everywhere between vertices. See phase 2.

## Still not in scope

- Tiling, binning, prefix sums, indirect dispatch. That is rung two and three.
  (Binning and a prefix sum arrived in rung two, on the CPU. Indirect dispatch
  still has not, and is not needed for it.)
- Atomics or threadgroup memory. Neither was needed and neither was added.
- Fixing conflation **between separate draws**. Coverage accumulation removes
  conflation *within* a path — which is what makes the knob's arc and pointer
  join cleanly — but two abutting widgets are two draws and will still seam.
  Pixel snapping and merging abutting geometry remain the answer there.
- Stroking. Strokes become fills via an offsetting pass. `PathTessellator`
  still has the old ribbon stroker for `PathView`; nothing strokes through
  coverage yet.

## Design

### CPU side

1. `Path` flattens to polylines, adaptively.
2. Walk the sub-paths, emitting directed segments as flat floats
   `[x0, y0, x1, y1, …]` into one `Buffer` with `Storage` usage. Closing
   segments are emitted explicitly so every sub-path is a closed loop, and
   horizontal ones are dropped — they contribute nothing to any pixel, so
   dropping them once here beats guarding against them once per pixel.
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
    (a, b) = segments.read4(i)                 // one record, four floats
    a -= pixelCorner;  b -= pixelCorner

    // The part of the segment's vertical span inside this pixel. Zero for one
    // above it, below it, or horizontal — which is the only guard the body
    // needs, a horizontal segment being also the only one that divides by zero.
    low    = clamp(min(a.y, b.y), 0, 1)
    high   = clamp(max(a.y, b.y), 0, 1)
    height = high - low

    ifThen(height > 0):
        slope = 1 / (b.y - a.y)
        xLow  = a.x + (low  - a.y) * slope * (b.x - a.x)
        xHigh = a.x + (high - a.y) * slope * (b.x - a.x)

        direction = b.y > a.y ? +height : -height
        winding  += direction * (1 - meanClampedX(xLow, xHigh))
    i += 1

total    = abs(winding)
coverage = evenOdd ? triangleWave(total) : min(total, 1)
write(target, threadPosition() + origin, float4(coverage))
```

**The coverage integral is exact, not sampled.** The plan started with `x` taken
at the midpoint of the clipped span, on the theory that the divergence was too
small to matter. Measured, it mattered: where an edge leaves through the pixel's
left or right side, the midpoint puts the whole ramp in one pixel when it
belongs across two, and a near-horizontal edge read visibly harder than
CoreGraphics'.

The exact form costs about ten instructions and no extra reads, because
`clamp(x, 0, 1)` has a closed-form antiderivative:

```
G(x)                 = clamp(x, 0, 1)² / 2 + max(x - 1, 0)
meanClampedX(from, to) = |to - from| < ε ? clamp(from, 0, 1)
                                         : (G(to) - G(from)) / (to - from)
```

`CoverageKernel::meanClampedX` is that, with the zero-run case selected rather
than branched.

### Integration with `ui::Graphics`

The kernel writes coverage into a **shared atlas texture**, not a texture per
path. Paths draw as quads sampling their own sub-rect, so several go out in one
batch instead of forcing a texture bind and a batch break each.

`ShapeBatch` therefore carries a uv sub-rect on its instance, which is the one
piece of design here that touched committed code. Alternative considered and
rejected: a second batcher for textured quads, which would reintroduce the
ordering problem the glyph queue already has.

What kept it to **one pipeline** rather than two was not in the plan and is the
nicest part of the result: texel (0,0) of the atlas is opaque, and every
*unmasked* shape's uv is a zero-sized rect inside it. A rounded rectangle
multiplies its distance-field coverage by one. Every shape pays a fetch of a
texel that is in cache for the whole frame; what it buys is that a path and the
rectangles around it never break the batch apart.

Cost model, honestly: `O(bbox pixels × segments)`. A 64×64 icon with 200
segments is 800k segment-pixel tests — nothing. A full-screen path with 10k
segments is ~20 billion — hopeless. This rung is for UI-scale paths.

*(That was rung one's cost and it is no longer the cost. Rung two replaced it
with the outline's, which is what made the full-screen case ordinary rather than
hopeless — see the last section.)*

### The ordering constraint

Not foreseen, and it shaped the public API more than anything else here:
**a compute pass cannot be open while a render pass is.** The atlas is written by
compute and sampled by the render pass, so every rasterization for a frame has
to be recorded before `beginPass` — which is before any `paint()` runs.

So `fillPath` cannot take a path and rasterize it. Either the work is scheduled
outside the frame, or a path drawn for the first time is a frame late. The API
takes a `PathShape` the component owns and sets when its geometry changes; the
host walks the tree for dirty ones at the top of the frame. Rasterizing is
therefore something a widget does when its value moves, and drawing is a quad —
which is the right shape for the cost model anyway, but it was the constraint
that picked it, not taste.

## Results

Each phase ended with something runnable and judged before the next started.

**1. Kernel in isolation** — `Apps/GPU/PathCoverage`. A self-intersecting
five-pointed star rasterized under both fill rules: solid under non-zero, a
pentagonal hole under even-odd. Interior coverage exactly saturated with no
seams where the contour crosses itself, and edges carrying 97 distinct coverage
levels. Compute-to-render on one frame works with no fence, as documented. The
generated MSL was read rather than assumed: the loop re-tests its condition
correctly and nothing is hoisted wrongly.

**2. Quality comparison** — `Apps/GPU/PathQuality`. A rounded rectangle and a
circle rendered three ways side by side: this kernel (single-sampled),
`GPUWidgets::PathView` (ear-clip + 4x MSAA), and `Graphics::Context::fillPath`
(CoreGraphics). Measured off the screenshot, over the circle's boundary:

| | distinct coverage levels | mean abs. difference from CoreGraphics | worst pixel |
|---|---|---|---|
| coverage kernel | 97 | 0.00098 | 0.21 |
| ear clip + 4x MSAA | **3** | 0.00120 | 0.37 |
| CoreGraphics | 97 | — | — |

Three levels is not a measurement artefact, it is what 4x MSAA *is*: a pixel can
only be a quarter, a half or three quarters covered. That is the finding, and it
is why analytic coverage is worth having.

Reaching it took two fixes the first cut had wrong, neither in the compute
design:

- **The exact integral**, above. The difference between "smooth" and "as smooth
  as the platform".
- **Adaptive flattening.** The 48-gon problem. `Path` now subdivides to a
  flatness tolerance — from the second-difference bound for Béziers and the
  sagitta for arcs — and `addEllipse` pushes its vertices out by half the
  sagitta so the polygon straddles the true curve instead of sitting inside it.
  This is geometry, not rasterization: `PathView` renders through the same
  `Path` and got the same improvement, which is why the MSAA column above is a
  fair comparison rather than a strawman.

The diagnosis is worth keeping: the kernel and the MSAA panel had *identical*
radial coverage profiles, both sitting ~0.15 inside CoreGraphics'. Identical
profiles meant the difference was not antialiasing at all — it was the shape.

**3. Atlas + batching.** `CoverageAtlas`, `PathRasterizer::setTarget`, the uv
sub-rect on `ShapeInstance`, and the opaque-texel trick above.

*Verified:* 48 knobs added to `ComponentTree` — 247 components and 7 batch
breaks became 295 and 8. The +1 is not the paths: with `fillPath` commented out
and the knobs still in the tree it is also 8, so it is the extra child
straddling the scroll clip. **48 vector paths cost zero batch breaks.**

**4. `ui::Graphics::fillPath`** and `UI::Knob`.

The knob's arc and pointer are one path with two contours, which is where
conflation-free filling earns its keep: they overlap, and coverage accumulating
*within* a path means the join is solid rather than seamed. Wound the same way
round, at that — under non-zero, contours that disagree subtract, and the first
cut punched a visible hole through the join, findable only by zooming in.

Atlas slots are kept between rasterizations while the mask still fits, so a knob
being dragged re-rasterizes every frame and allocates nothing. When the shelf
does run out the atlas grows, or compacts once it is at its largest; either
moves every slot, which the host answers by re-rasterizing the tree. *Verified*
by resizing the window nine times, which rebuilds all 48 masks at nine different
sizes: still correct, still 8 breaks.

**Not verified:** turning a knob by hand. Synthesising input on the development
machine lands clicks in whatever app is frontmost, so the drag path is the one
thing here that has been reasoned about rather than watched.

## Risks, as they turned out

- **Scalar-only buffer reads** — four per segment. Never showed up. The fallback
  (segments in a texture, read with `fetch`) was not needed and is still there
  if a profile ever asks for it.
- **Loop length uniform across the dispatch** — every thread walks every
  segment, including threads far outside the path. Real, and unmeasured at UI
  scale because it does not bite there. It is exactly what rung 2 fixes.
- **EDSL loop codegen quality** — read the generated MSL in phase 1 rather than
  assuming. It is fine: the condition is re-tested in the `while` header as
  documented, and the only redundancy (a `clamp` printed twice) is something any
  shader compiler CSEs.
- **Compute-to-render in one frame** — held, and is now load-bearing for every
  frame `eacp-ui` draws with a path in it.

New risk the build introduced, worth naming: **the atlas has a ceiling.** It
grows to 4096² and then compacts; a tree whose masks genuinely do not fit at
once will have some of them missing rather than wrong. That is the honest
failure mode, but it is silent, and the first thing to add if a real interface
ever approaches it is a way to notice.

# Rung 2 — CPU-side binning

**Shipped.** A path is now priced by its outline instead of by its area. Nothing
about the picture changed, which is the whole point: this rung is a rewrite of
*which segments a pixel looks at*, not of what a segment contributes.

## What shipped

`eacp-gpuwidgets`

- `PathRasterizer` bins its segments into 16×16-pixel tiles and uploads three
  buffers instead of one: the segments grouped by tile, where each tile's run
  starts, and the backdrop.
- `PathRasterizer::getSegmentCount` / `getSegmentTests` — what the dispatch is
  about to cost, settled at `setPath` so it can be read before a frame rather
  than measured during one.
- `CoverageKernel` walks one tile's run, starting from the backdrop.

`Tests/GPUWidgets/PathRasterizerTests.cpp` — the coverage the kernel writes,
against an independent CPU reference that walks every segment for every pixel.

No change to `eacp-ui`, `eacp-gpu`, or any public API but those two accessors.

## The design, and the one place the obvious version is wrong

Binning a segment into the tiles its bounding box touches is not enough, and the
reason is the thing worth writing down.

A segment does not contribute only to the pixels it passes through. In this
formulation it contributes the signed area to its right, so a segment
**anywhere to the left** of a pixel adds its full winding to it. Bin only by
overlap and every tile to the right of the outline loses everything.

The standard answer is a *backdrop*: the winding entering a tile from the left,
so a thread starts from a number instead of walking the segments that produced
it. The subtlety is what it is indexed by. Vello keeps one integer per tile,
which is only correct if every segment left of the tile spans the tile's whole
vertical extent — and to hold that, a segment that ends *inside* the band has to
go into the list of every tile to its right. On a shape whose outline runs
near-horizontally through a band — the top of any circle — that is most of the
row, which is exactly the blow-up binning was for.

So the backdrop here is **per pixel row per tile column**, not per tile:

```
backdrops[row * tilesWide + column]
```

That is `coverage pixels / tileSize` floats — a sixteenth of the mask — and it
makes the split exact with no special case. A segment is either in a tile's list
or in its backdrop, decided per tile row:

```
clip the segment to the tile row's y-band       -> [enters, leaves]
beyond = ceil(max(enters, leaves) / tileSize)   -- first column entirely right of it
list    : columns [floor(min(enters, leaves) / tileSize), beyond - 1]
backdrop: column beyond, then a prefix sum along the row carries it right
```

The two ranges are disjoint and together cover everything that contributes;
columns left of the first are entirely right of the segment and contribute zero.

Clipping to the tile row before taking the x-range is the second thing that
matters. A long diagonal binned by its bounding box lands in every tile of a
square; clipped per row it lands in the two or three per row it actually
crosses. That is the difference between binning helping and binning being a
different way to do the same work.

Two CPU passes, counting-sort style, so no tile owns a container: count the
entries, prefix-sum the counts into offsets, fill. The per-(segment, tile row)
clip is recorded on the first pass so the second does not redo the geometry.
Every buffer is kept between rasterizations, so a knob being dragged still
allocates nothing.

## Results

**The picture did not move.** `Apps/GPU/PathQuality`, measured over the circle's
boundary the same way phase 2 was, before and after:

| | distinct coverage levels | mean abs. difference from CoreGraphics | worst pixel |
|---|---|---|---|
| unbinned | 194 | 0.00900 | 0.13 |
| binned | 194 | 0.00900 | 0.13 |

(Absolute figures differ from phase 2's table because this measurement projects
all three channels rather than reading one; what matters is that it is the same
measurement on both sides.) The two renders differ in **17 pixels out of 2.9
million, by one 8-bit step each** — the CPU summing a backdrop in a different
order than the GPU loop did.

**Fill rules and seams.** `Apps/GPU/PathCoverage`: solid under non-zero,
pentagonal hole under even-odd. Over 1.69M pixels of flat region — flat found
from the coverage *gradient*, not from its value, since defining it as "the
saturated pixels" would exclude the wrongly-unsaturated block one is hunting
for — there are zero pixels that are neither empty nor full, and zero
flat-to-flat steps on any residue of the tile grid. Even-odd matters here: it
folds, so a backdrop out by one winding inverts a region instead of hiding under
`min(total, 1)`.

**The win**, from `getSegmentTests()` against the same path's unbinned cost:

| path | coverage px | segments | unbinned | binned | |
|---|---|---|---|---|---|
| knob indicator, 40pt | 71×67 | 102 | 485k | 31k | 15.6× |
| knob indicator, 96pt | 173×163 | 102 | 2.9M | 42k | 68.9× |
| PathQuality panel | 608×994 | 194 | 117M | 100k | 1167× |
| automation curve, 1200pt | 2402×389 | 818 | 764M | 375k | 2037× |
| full-window ellipse | 3203×2004 | 281 | 1.80**B** | 233k | 7747× |

The ratio grows with area because the numerator does and the denominator does
not. The last row is the case rung one called hopeless.

**Component tier.** `Apps/UI/ComponentTree` still reports 295 components and 8
batch breaks — the figures from rung one, unchanged, as they should be: binning
touches what a dispatch costs and not what a draw does.

## What this rung got wrong, and what is still unverified

- **"Recovers most of the asymptotics."** It recovers all of the ones that
  matter. What is left is the per-tile constant — three buffer reads before the
  loop — which is why the 40pt knob gains 15× and the full-window path 7747×.
  Binning is close to free at small sizes rather than a win at them.
- **The test does not go through a render pass, and that was not the plan.**
  The first cut drew the mask and read the pixels back, and every shape failed
  by ~0.19. It was the display transfer function: a coverage of 0.607 comes back
  as 0.800. Reading the mask into a buffer with a second compute pass instead
  removes the compositor from the measurement entirely, and the tolerance went
  from "some fraction I would have had to justify" to 1.5/255.
- **The tests were checked against deliberately broken binning**, because a
  test that cannot fail is not evidence. A backdrop column off by one, and a
  tile run one column short: 6 of the 7 tests fail on each.
- **Not verified: the knobs on screen.** The machine locked partway through and
  `ComponentTree` could not be screenshotted after the change. Its own reported
  figures are unchanged, and the knob indicator is checked pixel-for-pixel at
  nine sizes and five values by the new test — which covers the resize and drag
  paths better than a screenshot would — but nobody has looked at it.

## Rungs above this one

**Rung 3 — full GPU pipeline.** Needs atomics, threadgroup memory with
barriers, and indirect dispatch added to `eacp-gpu` first. Only worth it for
many complex overlapping paths — a DAW canvas of live automation curves, a map
view, a vector editor, or SVG document rendering. Note that an `SVG` module
already exists in the tree, so that last one is less hypothetical than it
sounds. Rung two moves the bar it has to clear a long way: the automation-curve
case above is now 375k segment-pixel tests, and a GPU pipeline has to beat that
*including* what its own binning stages cost.

**Not a rung, but next in line if paths get used:** stroking. Every widget that
wants an outline currently gets it from the distance field, which only knows
rounded boxes. A stroked *path* has no route through any of this yet.

**The atlas ceiling** is still the silent failure named at the end of rung one,
and rung two did not touch it.
