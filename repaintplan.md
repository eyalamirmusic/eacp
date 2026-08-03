# Painting only what changed

`Apps/Graphics/GUI` puts a `LOG` in `ColouredBox::paint` and it fires every
display refresh, forever, on a box that has not changed since the window opened.
Nothing is broken — that is exactly what the tier is built to do today — but it
is the wrong thing to be built to do, and the two ways out are the two halves of
the same idea: give a component's *drawing* an identity so it can be kept, and
give what that drawing *refers to* an identity so it can be shared.

# What actually happens

`Component::repaint` (`Component.cpp:259`) walks up to the host and invalidates
the whole view. `ComponentHost::render` then walks the whole tree
(`ComponentHost.cpp:449`) calling every `paint()` in it. There is no per-component
bookkeeping at all, and `Component.h:99` says so out loud:

> Marks the tree dirty. There is no partial-repaint bookkeeping: the host redraws
> the whole tree, because with the draws batched that costs less than tracking
> which rectangles changed.

The GUI example then supplies the invalidation: `AnimatedDisc` holds a
`DisplayLink` that advances two floats and calls `repaint()` at every refresh
(`Main.cpp:91`). One animating component therefore repaints the world at the
display rate, and every `paint()` in the tree runs 60 or 120 times a second.

**What that costs, in the order it costs it.** Every string is laid out twice per
frame — `Graphics::drawText` calls `text.measure` and then `text.draw`, and each
walks the string glyph by glyph through an atlas lookup. Every `ShapeInstance` is
rebuilt (130 bytes apiece) and re-uploaded. Every `setGradient` runs
`GradientRamps::rowFor`, which is a linear scan over up to 256 rows comparing
whole stop vectors (`GradientRamps.cpp`, `sameStops`). None of it is expensive
for ten components. All of it is linear in the tree, and the tier's whole claim
is that a tree can be large.

**What it already does not cost**, and this is the important half: the leaves are
retained already. A `PathShape` is rasterized only when its geometry is dirty
(`rasterizeDirtyPaths`). A `Layer` is re-rendered only when its content is dirty.
A glyph is rasterized once and keyed by `(face, codepoint, style)`. A gradient is
baked once and keyed by its stops. Clip changes are elided when the primitive is
inside both rects.

So every expensive *resource* in the tier is already keyed and cached. The one
thing with no identity is the drawing itself — the list of primitives a `paint()`
produces — and because that has to be rebuilt, everything upstream of it is
walked again to rebuild it.

# The decisions, taken before any of it is built

- **Recording is not optional.** `paint()` always records into a list and the
  frame always replays lists. There is no "cached path" and "live path" to
  diverge, and the replay code is exercised by every frame of every app rather
  than only by the frames where something was clean.
- **A list is recorded in the component's own space.** Replay applies the origin.
  A component that moves is therefore not a component that repaints, which is
  what makes scrolling and dragging cheap and is the whole difference from
  recording in surface coordinates.
- **A component is the unit of invalidation.** There is no partial repaint
  *within* one. Rung 2's damage rectangle narrows which components are visited;
  it never narrows what one of them draws.
- **Nothing is keyed by anything the caller supplies.** The word in the brief is
  "tagging", and the tag is the content: a path is keyed by its geometry, a
  gradient by its stops, a glyph by its face. A key the caller types is a key the
  caller gets wrong, and the failure mode — two different shapes sharing a mask —
  is silent and looks like corruption.
- **`Component::repaint`'s documented contract changes**, so the comment at
  `Component.h:99` is rewritten in the same commit rather than left standing as a
  description of something that is no longer true.

# Rung 1 — the draw list

The rung that answers the brief's first half: after it, `paint()` runs on a
component when that component asked for it, and on no other.

## Dirty, in two bits

`Component` gains `selfDirty` (its own drawing is stale) and `descendantDirty`
(something below it is). Both start true, a component that has never painted
being stale by definition.

```cpp
void Component::repaint()
{
    if (selfDirty)
        return;              // ancestors were marked when it was first set

    selfDirty = true;

    for (auto* p = parent; p != nullptr && !p->descendantDirty; p = p->parent)
        p->descendantDirty = true;

    if (auto* found = findHost())
        found->repaint();
}
```

The early-out holds because the host clears both bits in one walk, on its way
through: nothing ever clears `descendantDirty` while a descendant's `selfDirty`
is still set. Worth a test of its own, since it is the invariant the whole rung
rests on and it is invisible when it breaks — the symptom is a stale picture, not
a crash.

The existing internal `repaint()` calls need sorting into the two kinds, because
they are not all the same thing:

| call site | what is actually stale |
|---|---|
| `setBounds`, size changed | this component (its `paint` draws in local bounds) |
| `setBounds`, moved only | **nothing** — replay applies the new origin |
| `setVisible` | nothing; the walk stops at an invisible component |
| `addChildComponent` / `removeChildComponent` | nothing of the parent's own content — the walk reads the child list live |
| `toFront` / `toBack` | likewise: order is structure, not content |
| `ComponentHost::setFont` | every list in the tree |

Only the host invalidation is common to all of them.

## What a paint produces

```cpp
// What one paint() produced, in the component's own space.
//
// Recorded rather than issued, so that a component whose drawing has not changed
// is replayed rather than asked again -- and so that the arithmetic a paint()
// does (laying out a string, resolving a gradient, building an instance) is done
// when the drawing changes rather than when the frame does.
class DrawList
{
public:
    void clear();
    bool isEmpty() const;

    // Everything a recorded instance points at that could move underneath it:
    // the two atlas layouts, and the scale they were built at. A list whose
    // sources have moved holds uvs into texels that belong to somebody else now.
    struct Sources
    {
        std::uint32_t coverage = 0;
        std::uint32_t glyphs = 0;
        float scale = 0.f;
    };

    bool matches(const Sources& current) const;

private:
    Vector<Command> commands;                     // in issue order
    Vector<ShapeInstance> shapes;
    Vector<Text::GlyphInstance> glyphs;
    Vector<GPUWidgets::MeshVertex> vertices;
    Sources sources;
};
```

A `Command` is a tag and a range — `Shapes`, `Glyphs`, `Mesh`, `Layer`, `Clip`,
`ClipMask` — and consecutive primitives of one kind collapse into one command, so
replaying a component is a loop over a handful of commands rather than over its
primitives. The instance bytes are already in the layout the batch wants, so
replay is an append of a run, not a rebuild of it.

## Where the state machine ends up

`Graphics` becomes a recorder. It keeps the `GradientRamps` (a gradient still has
to be resolved to a row while its stops are in hand) and the `TextRenderer` (a
string still has to be laid out and measured), and it loses the `RenderPass`,
`ShapeBatch`, `MeshBatch` and `LayerRenderer` entirely — those move to the
replayer, which is the only thing that knows surface positions and global order.
The clip elision in `prepareToDraw` and the renderer alternation it counts go
with them, because both are questions about the frame rather than about the
component.

Two consequences fall out, and both are worth having on purpose:

- **The recorder needs no GPU.** `Graphics` cannot currently be constructed
  without a live `RenderPass`, which is why `Tests/UI` tests registration and
  dispatch and never tests drawing. A recorder can be built in a test, painted
  into, and its list inspected — so "this component draws these primitives" and
  "a moved component replays identically" become ordinary unit tests.
- **The paint walk moves out of the render pass.** Recording touches no pass, so
  the walk can run before `frame.beginPass`. That retires the rule `PathShape`
  currently has to document at length — that a path may not be set from
  `paint()`, because the compute dispatch is already recorded by then. Retiring
  it is *not* promised in this rung; it becomes possible in it, which is a
  different claim, and turning it on needs its own look at ordering.

There are exactly two construction sites for `Graphics` (`ComponentHost.cpp:299`
and `:446`), so the churn is contained.

## The frame

```
1. walk the tree: dirty -> paint() into its DrawList, clear the two bits
                  clean -> skip; a clean subtree is not visited at all
2. rasterize dirty paths        (compute pass, as today)
3. render dirty layers          (their own passes, as today)
4. begin the frame pass; replay every visible component's list in tree order,
   applying its origin, through the clip elision and renderer switching
5. flush
```

Step 4 is the only place instances reach a batch, so what a frame draws cannot
disagree with what a paint recorded.

## The trap in step 1

A dirty component re-recording can rasterize a glyph the atlas has never seen,
and that can grow the glyph atlas and tick its generation — which invalidates the
source rects in every *clean* list, in the middle of the frame that is relying on
them. The same is true of the coverage atlas moving under step 2.

The answer is the one `rasterizePaths` already uses for the same shape of
problem: note both generations before the walk, and if either ticked, mark every
list stale and walk once more. **At most once.** If it ticks again the frame
draws as it stands and the next one fixes it, which is the honest outcome —
better a frame with one stale glyph than a frame that never ends.

## Also in this rung, being adjacent

`Graphics::drawText` lays every string out twice — `measure` then `draw`, each
walking the string. `TextRenderer::layout` already does both jobs behind one
loop; exposing it as "emit and return the advance" halves the recording cost of
every string in the tier.

## What ships

| where | what |
|---|---|
| `UI/Component/Component.{h,cpp}` | `selfDirty` / `descendantDirty`, the two-kinds sort above, the rewritten `repaint` comment |
| `UI/Render/DrawList.{h,cpp}` | the list, its commands, and replay into the batches |
| `UI/Graphics/Graphics.{h,cpp}` | recorder: the batches and the pass come out, the ramps and the text stay, the API is untouched |
| `UI/Host/ComponentHost.{h,cpp}` | the four-step frame, the generation re-walk, `getLastPaintedComponentCount` |
| `Text/TextRenderer.{h,cpp}` | emit-into-a-vector, and one layout per drawn string instead of two |
| `Tests/UI/DrawListTests.cpp` | record/replay equivalence, replay under a translation, every row of the invalidation table |
| `Tests/UI/RepaintTests.cpp` | dirty-bit propagation, and the early-out invariant |
| `Apps/UI/ComponentTree` | "painted" beside "components" in the readout, since that is the claim |

**How it is measured:** the GUI example reports 1 painted component per frame
while the disc animates and 0 while it does not, against 10 today. ComponentTree
reports 0 painted per frame once it has settled, and its 240 components stay one
handful of draws.

# Rung 2 — damage regions

Rung 1 stops the tree being *painted*. It does not stop it being *drawn*: every
visible list is still replayed into the batch and the whole window is still
rebuilt from a clear. For ten components that is nothing. For a document-sized
tree with a blinking caret in it, it is the whole picture per blink.

The mechanism is ordinary and the reason it is sound here is not:

- `repaint()` unions the component's surface bounds into a host damage rect;
  `repaint(const Rect&)` takes a narrower one for a caret or a meter.
- The frame renders into a **persistent canvas texture** with `clear = false`,
  scissored to the damage, and then puts the canvas on the drawable. The canvas
  is needed because a swapchain drawable's previous contents are undefined —
  Metal hands out one of three from a pool — so there is nothing to preserve
  without one. `Frame::beginPass(const Texture&, ...)` already renders into an
  app-owned texture on the same frame, so no new machinery.
- The walk skips any component whose surface bounds miss the damage: no paint,
  no replay, no instances.

**Why it is sound here**: `paintComponent` reduces the clip to the component's
own bounds before calling `paint`, so a component *cannot* draw outside the
rectangle its damage was computed from. Damage rectangles are unreliable in
frameworks where paint may overflow its bounds; this tier has never allowed it.

**What it costs**: a window-sized texture, and a full-screen quad per frame.
Both are small, neither is free, and the second is paid on every frame including
the ones that changed nothing.

**Build it after measuring rung 1, not before.** If the GUI example's per-frame
cost after rung 1 is a memcpy of two hundred instances and one draw, this rung
buys battery on large trees and nothing on small ones, and that is a decision to
take with numbers rather than in advance.

# Rung 3 — identity for what a draw refers to

The brief's second half. Rung 1 gives the drawing an identity; this gives the
same treatment to the two shared resources that do not have one yet.

## Path masks, keyed by geometry

`PathShape::setPath` currently sets `dirty = true` unconditionally
(`PathShape.cpp:34`), so a `resized()` that rebuilds a path identical to the one
already there costs a full compute dispatch. And two components drawing the same
shape hold two atlas slots for the same texels — ComponentTree's 48 channel
strips draw one knob arc 48 times over.

Both fall to one change. Hash the geometry (points, verbs, fill rule, backing,
device scale) on `setPath`, and:

- hash equal to the current one → **not dirty at all**, nothing rasterizes;
- otherwise ask a `MaskCache` beside the `CoverageAtlas` for that hash: a hit
  shares the slot and bumps a refcount, a miss allocates and rasterizes as today.

Two things to be exact about. **The hash is over bit patterns, so it catches a
path rebuilt identically and not one that is geometrically equivalent** — which
is precisely the case worth catching, because a rebuilt path came out of the same
arithmetic and is bit-identical. And **a hit compares the stored path before it
shares the slot**, so a collision costs a comparison rather than drawing the
wrong shape.

Release is refcounted, and a compaction drops the cache with the shelf — which
`forgetAllocations` already exists to do, and which is the one place the two have
to agree.

## Gradient rows, keyed rather than scanned

`rowFor` scans up to 256 rows comparing whole stop vectors, per `setGradient`,
per record. Hash the stops into a key and keep a map. It is a small change and an
obviously correct one, and after rung 1 it is no longer per frame — so it is here
for tidiness and for the document case, not because it is hot.

## The rule the tier ends up with

Every expensive thing is keyed by its content and shared; everything cheap is
rebuilt. Glyphs by `(face, codepoint, style)`, gradients by stops, masks by
geometry, layers by their own dirty bit, and a component's whole drawing by its
two dirty bits. That sentence is the tier's cost model, and after this rung it is
true without exceptions.

## What is deliberately not done

**Persistent per-list GPU buffers.** `ShapeBatch::flush` uploads its instances
before every draw, and a clean component's run is byte-identical frame to frame,
so a buffer written once and re-bound would remove the upload. It is not worth it
yet and it fights the thing that makes this tier fast: a buffer per list means a
draw per list, where the whole point is that a hundred components share one
instanced draw. Revisit only if profiling names the upload, and then as a
per-frame arena rather than per-component buffers.

# What this plan will probably get wrong

- **That recording is free.** It adds a copy of every instance per frame (record,
  then replay) and gives every component four vectors. 240 components is a
  thousand small allocations, against a tier whose claim is that a component
  costs one. The fix is an arena per host with per-component ranges rather than
  vectors per component — and the honest thing is to expect to need it rather
  than to discover it.
- **That local-space recording is obviously right.** The fiddly part is the
  gradient: `GradientFill::toGradientSpace` is an inverse mapping, so replaying
  under a translation means composing it on the input side rather than adding an
  offset. Get it wrong and a gradient slides when its component moves — which is
  the kind of wrong that looks like a rendering bug rather than a bookkeeping
  one.
- **That "dirty" is one bit per component.** A container that draws something
  derived from a child — a frame sized to a child's text — is stale when the
  child changes and nothing tells it so. The whole-tree walk hides that today and
  caching exposes it. It is the classic retained-mode bug, the symptom is a stale
  picture, and there is no mechanism here that prevents it; the tier will have to
  say plainly that a component deriving its drawing from a child must repaint
  when the child does.
- **That the generation re-walk is rare.** It is rare for an interface and it is
  not obviously rare for a list scrolling new strings past a growing glyph atlas.
  The one-re-walk bound stops it being unbounded; it does not stop it being every
  frame in the bad case, and then the cache costs more than it saves.
- **That rung 2 composes with `Layer` and `PathShape`.** A dirty layer re-renders
  its whole texture whatever the damage rect says, and a coverage atlas that
  moves invalidates every list in the tree. Both are correct and neither is
  cheap, so the damage rect's promise is "small when nothing structural changed"
  rather than "small".

# Rung 1, as built

A record rather than a plan. The draw list is in, `paint()` runs where `repaint()`
was called, and the whole test suite is green at 1105.

## What shipped

| where | what |
|---|---|
| `UI/Render/DrawList.{h,cpp}` | the recorded list: shapes, glyph runs, meshes, layers and clips, in the painting component's own points; runs of one kind collapse into one command |
| `UI/Render/DrawPlayer.{h,cpp}` | replays a list at an origin under a clip, and holds everything that is a property of the frame — the renderer alternation, the scissor elision, the two counters |
| `UI/Graphics/Graphics.{h,cpp}` | a recorder: the pass and the three batches come out, the ramps and the text renderer stay, the public API is untouched |
| `UI/Component/Component.{h,cpp}` | `selfDirty` / `descendantDirty`, `needsRepaint()`, and the sort of what actually invalidates a recording from what merely needs a frame |
| `UI/Host/ComponentHost.{h,cpp}` | the two walks, `paintDirtyComponents()`, `getLastPaintedComponentCount()`, the atlas-generation re-walk, lazily built ramps |
| `UI/Render/{Layer,PathShape}.cpp` | a shape or a layer whose geometry, content or existence changed repaints the component that draws it — which nothing but the shape itself knows |
| `UI/Render/CoverageAtlas.{h,cpp}` | `generation()`, ticked where the uvs stop meaning what they did |
| `Text/TextRenderer.{h,cpp}` | `PlacedGlyph`, `layoutInto`, `drawGlyph`, `generation()`; and one layout per drawn string where there were two |
| `Tests/UI/{DrawList,Repaint}Tests.cpp` | 21 cases: what a paint leaves behind, the local-space rule, and every row of the invalidation table |
| `Apps/UI/ComponentTree` | "painted" beside "components" and "batch breaks" |

## What it actually does

The GUI example, which is where this started: **3 paints at startup and none
after**, against one per component per refresh. Its disc still animates at the
display rate — 233 paints of the disc in four seconds — and the three boxes
around it are painted when the pointer crosses them and never otherwise.

ComponentTree settles at **295 components, 8 batch breaks, 1 painted**, and the
one is the label reporting the figures.

## What the plan got right, and what it did not say

- **Recording the parameters rather than the instances was the right call.** It
  keeps the antialiasing margin and the corner clamp in `ShapeBatch` where they
  were, and it made the whole change a lift rather than a rewrite: `prepareToDraw`
  and `applyClip` moved into the player unchanged.
- **The local-space rule cost one line of matrix algebra**, not the fiddly
  composition the plan feared: a gradient's map is composed with
  `translation(-origin)` on its input side. Nine SVG documents render
  byte-identical to `main` afterwards, except one channel of one pixel on the
  gradients page, off by one, which is float rounding in exactly that
  composition.
- **The ordering trap was layers, not glyphs.** The plan worried about the glyph
  atlas ticking mid-frame, and that is handled. What actually broke the picture
  was rendering layers *after* the recording walk: `drawLayer` skips a layer with
  no texture yet, so a tree recorded first records nothing for a layer about to
  arrive, and nothing ever tells it to look again. Layers are rendered before the
  walk, as they always were.
- **A repaint from inside a paint is half-fixed, and the half matters.** The
  request now survives — the walk clears a component's bit before painting it —
  but asking the *view* for a frame from inside its own draw cycle is still
  coalesced away, so a component with no other reason to be redrawn still has to
  post the ask. `ComponentTree`'s stats label still does, and now says why.
- **What the plan did not say at all** is that this makes `Graphics` testable.
  It cannot currently be constructed without a live `RenderPass`, which is why
  `Tests/UI` has never tested drawing; a recorder needs no GPU beyond the two
  atlases, so "this component draws these primitives" is now an ordinary unit
  test. Eight of the twenty-one new cases are that.
