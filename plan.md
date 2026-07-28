# Compute coverage rasterizer

A GPU path rasterizer for `eacp-ui` that computes antialiasing coverage
analytically in a compute kernel, rather than approximating it with
multisampling or avoiding it with stencil-then-cover.

**Everything below has shipped.** This document is the record of it as built,
kept beside the rungs above so the next one is not designed from memory. Where a
plan turned out wrong the original claim is left in and corrected rather than
quietly deleted — those are the useful parts, and there is one in each section.

Three pieces, in the order they were built:

- **Rung 1 — direct per-pixel evaluation.** The cut-down version: no tiling, no
  binning, one thread walking every segment. The smallest thing that produces a
  real artifact to judge, and it answered the quality question before anything
  was spent on making it scale.
- **Rung 2 — CPU-side binning.** A thread walks the outline near its own pixel
  instead of the whole of it. The picture does not change; what changes is that a
  path is priced by its outline rather than by its area.
- **Stroking.** Not a rung — no new machinery, and coverage is computed exactly
  as before. It is the capability that was missing, and it falls out of the one
  property this rasterizer already had.

The last section of each says what that piece got wrong.

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
- **The knobs on screen**, which the machine locking mid-run had left as the one
  thing reasoned about rather than watched. Since looked at: 48 of them, 295
  components, 8 batch breaks, and the arc-pointer join solid under magnification
  with no seam and no hole. Worth keeping the method: the window was occluded, so
  `screencapture -l <windowID>` (id from `CGWindowListCopyWindowInfo`) got it
  without raising the window and stealing focus from whatever was in front.

## Rungs above this one

**Rung 3 — full GPU pipeline.** Needs atomics, threadgroup memory with
barriers, and indirect dispatch added to `eacp-gpu` first. Only worth it for
many complex overlapping paths — a DAW canvas of live automation curves, a map
view, a vector editor, or SVG document rendering. Note that an `SVG` module
already exists in the tree, so that last one is less hypothetical than it
sounds. Rung two moves the bar it has to clear a long way: the automation-curve
case above is now 375k segment-pixel tests, and a GPU pipeline has to beat that
*including* what its own binning stages cost.

*(That was the wrong bar, and rung 3's phase 0 measured the right one. Segment-
pixel tests are the GPU's work, and rung two cut them so far that they stopped
being what a rasterization costs — see below.)*

**Stroking** was next in line, and is done — see below.

**The atlas ceiling** is still the silent failure named at the end of rung one,
and neither rung two nor stroking touched it. It is now the only thing left on
this list that is known to be wrong rather than merely absent.

# Stroking

**Shipped.** Not a rung — it adds no machinery to the rasterizer and changes
nothing about how coverage is computed. It is the capability that was missing:
before it, the only outline a widget could draw came from the distance field,
which knows rounded boxes and nothing else.

## What shipped

`eacp-gpuwidgets`

- `StrokeStyle`, `LineCap` (butt / round / square), `LineJoin` (miter / round /
  bevel, with a miter limit).
- `strokeToFill` — a stroke as a path to fill.

`eacp-ui`

- `PathShape::setStroke`, which converts and stores. It forces non-zero rather
  than taking the rule, for the reason below.

`Tests/GPUWidgets` — `PathStrokerTests.cpp`, and `CoverageProbe.h`, which is the
mask read-back lifted out of the rung-two tests so both files use it.

Demo: `Apps/GPU/PathStroke`.

## Why there is no offset outline

Stroking properly means offsetting each side of the path, resolving the joins,
and unioning the pieces — and the union is the hard part, the one that needs
line-line intersection and self-intersection removal.

None of that is needed here. **Coverage accumulates within a path**, and under
the non-zero rule overlapping contours saturate rather than fight. So the stroke
is emitted as the pieces it is made of — a quad per segment, a join per corner,
a cap per open end — each its own closed contour, all overlapping, and the fill
does the union. The thing the rasterizer was already good at is exactly the thing
that removes the hard half of stroking.

What it costs is that **every contour must wind the same way**. Two that disagree
subtract, so a join wound backwards does not look wrong — it removes the corner
it was there to fill. That is the same trap the knob fell into in rung one, so
rather than write the rule down again, the emitter measures each contour's signed
area and reverses it if it disagrees. No call site spells a winding.

It also means a stroked path **must** be filled non-zero. Under even-odd every
overlap between two pieces would read as a hole. `setStroke` does not offer the
choice; `strokeToFill` says so loudly and cannot enforce it.

## What the plan got wrong

**"Skip the join when the corner is nearly straight."** The first cut skipped a
join when a bevel and a round join differed by less than the flattening
tolerance — which on a flattened curve is every corner, and looked like a huge
saving.

It was a misread of which gap was which. `half·(1 − cos(τ/2))` is how far a
*bevel* falls short of a *round* join, and it is tiny. Leaving the corner out
entirely leaves the whole bevel triangle empty, and that runs from the outer edge
all the way down to the vertex: narrow, but as deep as the stroke is wide. On a
circle of 84 segments it is 84 slits, and it read as spokes.

Caught by measuring coverage against distance rather than by looking: pixels
*deep inside* a ribbon of half-width 2.5 were coming back at 0.85–0.99 instead of
1.0, in a pattern that tracked the vertex spacing. The fix keeps the saving and
drops the bug — a smooth corner is **beveled** rather than skipped, three points
instead of a 23-point disc:

| | contours for an 84-segment circle | deepest unsaturated pixel |
|---|---|---|
| join skipped | 84 | 2.37 of 2.5, at 0.988 |
| bevel substituted | 168 | 0.29 of 2.5 — the antialiased band |

## Results

Against `Graphics::Context::strokePath` (CoreGraphics), identical geometry, 9pt
wide, miter join and butt cap, over the 27,729 pixels either side calls partial:

| | |
|---|---|
| distinct coverage levels | 203 (CoreGraphics 256) |
| mean abs. difference, boundary pixels | 0.0153 |
| mean abs. difference, whole panel | 0.00098 |
| total ink | −0.54% |

The panels are registered to the pixel — checked by shifting one against the
other, and ±0 in both axes is the minimum.

**Where the difference is, and it is not the stroker.** Broken out by shape, the
corners agree to 0.008 and the cubic to 0.044. That is flattening: `strokeToFill`
receives a path that is *already* a polyline, and offsetting amplifies its error
— on the outside of a bend of radius r the sagitta grows by (r + width/2)/r, and
both edges carry it. Tightening the source path's flatness by ten confirms it:

| | zigzag | cubic | rounded rect | overall |
|---|---|---|---|---|
| default flatness (0.05) | 0.0083 | 0.0442 | 0.0099 | 0.0153 |
| a tenth of it | 0.0051 | 0.0102 | 0.0065 | 0.0066 |

and pixels differing by more than 0.3 go from 263 to zero.

So **a stroke wants a tighter flatness than a fill does**, and nothing in
`strokeToFill` can supply it: by the time it sees the path, the curve is gone.
The default was tuned for fills and is left alone — it is a `Path` property, and
paying for it on every fill to serve strokes is the wrong trade. It is documented
on `strokeToFill` and on `PathShape::setStroke` instead, with these numbers.

*Verified* besides: every contour of every shape under every join and cap winds
the same way and is closed; a round-joined, round-capped stroke matches the
distance field its geometry defines, per pixel, with no pixel inside it unfilled
and none outside it filled; caps reach exactly as far as they should and a closed
sub-path ignores them; the miter limit turns a hairpin over; a zero-length
sub-path is a dot under round and square caps and nothing under butt.

*Verified by eye*: the three joins are three different corners at a 50-degree
chevron. Worth saying because the first version of that demo used a hairpin, and
all three came out as bevels — correct, since the miter limit was doing its job,
and useless as a demonstration.

## Still not done

- **Dashing.** No dash array, no phase. It is a different operation — cutting the
  polyline before stroking it — and nothing here anticipates it.
- **Variable width.** One width for the whole path.
- **Stroking with a transform.** A non-uniform scale should stroke an ellipse's
  pen, not a circle's. Everything here assumes a round pen in path units.
- **The overlap is real work.** A stroke is 2–3× the contours of the path it came
  from, and every overlapping piece is a segment every pixel in its tile tests.
  Rung two is what makes that affordable; it was not, before.

# Rung 3 — the GPU pipeline

**In progress.** Phase 0 has shipped and is what the rest is designed from.

## Phase 0 — what rung 3 actually has to beat

Rung 2 ended by naming its own successor's target: beat 375k segment-pixel
tests, including what the GPU's own binning stages cost. That was measured
before building anything, and it was the wrong target — segment-pixel tests are
the *GPU's* work, and rung 2 cut them so far that they stopped being what a
rasterization costs.

`Apps/GPU/PathBench` is the measurement: headless, no window and no compositor,
timing `setPath` (emit, bin, sum the backdrops, upload three buffers) against the
dispatch. Release build, Apple silicon. Dispatches are batched twenty to a
command buffer, because submitting each one and waiting measures the round trip
instead of the kernel — unbatched it made a 40pt knob look dearer than a 96pt
one. The empty-command-buffer floor is 0.02ms and is subtracted.

| path | coverage px | segments | seg. tests | CPU ms | GPU ms | CPU share |
|---|---|---|---|---|---|---|
| knob indicator, 40pt | 73×67 | 72 | 24k | 0.001 | 0.034 | 4% |
| knob indicator, 96pt | 177×163 | 166 | 58k | 0.004 | 0.022 | 14% |
| PathQuality panel | 514×829 | 178 | 86k | 0.014 | 0.040 | 26% |
| automation curve, 1200pt | 2402×356 | 846 | 461k | 0.049 | 0.047 | 51% |
| full-window ellipse | 3203×2004 | 281 | 233k | 0.220 | 0.147 | 60% |
| artwork, 4k segments | 1800×1786 | 4,000 | 3.4M | 0.223 | 0.081 | 73% |
| artwork, 20k segments | 1807×1807 | 20,000 | 11.0M | 0.645 | 0.131 | 83% |
| artwork, 100k segments | 1812×1821 | 99,998 | 37.3M | 2.362 | 0.188 | **93%** |

And the workload the rung is actually for — a canvas where every path moves, so
every one is re-binned and re-uploaded every frame:

| | paths | segments | CPU ms | GPU ms | CPU share |
|---|---|---|---|---|---|
| automation lanes | 32 | 27k | 1.585 | 0.767 | 67% |
| automation lanes | 128 | 108k | 6.492 | 2.306 | 74% |
| PathQuality panels | 128 | 23k | 1.855 | 1.342 | 58% |

**The GPU is no longer the rasterizer; the CPU is.** The 100k-segment artwork
does 37 million segment-pixel tests in 0.19ms, and the CPU spends 2.36ms
deciding which 37 million. A canvas of 128 live automation lanes spends 6.5ms of
a 16.6ms frame on the CPU before a single pixel is computed. So rung 3's case is
not that it computes coverage faster — it will not — it is that it takes the CPU
out of the per-frame path entirely.

## The two costs, which are not one cost

The stage split, measured by instrumenting the four stages of `setPath`
temporarily and reverting it (the numbers are here so it does not have to be
done again):

| path | emit | bin | backdrop | upload |
|---|---|---|---|---|
| PathQuality panel | 0.001 | 0.003 | **0.008** | 0.001 |
| automation curve, 1200pt | 0.006 | 0.016 | **0.022** | 0.003 |
| full-window ellipse | 0.002 | 0.020 | **0.177** | 0.021 |
| artwork, 4k segments | 0.028 | **0.097** | 0.079 | 0.013 |
| artwork, 20k segments | 0.147 | **0.367** | 0.079 | 0.020 |
| artwork, 100k segments | 0.756 | **1.411** | 0.081 | 0.044 |

This is the finding phase 0 exists for, and it was not expected: there are
**two independent CPU costs**, each of which dominates a different real case,
and moving only one leaves the other in charge.

- **Binning and emit** are priced by the outline: `O(segments × tile rows
  crossed)`. It is 92% of the 100k-segment artwork and 2% of the full-window
  ellipse.
- **The backdrop** is priced by the *area*: it is one float per pixel row per
  tile column, so `coverage pixels / 16`, zeroed and then prefix-summed along
  every row whatever the path is. It is 80% of the full-window ellipse — which
  has only 281 segments — and 3% of the 100k artwork.

The full-window ellipse is the one that makes the point. 281 segments is nothing,
binning them costs 0.020ms, and the rasterization still costs 0.220ms, because
201 tile columns × 2004 rows is 402k floats to clear, sum and upload — 1.6MB of
CPU memory traffic for a shape with an outline a knob could carry.

That is rung 2's per-pixel-row backdrop being exactly as expensive as it is
correct. It is the design decision rung 2 was proudest of — a per-tile backdrop
is a sixteenth the size but needs every segment ending inside a band filed under
every tile to its right, which is the blow-up binning existed to prevent — and it
is now the single largest CPU cost for any path that covers real area.

## What this makes rung 3

Not "port the binner to the GPU". Three things, and the middle one is the
interesting one:

1. **Flatten and emit on the GPU** — 32% of the 100k-segment artwork's CPU time
   is transforming an already-flattened polyline into the segment array.
2. **The backdrop without a per-row array.** Vello's answer is a per-tile integer
   backdrop, which rung 2 rejected for a good reason that still holds. The GPU
   version can have what the CPU one could not: a per-tile backdrop *plus* a
   per-row correction computed by the threads that need it, so the O(area) array
   never exists on either side of the bus.
3. **Bin with atomics** rather than the two counting-sort passes.

Only the third is what the plan above named. The prerequisites are unchanged —
atomics, threadgroup memory with barriers, indirect dispatch — but what they are
for has moved.

**The floor this cannot go below:** a UI-scale path is 0.001–0.014ms of CPU and
already 4–26% of its own cost. Rung 3 adds stages, dispatches and a round trip
that rung 2 does not have, so it will make a knob slower. Whatever ships has to
keep rung 2's path for small paths and pick between them, or it is a regression
for the only interface eacp actually draws today.

## Phase 1 — atomics in the EDSL

**Shipped.** The first of the three GPU features rung 3 is gated on.

### What shipped

`eacp-gpu`

- `BufferAccess::Atomic`, `AtomicBuffer`, `Uniform<AtomicBuffer>` — a storage
  buffer of unsigned integers, declared `device atomic_uint*` on Metal and
  `RWStructuredBuffer<uint>` on D3D. It binds exactly as an output does; only
  the element type differs, and only the emitted declaration knows.
- `atomicAdd(buffer, index, value)` — adds to one element, yields what it held
  before. Literal index and addend accepted, since a single shared counter is
  spelled at element zero and would otherwise be the one index a kernel could
  not write.
- `AtomicBuffer::load` — reading a counter back, which nothing else can do: the
  bits are integers, so the same buffer bound as an `InputBuffer` reads garbage.
- **`UInt` comparisons**, which did not exist. Neither the float set (constrained
  on the float scalar shape) nor the integer set covered them, so a `UInt` — the
  thread id, a buffer index, and now a reserved slot — could not be compared with
  anything without crossing into an `Int` first. The single most important guard
  in a binning kernel is whether the slot it just reserved fits in the array it
  indexes, so this was not optional.

`Tests/GPU/AtomicTests.cpp`, and both backends' generated source checked on
whichever host runs the suite — the Windows half cannot be executed here and is
the half more likely to be wrong, the two languages disagreeing about whether an
atomic add is an expression at all.

### The one thing that is not a choice

`atomicAdd` is a **statement**, not an expression, and both languages forced it:
MSL's `atomic_fetch_add_explicit` returns the old value, but HLSL's
`InterlockedAdd` writes it through an out parameter and cannot appear inside a
larger expression. Naming the result is the only shape both can print — and it
is the shape a caller wants anyway, the old value being a slot reserved for this
thread.

### What the tests had to be

A test that cannot fail is not evidence, and the obvious atomics test cannot
fail: `counter = counter + 1` across a thousand threads still lands on a
plausible number, just a smaller one, so checking the total is "about right"
passes on a broken build. What is checked instead is that the tickets handed out
are a **permutation of 0..n-1** — every one distinct, none skipped — which no
lost update can survive.

*Verified* by de-atomicising the Metal emission on purpose (a plain load, add and
store, and a non-atomic buffer type): all three tests fail. Restored, all three
pass, and the suite is 824/824.

### Two bugs it found, and only the second was mine

**A kernel that only counts was not a kernel.** `isCompute()` was keyed off
having a store, and an atomic add is not a store — so a kernel whose only output
was a counter emitted a *vertex/fragment pair* and failed to compile on a `gid`
no render stage has. Found by the histogram test, which writes nothing.

**A store did not happen where it was written.** This one predates this work
entirely and is the more serious of the two. Stores were **roots** of the compute
graph rather than statements in it — collected into a list and emitted after the
body, whatever block the `write()` call was made in. So:

```
    if ((v0 < 64u))
    {
    }
    buffer1[v0] = float(gid);       // the guard does nothing
```

and a store inside a loop ran **once, after it**, on the counter's final value —
a kernel told to write four elements wrote one, at the wrong index. Both
compiled. Both produced plausible output. Neither said anything.

Every shipped kernel writes at the top level, which is exactly why it survived —
and phase 4 cannot: a binner must guard its writes against overflow, which is
the very shape that was silently discarded. Stores are statements now
(`StatementKind::Store` / `TextureStore`), so they land in the block they were
recorded in. `Tests/GPU/StorePlacementTests.cpp` checks the values rather than
the source — the elements a kernel was told to leave alone still hold what they
held — and both tests fail against the old shape.

Fixing it broke two shipped codegen tests, which turned out to be a second
instance of the same omission: `blockUses` counted a statement's *value* as a
use but not its *index*, so once each store was its own statement the offsets of
a record looked used once instead of twice and stopped being named. The tests
were right and the emitter was wrong; `blockUses` counts subscripts now.
