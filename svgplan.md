# SVG through the component tier

**Rungs 1 and 2 are done, both of rung 1's questions are answered, the fix the
first answer pointed at is built, and rung 3's first feature with it.** What
follows was written before any of it was built; the sections it got wrong are
marked where they stand, and the records of what actually happened are at the end
under *Rung 1, as built*, *Rung 2, as built*, *The mesh route, as built* and
*Rung 3, gradients, as built*.

**Where that leaves the ceiling:** the atlas still cannot hold a document's masks
and no longer has to. A shape too large to be worth one is drawn as triangles
instead, chosen per shape from its own area, and the document that asked for
45.7M texels and lost 229 masks now asks for none and loses nothing. Rung 3 is
unblocked.

Rung 1's two answers, since they are the point:

- **A document does not fit in the atlas.** A 300-shape stacked drawing asks for
  45.7M texels against the atlas's 16.8M and loses 229 of its 301 masks. The
  arithmetic below was right and `plan.md`'s ceiling argument does not cover
  artwork. *Answered by the mesh route: the same document now takes no atlas
  space at all.*
- **Abutting shapes seam.** Measured, not judged: the shared edge of two
  triangles carries about a quarter of the backdrop, and against a contrasting
  one it is an unmistakable hairline. *Still open, and unaffected by the mesh
  route — those tiles are small enough to keep their masks, and a mesh seams the
  same way a mask does.*

`eacp-svg` renders a document into one native `Graphics::ShapeLayer` per shape —
CAShapeLayer on macOS, Direct2D on Windows — which is the tier `UI::Component`
exists to avoid. Moving it onto the coverage rasterizer means an SVG draws the
way the interface does: masks rasterized in one compute dispatch before the
frame, then quads out of a shared atlas in one instanced draw.

# Where the module stands now

The one section that is neither a plan nor a record: what is true today, so
nothing has to be reconstructed from the three that follow. **Everything below
about `SVGComponent` is the component tier; `SVGBuilder` is the native one and
has moved only where the parse layer beneath it moved.**

| file | what it is now |
|---|---|
| `XMLParser`, `SVGElement`, `NumberReader` | markup to a tag tree. `NumberReader::readFlag` was added for arcs |
| `SVGAttributes` | colours, transform lists as matrices, `preserveAspectRatio` and the viewBox fit, style declarations, number and point lists |
| `SVGPathParser` | `d` to a path, templated over `Graphics::Path` and `GPUWidgets::Path` and instantiated for both. Every command including `A`/`a` |
| `SVGComponent` | the component-tier builder. One component, one `PathShape` per fill and per stroke |
| `SVGBuilder` | the native builder, unchanged in what it renders |
| `SVGParser` | the two joined |

`CMakeLists.txt` links `eacp-graphics` and `eacp-ui`. `Tests/SVG` has 42 cases,
and the dash and mesh geometry are pinned in `Tests/GPUWidgets`.

Nothing in the module chooses between a mask and a mesh, and that is deliberate:
`UI::PathShape` decides from the shape's own area, so the builder is unchanged by
the thing that unblocked it and every other widget gets the same trade.

What `SVGComponent` draws: shapes and paths with every path command, `viewBox`
with its origin and `preserveAspectRatio`, transform lists as real matrices baked
into the points, inherited presentation attributes, `style=""` declarations
beating the attribute of the same name, `fill-rule`, stroke width / caps / joins
/ miter limit / opacity / dashes, `defs` / `use` / `symbol`, gradients — linear
and radial, any number of stops, both unit systems, `gradientTransform`, all
three spread methods and `href` inheritance — and text at any family and size
with real measurement.

What it does not: the focal point of a radial gradient, `clipPath` and `mask`,
group opacity as compositing, `<style>` elements and CSS selectors, filters,
`<image>`. An element asking for one of those draws without it rather than not at
all.

And the thing that used to limit all of it: **the coverage atlas cannot hold a
large document.** Measured, twice over — see *Rung 1, as built* and *What is
still open* — and answered in *The mesh route, as built*, which is the last
section.

# What the module was before any of this

*Superseded by "Where the module stands now" above; kept because the rungs are
written against it.*

The claim this plan made is that **the render-tier swap is small and the
document features are the work.** The parse layer is already renderer-agnostic
in all but two return types, and `GPUWidgets::Path` was built to mirror
`Graphics::Path` call for call. What is actually missing is a short list, and
only three items on it are machinery neither tier has. *It held: the swap was
rung 1 and the features were rung 2, and rung 2 was the larger of the two.*

The claim it also made, and this one is the reason to read further: **the atlas
ceiling analysis in `plan.md` does not cover this case.** That is the first
thing rung 1 has to find out, and it is the only thing here that could send the
design back. *It did not cover it, and it has sent the design back — which is
what "What is still open" is about.*

| file | what it is | does it move |
|---|---|---|
| `XMLParser`, `SVGElement`, `NumberReader` | markup to a tag tree | no — renderer-blind already |
| `SVGAttributes` | colours, transforms, number lists | no — returns `Graphics::Color`, which both tiers share |
| `SVGPathParser` | `d` to a path | returns `Graphics::Path`; the body does not care |
| `SVGBuilder` | the tree to native views and layers | **this is the port** |
| `SVGParser` | the two joined | trivially |

`CMakeLists.txt` links `eacp-graphics` and nothing else. There is no `Tests/SVG`.

# What is already true, and was read rather than assumed

- **The path parser needs no new call.** `parseSVGPath` uses `moveTo`, `lineTo`,
  `quadTo`, `cubicTo` and `close` — every one exists on `GPUWidgets::Path` with
  the identical signature, deliberately: *"Method names mirror
  `eacp::Graphics::Path` so the two read as siblings"* (`Path.h:19`). Retargeting
  it is a type change, not a rewrite.
- **The primitives match too.** `addRect`, `addRoundedRect`, `addEllipse` are on
  both, so `buildRect` / `buildCircle` / `buildEllipse` port line for line, and
  line / polyline / polygon are `moveTo` + `lineTo`.
- **The types are the same types.** `UI::Color`, `Point` and `Rect` are aliases
  of the `Graphics::` ones (`UI/Common.h:14`), and `GPUWidgets::Path` stores
  `Graphics::Point`. Nothing in the attribute layer has to be touched at all.
- **Fill rule is free and is a gain.** SVG's `fill-rule` is not parsed today;
  `PathShape::setPath` takes `FillRule::NonZero | EvenOdd` and the kernel
  computes both. *It was free, and `SVGComponent` reads it.*
- **Stroke style is free.** `stroke-linecap` and `stroke-linejoin` map onto
  `LineCap` / `LineJoin` / `miterLimit` exactly, all shipped.
- **A document is one dispatch.** `CoverageBatch` gathers every dirty shape in
  the frame and dispatches once, so a document of three hundred shapes costs the
  same number of dispatches as one shape does.
- **A static document costs nothing per frame.** Rasterization is triggered by
  `setPath`, so a document that is not being zoomed or animated pays its CPU once
  and then draws as quads for ever.

# What is not there, and was also read

*Three of these five were built. Each is marked; the two that stand are what
rung 3 is for.*

- **The text tier holds one font.** ~~`Text::TextRenderer` carries a single
  family and point size, and changing either rebuilds the glyph atlas
  (`TextRenderer.h:77-80`); `UI::Graphics::drawText` takes no font~~ *— still
  true of `TextRenderer` itself, and worked around rather than fixed:
  `Graphics::setTextRenderer` lets a document swap one in per run, at a batch
  break each way and a glyph atlas apiece. Option 2 below, the size-keyed atlas,
  is still the honest fix and still unbuilt.* Only `FontStyle` varies per call.
  **An SVG document mixes sizes and families in one tree, and `example.svg`
  already does** — this is the one place the port needs work in a module below
  it, and it is discussed under rung 1.
- ~~**`GPUWidgets::Path` has no transform.**~~ *Built in rung 1:
  `transformed(affine)` and `scaled`.*
- ~~**Elliptical arcs exist nowhere.**~~ *Built in rung 2, and not where this
  expected — as cubics in the parser rather than an `arcTo` on `Path`, so both
  path types got them. See "Rung 2, as built".*
- **`ShapeBatch` fills with one solid colour times coverage** (`fillMask`), so
  there is no gradient anywhere in this tier. `GPUWidgets::Gradient` bakes into
  `PathView`'s vertex-colour mesh, which is the other renderer. *Still true.*
- **The clip is a GPU scissor**, axis-aligned by construction, and
  `UI::Graphics` offers translation only for that reason (`Graphics.h:29-35`).
  `clipPath` and `mask` have no answer in the tier as it stands. *Still true.*

# Bugs in the module, which are now bugs in `SVGBuilder`

Worth naming separately, because a port that faithfully reproduces them looks
like a working port and is not one. Each is cheap to fix *while* porting and
expensive to find afterwards.

*All four are fixed in `SVGComponent` and **all four are still live in
`SVGBuilder`**, which is what the two halves of the demo differ by. Read as a
list of what the native tier does wrong, they are still current.*

- **`viewBox` origin is ignored.** `buildSVG` reads `nums[2]` and `nums[3]` only,
  and only when width or height is absent — so `viewBox="10 20 100 100"` renders
  translated by (10, 20) and nobody is told. `preserveAspectRatio` is not read at
  all.
- **Transforms do not compose.** `parseTransform` accumulates into a struct of
  translate / scale / rotate, so `translate(...) rotate(...)` and the reverse
  produce the same result, a second `translate` overwrites the first, and
  `matrix`, `skewX` and `skewY` are unhandled. The builder then applies only the
  translation, by moving a child view's bounds (`SVGBuilder.cpp:189-197`).
- **Presentation attributes do not inherit.** `applyFillAndStroke` reads
  `element.attr("fill", "black")` with no walk to the parent, so a `<g
  fill="red">` colours nothing and every child defaults to black. Most real
  documents set fill on a group.
- **Text metrics are a guess.** `textWidth = fontSize × length × 0.6`
  (`SVGBuilder.cpp:159`), which is what `text-anchor` centres against. The UI
  tier has a real `measureText`, so the port fixes this by construction.

# Rung 1 — the same document, drawn on the GPU

*Done. The record is under "Rung 1, as built"; the list below is what was
planned.*

The smallest thing that produces a real artifact to judge, following the shape
rung 1 of `plan.md` used: get a document on screen through `ComponentHost` and
look at it, before any document feature is added.

**Deliverable:** `example.svg` rendered through the component tier, beside the
existing `Apps/SVG/SVG` for comparison.

| work | where | size |
|---|---|---|
| `Path::transformed(affine)`, and `scaled` on top of it | `GPUWidgets/Path/Path.h` | small — a walk of the sub-path points |
| retarget the path parser | `SVGPathParser.cpp` | mechanical; template on the path type or switch it outright |
| an `SVGComponent` builder | new, beside `SVGBuilder` | ~300 lines, mirroring what is there |
| fill and stroke as two shapes | the builder | see below |
| per-element font | `eacp-text` | see below — the real work of this rung |
| a demo and a test target | `Apps/UI`, `Tests/SVG` | the module has no tests at all today |

## The font is the awkward one

`example.svg`'s four lines already need it: the document asks for
`font-size="18"` and the host is at 13. The options, in the order they should be
considered:

1. **A `TextRenderer` per (family, size) in the document.** ~~No change below the
   SVG module.~~ *Wrong: `UI::Graphics` holds the host's renderer by reference and
   has no way to draw or measure through another, so this needs
   `Graphics::setTextRenderer` first. Small, but below the module.* Costs an
   atlas apiece and a batch break apiece, since each is its
   own texture — a document with six text sizes is six breaks, which is
   acceptable for text-light artwork and bad for a chart.
2. **A size-keyed glyph atlas in `eacp-text`.** The honest fix: the atlas already
   packs by shelf and the rasterizer already takes a size, so what changes is
   that the key is (glyph, size, style) rather than (glyph, style), and
   `draw`/`measure` take a size. One atlas, no extra breaks, and every other
   consumer of the module gets it.
3. **Text as paths.** Glyph outlines through the coverage rasterizer, which is
   where SVG text-on-a-path eventually has to go anyway. Largest, and out of
   scope here.

Recommendation is (1) for rung 1 — it is entirely inside `eacp-svg` and keeps
the rung small — with (2) named as the thing to do the moment a document is
text-heavy enough to notice. Nothing in the builder should encode which.

## Two shapes per element

`PathShape` holds a fill or a stroke, not both: `setStroke` converts the geometry
through `strokeToFill` and stores the result as the shape's path. An SVG element
with `fill` and `stroke` — the first line of `example.svg` — is therefore two
`PathShape` members, two atlas slots, two quads, drawn fill first.

That is not a workaround, it is what the tier already does, and it is the right
shape: the two masks differ, and the stroke's wants a tighter flatness than the
fill's. `strokeToFill`'s own documentation says a stroke should be built from a
path flattened about ten times tighter, and the numbers behind it are in
`plan.md`'s stroking section. **The builder must therefore build the geometry
twice**, at two tolerances, rather than sharing one `Path` between the two
shapes.

## What rung 1 deliberately does not do

Gradients, clip paths, group opacity, `defs`/`use`, CSS, filters, images. Also
not arcs: they are missing today, so a document that needs them is no worse than
it is now, and rung 1 is about the tier and not the format.

*Rung 2 did arcs, `defs`/`use` and the style attribute. Gradients, clip paths,
group opacity, CSS selectors, filters and images are still undone and are
rung 3.*

# The two questions rung 1 exists to answer

*Both answered. The measurements are under "Rung 1, as built" at the end; the
reasoning below is what was expected of them, and it held.*

## Does a document fit in the atlas

`plan.md` explains why the ceiling survived three rungs (`plan.md:1213`): a mask
is the size of the shape on screen, so everything visible at once cannot exceed
the window's own area — a 1100×720 window at 2x is 3.2M texels against the
atlas's 16.8M, and *"a window packed solid with vector art is a fifth of the
atlas."*

**That reasoning assumes shapes tile. Artwork stacks.** A drawing is a background
covering the whole document, then shapes on top of it, then shapes on top of
those, each carrying its own full bounding-box mask. The sum of the bounding
boxes is not bounded by the window area and can exceed it many times over.

Arithmetic, not measurement: a 1000×1000 document at 2x with 300 shapes
averaging a third of the document across is 300 masks of ~666² — **133M texels
against an atlas of 16.8M.** That drops shapes, visibly, and says so through
`getLastDroppedPathCount`.

So the first thing to do after the first document renders is put a real one — an
illustration, not `example.svg` — in front of it and read the fill fraction and
the dropped count. What the answer changes:

- **If a typical document fits**, nothing here needs designing and the two atlas
  items still listed as wrong in `plan.md` (nothing gives space back on destroy;
  a dropped shape does not retry itself) become ordinary follow-ups.
- **If it does not**, the options are worth naming now and choosing later, on
  the measurement: give space back on destroy and retry on drop, which is the
  cheap half; more than one atlas, which `plan.md:1438` says a batch cannot have
  today; or routing large-area shapes through `PathView`'s ear-clip mesh, which
  needs no mask at all and costs the quality difference measured in rung 1's
  phase 2 — 3 coverage levels against 97.

Nothing about the port depends on the answer, which is why it is rung 1's
question rather than a prerequisite for it.

## Do abutting shapes seam

Never in scope, since rung 1 of `plan.md`: coverage accumulates *within* a path
and not between draws, so two abutting shapes are two draws with two antialiased
edges, and the seam between them shows the background. Widgets rarely abut and
artwork does it constantly — every shared border in a map, a chart or a logo.

This is a judgement to make from a screenshot rather than a number, the way the
quality question was answered in rung 1's phase 2. It may well be invisible at
2x on real artwork, in which case it should be written down as such and left
alone. If it is not, the answer is not in this module: it is either pixel
snapping, merging abutting geometry before rasterization, or drawing a document
as one path per colour.

# Rung 2 — the document

*Done. What was built and where this was wrong is under "Rung 2, as built" at the
end; the list below is what was planned.*

Everything here is inside `eacp-svg` and needs nothing new below it. Roughly in
the order a real document notices:

- **`viewBox` properly** — origin, size, and `preserveAspectRatio` — replacing
  the current stretch-to-fit, which is a fair default and not what the format
  says.
- **A real affine.** `parseTransform` returns a matrix, composed left to right,
  handling `matrix`, `skewX` and `skewY`; the builder composes down the tree and
  **bakes the result into the path points** through `Path::transformed`. That is
  the tier's answer to rotation, and it is a better one than the current
  translate-only child bounds: a polyline transforms exactly, and the scissor
  clip never has to express a rotated region.
- **Inherited presentation attributes** — a context struct carried down the walk
  holding fill, stroke, widths, opacity and font, each overridden where an
  element says so.
- **`fill-rule`**, which the rasterizer already has.
- **Arcs**, as `Path::arcTo` plus the endpoint-to-centre conversion in the
  parser. `segmentsForArc` already exists for the flattening.
- **`defs` / `use` / `symbol`**, which is a lookup table and an instantiation —
  and where a shared `PathShape` would be tempting and wrong, since each use
  site has its own transform and therefore its own mask.
- **`style="..."` attributes**, the same properties by another spelling. Real
  CSS selectors are a different project and should be said to be out of scope.
- **Dashing**, which `plan.md` names as genuinely absent: a different operation,
  cutting the polyline before stroking it, and nothing in `strokeToFill`
  anticipates it.

# Rung 3 — what needs new machinery

- **Gradients.** `ShapeBatch::fillMask` multiplies one colour by coverage. The
  cheapest honest extension is a second instance field — a gradient axis and two
  colours evaluated per fragment — which keeps one pipeline and one batch, and
  covers two-stop linear gradients, which is most of what documents use. Beyond
  that (many stops, radial, spreads) is a stop-table texture and a second
  pipeline, and it should not be built until a document needs it.
- **Group opacity.** Per-element alpha is just colour alpha and already works;
  compositing a subtree and fading it as a unit needs render-to-texture and is a
  different feature wearing the same attribute name.
- **`clipPath` and `mask`.** Worth noting that the tier is closer to this than it
  looks: a shape is *already* a colour multiplied by a mask sampled from the
  atlas, so a clip is a second multiply. Composing the clip into the shape's own
  coverage at rasterization time — one more mask read in the coverage kernel —
  would need no new pipeline and no stencil, which is a better shape than the
  stencil work `Graphics.h` assumes. Unbuilt and uncosted; named here because it
  is the design worth trying first.
- **`<image>`**, which is a texture rather than a path and belongs with the
  sprite renderer.
- **Filters.** Out of scope, and should stay out.

# What this plan will probably get wrong

In the spirit of the document beside it, the predictions most likely to be
corrected by contact. *All four have now met it, and the scoreboard is three
right and one wrong:*

- **That the port is the small part.** It is small in lines. The font problem was
  found by reading `TextRenderer.h` after claiming text ported straight across,
  and there may be a second one of those in the builder's contact with
  `ComponentHost`. *Right, and the second one existed: `UI::Graphics` had no way
  to draw through a renderer other than the host's.*
- **That the atlas holds a real document.** The arithmetic above says it does
  not, and the arithmetic is crude — it assumes no shape is small, which no real
  drawing obeys. *Right. Measured at 45.7M texels against 16.8M, with 229 masks
  dropped, and rung 2's `use` gave it a second route in.*
- **That two shapes per element is free.** It doubles the mask count for every
  stroked-and-filled element, against a ceiling that is already the open question.
  *Right that it doubles, wrong that it matters most: stacking is what breaks the
  budget.*
- **That baking transforms into points is the whole answer.** It is exact for
  geometry and says nothing about stroke: a non-uniform scale should stroke an
  ellipse's pen, and `strokeToFill` assumes a round one in path units — which
  `plan.md` already lists as not done. A document that scales a stroked group
  will be wrong in a way this plan does not fix. *Wrong. Stroking in the
  document's units and transforming the region turns the round pen into the
  ellipse it should be, for free.*

# The order, and what each rung buys

1. ~~**Rung 1**~~ *done.* Puts a document on the GPU and answers the two
   questions above. Nothing after it should be designed before those answers
   exist.
2. ~~**Rung 2**~~ *done.* Makes it render documents rather than one document.
   ~~Every item is inside `eacp-svg`.~~ *All but dashing, which is a path
   operation and shipped in `GPUWidgets`.*
3. **Rung 3** is the three features that need work below the module, in the order
   documents actually miss them: gradients, then clipping, then group opacity.
   *Gradients are done — see "Rung 3, gradients, as built". Clipping and group
   opacity are not.*

One decision to make at the top of rung 1 and not later: whether the native
`SVGView` stays. Keeping both means one parse layer and two builders, which is
cheap and lets the two be compared on screen — which is exactly what rung 1's
quality question needs. Recommendation is to keep it through rung 1 and delete it
once the component tier renders the corpus at least as well.

*Where that stands: the component tier now renders strictly more than the native
one, so the stated condition is met and `SVGView` could go. It has not, because
the comparison is still earning its keep — the demo's two halves are how every
rung 2 feature was checked, and the four bugs listed above are still visible in
the left one. The case for deleting it gets stronger the moment nobody is reading
that window.*

Worth saying plainly, since it is the argument for doing any of this: a static
document does not need rung 3 of `plan.md` at all — it rasterizes once. What
needs it is a document being zoomed or animated, where every shape re-rasterizes
every frame, and that is precisely the workload the GPU binner made affordable.
The SVG module is the first real user of that work.

# Rung 1, as built

Everything from here down is a record rather than a plan. Measured on Windows,
in a 1180×660 window at a backing scale of 1.5, through `Apps/UI/SVGDocument` —
which draws the same markup twice in one window, native shape layers on the left
and the component tier on the right, and cycles documents on a click or on
`argv[1]`.

## Does a document fit in the atlas

**No, and not by a small margin.** The demo reports what a document asks the
atlas for against what the atlas holds, so the figure is read rather than
inferred:

| document | masks | asks | atlas | reserved | dropped |
|---|---|---|---|---|---|
| Badge — the old `example.svg` | 3 | 2.3M | 16.8M | 13% | 0 |
| Features — groups, transforms, curves, 3 text sizes | 11 | 2.1M | 4.2M | 51% | 0 |
| Tiles — 384 abutting triangles | 384 | 2.7M | 4.2M | 67% | 0 |
| Stacked — 300 large circles over a background | 301 | **45.7M** | 16.8M | 71% | **229** |

The prediction above guessed 133M for a slightly harsher document; the measured
figure for 300 shapes averaging a fifth of the document across is 45.7M. Same
conclusion, and the mechanism is exactly the one named: **the tiled document and
the stacked one are the same size on screen and differ by seventeen times in what
they ask for.** 384 shapes that tile cost 2.7M; 301 that stack cost 45.7M. Mask
area follows the shapes and not the window, so nothing about the window bounds
it.

Note the shape of the failure, because it is the good kind. 71% of the atlas is
reserved and 229 masks are missing — the atlas is not full, it is *fragmented
past the point where anything more fits*, and the shapes that did not fit are
absent rather than wrong. `getDroppedShapeCount` says so, and the demo prints it.

What this makes of the options the plan listed:

- **Give space back on destroy, and retry a dropped shape.** Still the cheap
  half, and now clearly not sufficient on its own: nothing is being destroyed in
  the stacked case, so there is no space to give back.
- **More than one atlas.** Would raise the ceiling by a factor, which against a
  17× spread between document kinds only moves where the cliff is.
- **Route large-area shapes through `PathView`'s ear-clip mesh.** This is the one
  the measurement points at. The masks that break the budget are precisely the
  large ones — a mesh needs no mask at all, and its cost is the quality
  difference, which is a *per-shape* trade a builder can make from the shape's
  own area. A background rect does not need 97 coverage levels.

That is the first thing rung 2 should decide, and it is a decision with a number
behind it now.

## Do abutting shapes seam

**Yes. Measured at roughly a quarter of the backdrop, one device pixel wide.**

The Tiles document is a grid of triangles sharing exact vertices. Rendered over
white the shared edges read as a plausible blend and are easy to talk yourself
out of; the test that settles it is to render the same document over a
contrasting backdrop and compare the same pixel.

At one purple/blue shared edge, the transition pixel is `(124, 161, 218)` over
white and `(124, 100, 158)` over red. The red channel is *identical* and green
and blue differ by 61 and 60 — which is the backdrop and nothing else, at
61/255 ≈ **24%**. An orange/purple edge gives 19%. Both sit just under the 25%
that two independent 50% coverages predict, which is the arithmetic the plan
gave and the confirmation that this is the mechanism rather than something else.

Whether it is *visible* depends on the contrast, and it is not a judgement call
at the extremes: over white, between similarly-toned fills, it is a faint
lightening you would not find without looking. Over a contrasting backdrop it is
an obvious hairline running the length of every shared edge. A map or a flag with
dark shapes on a light ground is the visible case, and it is a common one.

So this is not something to write down as acceptable and leave. The fixes named
in the plan stand, and the measurement adds one preference: since the bleed is
the backdrop and not white specifically, drawing a document as **one path per
colour** would remove it entirely for the flat-colour artwork that suffers most —
the abutting parts become one path, and coverage accumulates within a path with
no seam. Pixel snapping does not help a diagonal edge, which is where a map
lives.

## What the plan got wrong

- **"The font is the awkward one" was right, and its option 1 was not.** The plan
  said a `TextRenderer` per (family, size) needs "no change below the SVG
  module". It does: `UI::Graphics` holds one renderer by reference from the host
  and offers no way to draw through another, so `drawText` and — worse —
  `measureText` are locked to the host's font. The fix is small
  (`Graphics::setTextRenderer`, a batch break each way) but it is a change to
  `eacp-ui`, and it had to happen before a single line of the builder was useful.
- **The native tier had never drawn an SVG shape on Windows.** Found by putting
  the two side by side, which is the whole reason rung 1 does that.
  `SVGBuilder` never set its shape layers' bounds — a `CAShapeLayer` sizes itself
  to its path and needs none, a DirectComposition surface has to be told, and a
  zero-sized one draws nothing. Only the text appeared, because the text layer
  sets its own bounds. Two lines, and out of the plan's scope, but the comparison
  is unusable without it.
- **Transform composition was not left for rung 2.** The plan put a real affine
  there, but the builder needs a matrix to bake into the points on day one, and
  `parseTransform`'s flattened struct cannot supply one. `parseTransformMatrix`
  now handles every function of the specification — including `matrix`, `skewX`,
  `skewY` and the three-argument `rotate` — composed in written order. The old
  flattened form stays for `SVGBuilder`, which can only move a child view.
- **Stroking under a non-uniform scale is not wrong after all.** The plan's last
  prediction was that baking transforms into points "says nothing about stroke",
  and that a scaled stroked group would come out wrong. It does not, because the
  builder strokes in the document's own units and transforms the *region*: a
  non-uniform scale turns the round pen into the ellipse it should be, for free.
  What remains true is the tolerance, which is handled by building the stroke's
  geometry at a tenth of the fill's and dividing both by the transform's scale
  factor.
- **Two shapes per element is real and is not free**, as predicted. It is also
  not the thing that breaks the budget — stacking is.

## What shipped

| where | what |
|---|---|
| `GPUWidgets/Path/AffineTransform.h` | a 2D affine in SVG's own `matrix(a b c d e f)` naming; `then` composes in application order |
| `GPUWidgets/Path/Path.{h,cpp}` | `transformed(affine)` and `scaled`, with the note that they map the polyline and not the curve |
| `SVG/SVGPathParser.{h,cpp}` | templated on the path type, instantiated for both, and appending rather than returning so the caller can set flatness first |
| `SVG/SVGAttributes.{h,cpp}` | `parseTransformMatrix`, and one tokenizer shared with the old flattened form |
| `SVG/SVGComponent.{h,cpp}` | the builder: one component, one `PathShape` per fill and per stroke, presentation attributes inherited, viewBox origin honoured, text measured rather than guessed |
| `UI/Graphics/Graphics.{h,cpp}` | `setTextRenderer` / `ScopedTextRenderer`, so a document can mix fonts |
| `SVG/SVGBuilder.cpp` | the two Windows bounds fixes, so the native side draws at all |
| `Apps/UI/SVGDocument` | the two tiers side by side, four documents, and the figures above |
| `Tests/SVG` | 21 cases; the module had none |

Also done while porting, from the bug list above: `viewBox` origin, inherited
presentation attributes, real text measurement, and `text`'s `y` treated as the
baseline it is. `fill-rule`, `stroke-linecap`, `stroke-linejoin`,
`stroke-miterlimit`, `fill-opacity` and `stroke-opacity` came along free, as
predicted.

Still not done, and still rung 2: `preserveAspectRatio`, arcs, `defs`/`use`,
`style="..."`, dashing. Rung 3 is unchanged. *All five were built — the record
picks up under "Rung 2, as built" below.*

## One thing to fix early in rung 2

`clearContent` drops the document's `TextRenderer`s on every rebuild, so a live
resize rebuilds a glyph atlas per distinct text size per frame. Harmless on the
documents here — a resize re-rasterizes every mask anyway — and the first thing a
text-heavy document will notice.

# Rung 2, as built

A record again rather than a plan. Everything rung 2 listed as inside
`eacp-svg` is done, one item is not where the plan put it, and the atlas
decision the measurement pointed at is *not* done and is now the only thing
between here and rung 3.

## What shipped

| where | what |
|---|---|
| `SVG/SVGAttributes.{h,cpp}` | `parsePreserveAspectRatio` and `viewBoxTransform` — the nine alignments, `none`, and meet/slice; `parseStyleDeclarations` for the style attribute |
| `SVG/NumberReader.{h,cpp}` | `readFlag`, because an arc's two flags are single characters and not numbers |
| `SVG/SVGPathParser.cpp` | elliptical arcs: the endpoint-to-centre conversion of F.6.5, the radius correction of F.6.6, and the arc emitted as cubics |
| `SVG/SVGComponent.{h,cpp}` | the fit instead of a stretch; `style=""` beating the attribute of the same name; `defs`/`use`/`symbol` with an id map and a depth limit; `stroke-dasharray` / `stroke-dashoffset`; the font cache |
| `GPUWidgets/Path/PathStroker.{h,cpp}` | `DashPattern` and `dashPath` — the polyline cut into the pattern's on-lengths, before stroking |
| `Apps/UI/SVGDocument` | two more documents: one exercising every feature above, one whose aspect differs from its component |
| `Tests/SVG`, `Tests/GPUWidgets` | 42 cases, up from 21, and six on dashing where the geometry is readable |

## What the plan got wrong

- **Arcs are not `Path::arcTo`.** The plan put the flattening in
  `GPUWidgets::Path`, where `segmentsForArc` already sits. But the parser is
  templated over both path types, and `Graphics::Path` has no arc call and could
  not portably be given one — `CGPathAddArc` does not do elliptical arcs without
  a transform trick and Direct2D wants an `ArcSegment`. Emitting **cubics**
  instead costs one function in the parser and serves both: a quarter turn at a
  time is within about a ten-thousandth of the radius, two orders below what the
  flattening afterwards preserves, and the GPU path then subdivides them to
  whatever tolerance it was told to hold. The native tier got arcs for free, as
  real curves, which is visible in the demo — it is the only rung 2 feature both
  halves of that window draw.
- **The flags are not numbers, and this was not in the plan at all.** The grammar
  lets an arc's two flags run into what follows: `a5 5 0 0110 0` is largeArc 0,
  sweep 1, then the point (10, 0). Read with `readFloat` those characters are one
  value of 110 and every coordinate after them lands somewhere else. Every
  minifier writes them that way, so a parser without `readFlag` fails on most
  real documents while passing every hand-written test.
- **`preserveAspectRatio` is not a small addition to `viewBox`, it is a change of
  default.** The plan called the existing stretch-to-fit "a fair default and not
  what the format says". It is worse than that: the format's default is
  `xMidYMid meet`, so *every* document whose aspect differed from its component
  was being distorted, and the demo's Aspect document is three circles that the
  native tier still draws as ovals.
- **"Everything here is inside `eacp-svg`" was not quite true.** Dashing is not:
  it is a path operation, it belongs beside `strokeToFill`, and it shipped as
  `GPUWidgets::dashPath`. The plan's own bullet said as much about dashing while
  the section heading said otherwise. Small, and the same shape as rung 1's
  `Graphics::setTextRenderer` — the item that needs a module below it is the one
  the summary line forgot.
- **The font cache needed pruning, not keeping.** The plan said to stop dropping
  the renderers. Simply keeping them is wrong in the case the note was about: the
  point size is the document's times the transform's scale, so a live resize
  genuinely asks for new sizes and a cache that only grew would end a drag
  holding one renderer per frame of it. What shipped hands the old set to the
  build as a spare list, lets `findOrAddFont` claim out of it, and drops the
  rest — so the cache is exactly what the last build used.

## What is still open, and it is the same thing

*Written at the end of rung 2, and answered by the section after it.*

`defs`/`use` builds each use site's geometry again rather than sharing a mask,
which is not a shortcut: a `PathShape` holds the coverage a kernel rasterized at
one size and one place, so two uses of one symbol are two masks however identical
the markup was. A document that instantiates one shape two hundred times pays for
two hundred, and that is **the stacking case again, arriving by another route.**

So rung 2 has not moved the ceiling and has given it one more way to be hit. The
decision rung 1 measured is still the decision: route large-area shapes through
`PathView`'s ear-clip mesh, which needs no mask at all, and let the builder make
that trade per shape from the shape's own area. Nothing else in rung 2 or 3
depends on it, and everything in both is limited by it.

Still not done, and now the whole of what is left below rung 3: the mesh route,
and `<style>` elements with real selectors — which stays out of scope, as the
plan said.

# The mesh route, as built

The decision above, taken and measured. It is not in `eacp-svg` at all: the
choice belongs to `UI::PathShape`, which is what makes it available to every
widget and invisible to the builder.

## What it buys, on the documents that asked

Same window, same 1.5 backing scale, through `Apps/UI/SVGDocument`. *Asks* is
what the shapes that still take masks come to; *unmeshed* is what the document
would have asked for with no mesh route at all, which is the figure rung 1
measured.

| document | shapes | meshed | asks | unmeshed | dropped | switches |
|---|---|---|---|---|---|---|
| Badge | 3 | 2 | 1.1M | 2.2M | 0 | 3 |
| Features | 11 | 1 | 0.6M | 1.5M | 0 | 2 |
| Document features | 20 | 1 | 0.5M | 1.4M | 0 | 2 |
| Aspect | 4 | 4 | 0.0M | 0.8M | 0 | 1 |
| Tiles — 384 abutting triangles | 384 | 0 | 1.9M | 1.9M | 0 | 0 |
| Stacked — 300 large circles | 301 | **301** | **0.0M** | 43.9M | **0** | 2 |

The Stacked document is the whole point: 229 masks missing before, none now, and
the atlas is not merely coping but untouched. The Tiles document is the other
half of the same point — 384 shapes, none of them meshed, because each is small
enough that a mask is the better answer and the threshold says so.

*Switches* is the cost this adds and the only one: two renderers sharing a pass
draw in flush order rather than call order, so alternating between a masked and a
meshed shape costs a draw. A document that meshes all or none of its shapes pays
one or zero; the worst case is a document that alternates, and no document here
comes near it. `Graphics::getRendererSwitchCount` reports it because nothing else
would.

## Where the decision is made, and why there

`PathShape::Backing` is `Automatic` unless a caller says otherwise, and
`Automatic` compares the mask the shape would need against a fixed 256×256 device
pixels — a 256th of a full atlas, so a tree needs that many large shapes before
the ceiling is in view. A widget's paths are orders below it; artwork is what
meets it.

Putting it there rather than in the builder was the one design change from the
plan, and it earns itself twice: the SVG module needed no line for it, and
`Apps/UI/AtlasCeiling` had to ask for `Backing::Mask` explicitly to keep
demonstrating the ceiling — which is the clearest evidence that the default is
doing something.

## What the mesh is, and what it costs in quality

`GPUWidgets::tessellateAntialiasedFill` returns a triangle list carrying a
coverage per vertex: the contour pulled in by half a device pixel and filled
solid, ringed by a mitred band that fades to nothing over the other half. The
outline ends up in the middle of that band, which is where a mask puts its 50%
coverage too, so a shape neither grows nor shrinks by changing route.

The plan expected to pay `PathView`'s quality — 3 distinct coverage levels
against the kernel's 97 — and that turned out to be the wrong thing to expect,
in both directions. `PathView` gets its 3 levels from 4x MSAA, and the component
pass has no multisampling at all and cannot have any (`ComponentHost` explains
why: a multisampled scissor edge feathers, and the glyph pipeline is
single-sample). So the ear-clip mesh as it stood would have had *one* level —
hard edges on precisely the largest shapes in the document. The feather ring is
what replaces the MSAA, and it is analytic rather than sampled, so what it
actually costs against a mask is the difference between a linear ramp across the
flattened polyline and the exact area of each pixel the path covers. On the
Badge's circle that is not a difference you can find: the edge reads the same as
Direct2D's beside it.

## What it refuses, which is the part that had to be right

A refusal costs the atlas and is answered by the mask; a wrong acceptance is
answered by nothing and draws the wrong shape. So the tessellator is
conservative, and each refusal has a reason it could not be otherwise:

- **More than one contour.** A hole and a second blob are the same two contours
  with no fill rule to tell them apart. Which also means the fill rule never
  comes up — one simple contour fills the same way under either.
- **A contour that crosses itself.** This is the one that would have shipped
  quietly wrong. Ear clipping tests a candidate triangle against the other
  *vertices*, so a five-pointed star written as five crossing edges — which is
  how SVG documents write one — comes back fully consumed and fills as a
  pentagon. Nothing downstream would say so. A quadratic crossing test in front
  of it is what catches it, and it is why the point count is capped.
- **More points than ear clipping is worth running on**, the work growing faster
  than the count while the kernel it would replace reads segments in parallel and
  does not care.
- **Ear clipping not consuming the polygon anyway**, which is also the test that
  catches a shape with a feature thinner than its own feather: pulling the ring
  inwards folds it through itself, and the fill that came back would have a bite
  out of it.

## What this does not fix

- **A large thin stroke still takes a full mask.** `strokeToFill` emits the
  stroke as overlapping contours, so it is refused on the first rule above, and a
  ring's mask is its whole bounding box however little of it is inked. That is
  the next atlas consumer worth measuring, and the answer is probably to mesh
  from the polyline rather than from the stroked region — which is
  `tessellateStroke`, already sitting there, needing a feather and non-overlapping
  joins.
- **A meshed shape re-uploads its triangles every time the tree paints**, where a
  masked one draws a quad and re-uploads nothing. A static document paints once
  and does not care; an animated one trades a per-frame memcpy for the atlas
  space, which is the right way round but is not free.
- **Seams are unchanged.** Two abutting meshes antialias against the backdrop
  exactly as two abutting masks do.
- **Nothing clips a document to its own viewport.** Visible on the Stacked
  document, whose circles run past the letterboxed artwork to the component's
  own bounds. Older than this work and unrelated to it, but this is the document
  that makes it obvious.

Still not done below rung 3: stroke meshing, and `<style>` elements with real
selectors — which stays out of scope, as the plan said.

# Rung 3, gradients, as built

The first of rung 3's three, and the one documents miss most. It needed work
below `eacp-svg`, as the plan said it would — but not the work the plan
described.

## What shipped

| where | what |
|---|---|
| `UI/Render/Gradient.h` | `Gradient` — kind, placement, transform, spread, stops — and `GradientFill`, the resolved form a painter's state can hold without owning a stop list |
| `UI/Render/GradientRamps.{h,cpp}` | one texture, a row of 256 colours per distinct stop list, keyed by the colours alone |
| `UI/Render/GradientShader.h` | the fragment maths, written once and used by both renderers |
| `UI/Render/ShapeBatch`, `MeshBatch` | two instance fields apiece, and the ramp sampler |
| `UI/Graphics` | `setGradient` / `clearGradient`, placed and inverted once per call rather than per draw |
| `GPUWidgets/Path/AffineTransform.h` | `inverted` and `getDeterminant` |
| `SVG/SVGGradient.{h,cpp}` | `<linearGradient>`, `<radialGradient>`, `<stop>`, both unit systems, `gradientTransform`, `spreadMethod`, `href` inheritance |
| `SVG/SVGAttributes` | `parsePaintReference`, for `fill="url(#id)"` |
| `SVG/SVGComponent` | a gradient per shape, resolved against its own geometry at build time |
| `Apps/UI/SVGDocument` | a Gradients document, the first the native half cannot draw at all |
| `Tests/SVG`, `Tests/UI` | 19 more cases, 979 in the project |

## Where the plan was wrong, and it was wrong in the useful direction

- **A stop table does not need a second pipeline.** The plan said two-stop linear
  gradients as instance fields were the cheap answer and that "many stops,
  radial, spreads" meant "a stop-table texture and a second pipeline". The
  second half does not follow: an extra sampler on the *same* pipeline is what
  the coverage atlas already is, and every shape reading a ramp texel it may
  ignore is the trade `fillMask` already makes for the mask. So the ramp costs
  one more fetch and no batch break — and once it exists, arbitrary stops,
  radial and all three spreads come with it. The general answer turned out
  cheaper in instance bytes than the restricted one: a row is four floats where
  two colours were eight.
- **An axis is not general enough, and fails quietly.** The plan's "a gradient
  axis and two colours evaluated per fragment" would have been wrong for the
  commonest case in the format. `gradientUnits="objectBoundingBox"` is the
  default, so a gradient on a shape that is not square is under a non-uniform
  scale — and a linear gradient under one has bands that are no longer
  perpendicular to the line between its two ends. Transforming the endpoints
  gives a plausible picture that is not the right one. What ships instead is the
  *inverse of the placement*, a whole affine: the fragment is mapped into the
  gradient's own space, where linear is x and radial is distance from the
  origin. Two dot products, exact for both kinds under any transform, and the
  demo's top bar leans while the square beside it does not — from one
  definition, which is the proof.
- **The mesh route needed it too, and pays for it.** A gradient-filled shape is
  often a background, which is exactly what the mesh route takes. Its instance is
  a *triangle*, so the same eight floats are repeated a few hundred times per
  shape: 56 bytes a triangle became 88. That is the honest cost and it is
  written down rather than designed around — a per-shape table the fragment
  indexes would remove it, and is not worth building before a document is
  measured wanting it.

## What it does not do

- **The focal point of a radial gradient.** `fx`/`fy` are read as nothing and the
  gradient draws concentric. The map is already an affine, so this is a formula
  in `gradientFill` rather than a shape of data — it is out because no document
  here needed it.
- **Gradients on text.** Glyphs go through the text renderer, which fills from a
  colour. Unrelated machinery, and the same answer as text-as-paths.
- **Anything about the seam question**, which the mesh route did not move either.

## Rung 3's remaining two

Clipping, then group opacity — in that order, and the note in the original rung 3
section still stands as the design worth trying first for the clip: a shape is
already a colour times a mask, so composing a second mask into the coverage
kernel needs no new pipeline and no stencil.
