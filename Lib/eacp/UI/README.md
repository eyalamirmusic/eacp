# UI

A component tier drawn on the GPU: a whole tree of `Component`s — widgets,
paths, text, images, layers — inside one `ComponentHost`, which is a single
`GPU::GPUView` and so one swapchain, one display link and one `Frame` a
refresh, whatever the tree holds. This file is the implementation side of it:
how a frame is built, and `GPUComponent`, which puts custom GPU rendering —
a `GPUView`'s `render(Frame&)` — inside that tree as an ordinary component.

## How a frame is built

`ComponentHost::render` is the whole of it, and the order of its steps is the
point, because every step has to be finished before the one that reads it
begins and nothing can begin while a pass is open.

1. **Paths.** Every `PathShape` whose geometry changed since the last frame is
   rasterized into the shared coverage atlas by one compute dispatch on the
   frame's own command buffer (`rasterizePaths`, a `GPUWidgets::CoverageBatch`
   kept across frames so a canvas whose paths all move allocates nothing). A
   walk that had to grow or compact the atlas is thrown away and run again, and
   every recording that named a uv in it is marked stale.
2. **Layers.** `renderLayers` walks the tree and renders every `Layer` whose
   content is stale into a pass of its own: an `onPaint` or `setContent` layer
   has its components recorded and played into its texture, an `onRender` layer
   is handed a pass the host opens, and a `GPUComponent`'s layer is filled by
   the component's whole `render()` through the redirected frame (below). The
   walk visits a component's children before the component's own layers, and
   siblings in tree order, so whatever a layer samples that sits below or
   before it in the tree is already this frame's. It also tracks where each
   component sits and what its ancestors' clips leave of it, exactly as the
   main pass will, which is how a `GPUComponent` the frame would not draw is
   not rendered either. A captured subtree starts again unclipped, being drawn
   into its layer rather than where it sits.
3. **Record.** `recordTree` re-runs `paint()` on every component that asked to
   be repainted, into its draw list. It comes after the layers on purpose: a
   component draws a layer as a quad of its texture, and a layer with no
   texture yet draws as nothing, so a tree recorded first would record nothing
   for a layer that was about to arrive and never look again.
4. **One main pass.** The frame's own pass opens, cleared to the background,
   and the shape, mesh and image batchers begin on it.
5. **Replay.** `playComponent` walks the tree once more and plays every
   recording, in tree order, through the batchers and the glyph renderer, each
   layer a quad drawn by `LayerRenderer` under its ancestors' clip with its
   opacity and transform applied there. The batches are drained before the
   pass closes, so the last run of quads does not land on top of the last run
   of glyphs.

Everything before step 4 is passes and dispatches on one command buffer, ordered
by the queue, so nothing needs a fence. A tree that changed nothing records
nothing: a frame with no stale component and no stale layer is step 5 alone.

## GPUComponent

`UI::GPUComponent` (`Component/GPUComponent.{h,cpp}`) is custom GPU rendering
inside the tree. A subclass overrides `render(GPU::Frame&)`, and `update` if it
animates, exactly as it would on a `GPUView`; what it draws lands in a texture
of the component's own, which the host composites in step 5 like any other
layer — clipped by its ancestors, under the siblings after it and over the ones
before it, faded and transformed through the layer, captured with a group that
is faded as one. A popup, a tooltip or a `ComboBox` list opens over it as it
would over anything else. A `GPUView` subclass ports by changing its base
class.

It is built on a `Layer` by composition — `layer.frameComponent` names the
component, which is what marks the layer as one filled by a whole redirected
frame rather than by a pass the host opens — and its `paint()` is `final` and
only draws that layer. Its layer's bounds follow the component's local bounds on
their own (`Layer::syncFrameBounds`, set by the walk while the frame is being
built, so a resize asks for no second frame).

### The redirect, and what the frame answers for

The frame `render()` is handed is not a frame of its own. It is the host's,
pointed at the component's texture for the length of the call by a
`GPU::Frame::Redirect` (`Lib/eacp/GPU/README.md`, "Redirecting a frame"). While
it is in force the calls that a `GPUView`'s code uses to find its target answer
for the component:

| Call | Answers |
| --- | --- |
| `beginPass(descriptor)` | `beginPass(texture, descriptor)`: the component's texture, its depth and stencil attached, its samples resolved into it as the pass ends |
| `pixelSize()` | the texture's size, which is exactly the component's bounds times the scale, rounded up |
| `backingScale()` | the host's scale, or the snapshot's under `renderToImage` |
| `logicalSize()` | the two above divided, so the component's own size in points |
| `drawableFormat()` | the texture's format, `BGRA8Unorm` |
| `copyDrawable(texture)` | a copy of the component's texture, which is always copyable — no `setReadableDrawable` |
| `isValid()` | the recording is open and the texture is a valid render target |

Everything else is the host frame's, unchanged: `beginCompute`, `beginPass`
on a texture or several targets, copies, mipmaps, `flush()`, the device's frame
index and its timings. All of it records onto the host's one command buffer, in
order with the path atlas, every layer and every component rendered before this
one, and ahead of the main pass that composites the result — so a compute pass
feeding a draw in one `render()` needs nothing more than it did in a `GPUView`.

The rules that come with sharing the frame:

- **No pass is open across the boundary.** The host begins the redirect with
  none open and expects none open when `render()` returns; a `RenderPass` or
  `ComputePass` scoped inside `render()` is all this asks.
- **`flush()` flushes the host's frame.** It submits everything recorded so far
  — the atlas, earlier layers, earlier components — and carries on, which is
  what a mid-frame read-back of the component's own texture needs and costs
  what it always costs (on Metal the frame's total GPU time then measures only
  the part after it). It is not something to call per component per frame.
- **Nothing presents.** The component has no drawable and no swapchain; the
  host presents once, after step 5.

### `targetChanged`

`protected: virtual void targetChanged(GPU::Frame&)` is called under the
redirect, right before `render()`, on the first render and whenever the
texture's sample count, depth, stencil or colour format is not what the last
`render()` drew into, and again after a device restore — not on a resize or a
scale change, which remake the texture at the same description. It is where a
component builds the pipelines that have to agree with the target
(`sampleCount()`, `hasDepth()`, `hasStencil()`, `colorFormat()`), so `render()`
never has to compare, and since a restore calls it too, pipelines built there
are rebuilt on the new device with nothing else to write. A ported `GPUView`
that builds its pipelines lazily in `render()` keeps working without it.
Every tile in `Apps/UI/GPUComponents` builds its pipelines here.

### On demand and continuous

On demand is the default: `render()` runs on the host's frame when the
component was repainted (`repaint()`), resized, or moved to another backing
scale. A move, `setOpacity` and `setTransform` are applied as the texture is
composited and render nothing. `renderNow()` renders the host's whole frame
immediately rather than at the next refresh, for a component driven by
something other than the clock — a camera frame arriving; where the host has
nothing to present into (no drawable yet, or its own frame already open) the
render is left to the next frame.

`setContinuous(true)` ticks off the host's one display link, not a link of its
own: each tick paces (`setMaxFps`), calls `update(FrameTime)` — whose delta
spans any ticks the pacing skipped — marks the layer stale and asks the host
for a frame. A tick re-records no `paint()` anywhere; the frame it brings is a
render of the stale layers and a replay of the recordings already held. A
continuous component removed from the tree stops, and one re-added,
reparented, or made continuous before it had a host starts again
(`ComponentHost::subtreeAttached`).

### Off screen: culled and hidden

The two are disjoint:

- **Culled** — showing, but nothing of it survives its ancestors' clips:
  scrolled out of a `Viewport`, clipped away, or of zero size.
  `wasCulledLastFrame()`.
- **Hidden** — not showing: it or an ancestor is invisible (a tab page that is
  not current), or the tree is in no host. `wasHiddenLastFrame()`.
  `wasCulledLastFrame()` is false for a hidden component.

`isOnScreen()` is false for both, and `getVisibleBounds()` is the part of the
component's local bounds its ancestors left visible in the last frame — empty
when either. `ComponentHost::getLastRenderedGPUComponentCount()`,
`getLastCulledGPUComponentCount()` and `getLastHiddenGPUComponentCount()` are
those states counted over the tree as the last frame found them, and disjoint:
rendered is a component whose `render()` ran, and one on screen with nothing new
to render is in none of the three. Hidden subtrees are still walked every frame
(`markGPUComponentsHidden`), which is what keeps the hidden count and each
component's state current rather than whatever it was when it was hidden.

What happens off screen:

- **`render()` is skipped.** The component renders the frame it comes back on
  if anything changed while it was away, which a continuous one always has.
- **`update()` keeps running** for a continuous component as long as it is in
  a host, so its state does not stall and a simulation picks up where it
  would have been. Such a tick marks the layer stale and **asks nothing of the
  host**: a grid of a hundred animated tiles with three on screen costs a
  hundred `update()`s and a frame that renders three, not a frame per
  off-screen tile. The host's animation clock keeps running while any
  continuous component is attached, on screen or not.
- **`isOnScreen()` is the early return.** An `update()` whose only work is for
  the picture — rebuilding a vertex buffer, uploading a texture — checks it and
  returns, and `getVisibleBounds()` lets one that draws a huge canvas prepare
  only the part on screen.
- **The texture is kept.** A hidden or culled component keeps its texture and
  its pipelines; nothing is released until the component is destroyed (or the
  device is lost). Showing a tab again is a composite, not a rebuild.

A component inside a captured subtree is never culled by the clips outside the
capture: it is drawn into the capture's layer, which is what is clipped.

### Opaque and transparent

Opaque is the default, as on a `GPUView`: the alpha a pass leaves is ignored and
the component composites as a solid rectangle (`Layer::opaque`, an `opaque`
uniform in `LayerRenderer` that takes the colour as it stands at full
coverage). `setTransparent(true)` composites by the texture's alpha, taking the
colour as premultiplied — what the tier's own layers already are — so a
pipeline that is not opaque blends with
`GPU::BlendMode::AlphaBlendOntoTransparent`, and a pass cleared to alpha 0
shows the components beneath. Neither is a re-render.

### Sample count, depth and stencil

`sampleCount()` defaults to **1**, where a `GPUView`'s is 4. A view is one
swapchain; a component is a texture each, and they add up: at 4 samples with a
depth buffer, a grid of 64 retina-sized tiles runs past a gigabyte, since the
multisampled colour and depth are kept beside the resolved texture
(`Lib/eacp/GPU/README.md`, "Multisampling a target"). `setSampleCount(4)` is
one call for the component that wants smooth edges. Every pipeline drawing
there sets `sampleCount = sampleCount()`, as on a view.

`setDepth` and `setStencil` are a `GPUView`'s: the stencil plane brings the
depth buffer with it, and turning depth off drops the stencil. Any of the three
remakes the texture, and `targetChanged` follows.

### Texture size

The texture is exactly the component's pixels — `ceil(bounds × scale)` — and is
remade on every change of size, shrink included, because the code filling it
sizes itself off `pixelSize()`. (A plain layer keeps a larger texture and draws
from a corner of it; a frame layer cannot.) So a live resize reallocates per
frame. The ceiling is 8192 pixels a side, against 4096 for an ordinary layer:
the texture is a display rather than a drawing, and 8192 covers a full-screen
component on a 5K panel. Over it, or where the device refuses a texture that
large (Vulkan guarantees only 4096), the component draws nothing and says so
once in the log. A zero-size component gets no texture and no `render()`.

### Frame rate

`setMaxFps(n)` caps one component: the host's clock still ticks every refresh,
and the component updates and renders on at most `n` of those a second.
`ComponentHost::setAnimationMaxFps(n)` caps the clock itself, and so every
animation in the tree with it. Both are `Threads::FramePacer`
(`Lib/eacp/Graphics/Helpers/FramePacer.h`), the divider `DisplayLink::setMaxFps`
is built on: it fires on the tick that lands closest to due, so 60 on a 120 Hz
panel is every other tick rather than 40 by float dust, carries the remainder
so an uneven cap holds as an average, and caps what it banks at half an
interval so a stall buys one prompt tick rather than a burst. On-demand renders
are never throttled.

### Input

A `GPUComponent` is an ordinary component for input: `interceptsMouseClicks` is
on by default, and it gets the `UI::MouseEvent` callbacks, keys when focused,
the wheel and magnify. `UI::MouseEvent` carries what a 3D view wants beside
`delta`: `rawDelta`, the device's own movement with no acceleration curve (what
an orbit or a mouse-look follows, in device units rather than points),
`pressure` (0 to 1, 1 where the device has none) and `scrollPhase` for wheel
and magnify gestures.

Hit testing ignores layer transforms: a component turned with `setTransform`
still takes its events in its untransformed bounds.

### Backing scale

`backingScale()` is the host's, or the snapshot's while
`renderToImage(scale)` runs. The texture is in device pixels, so a change of
scale renders again, and `onBackingScaleChanged(scale)` fires first — before
the first `render()` at the new scale, not for the very first — for a component
holding anything sized in pixels (a glyph atlas, a framebuffer of its own).

### Device loss

Windows only, as the restore is. `GPUView` has a protected virtual
`deviceRestored()`, called just before `onDeviceRestored` so a view built on
`GPUView` can rebuild itself and leave the hook to the app. `ComponentHost`
overrides it: its batchers, the coverage atlas, the glyph, ramp and image
textures and every layer's texture are made again, the whole tree is marked to
repaint, and every `GPUComponent` in the tree — hidden ones included — has its
recorded target forgotten and is told through its `onDeviceRestored`, which is
where it recreates the buffers and textures it made on the old device. A render
follows, and `targetChanged` runs before it as it did before the first, so
pipelines built there need nothing in `onDeviceRestored`; one that built
pipelines anywhere else rebuilds them in the hook.

### GPU timing

`setGpuTiming(true)` (off by default) times every pass the component's
`render()` begins — compute and texture passes included, labelled or not —
through the redirect's timing label: in `Device::lastFrameTimings()` they
appear as `GPUComponent#n`, or `GPUComponent#n/label` for a pass with a label
of its own, `n` a process-wide serial given the first time timing is turned on.
The label is `Frame::Redirect`'s fourth argument and the renaming is
`FrameTimer::beginPass`'s, under the scope `FrameTimer::setScope` sets; a
redirect with no label turns the scope off for the passes under it and the
outer one's comes back as it ends. `lastGpuMilliseconds()` is their total in
the latest timed frame that rendered the component — a frame that did not keeps
the last figure — and zero while timing is off, before any figure arrives, or
where the device cannot time passes. The figures come off the GPU once a frame
has finished there, so they arrive a frame or more late — an on-demand
component's once the host has drawn again — and the host reads them on every
walk, hidden components included, since the device keeps only the latest
frame's. The device times at most 128 passes a frame
(`GpuTimestamps::maxTimedPasses`), shared by the whole host frame, so in a grid
of many timed components the passes past that run as they would have and are
simply not timed; that is why it is off by default.

### Ordering, and sampling another component

Components render children first and siblings in tree order — the walk in step
2 — so a component can sample the texture of one rendered before it
(`other.getLayer().getTexture()`), and a parent the textures of its children.
The dependency is not tracked across components: within one component a layer
rendering again re-renders the layers after it, but a component sampling
another must repaint itself when the other does (or be continuous). The texture
is remade on a resize or a change of samples, so bind it in `render()` rather
than once.

### What it costs

- A texture of exactly its pixels, plus a multisampled twin above one sample
  and a depth buffer when asked, kept while hidden.
- Per render: its own passes (each a load and store of its target — bandwidth
  on a tiled GPU) and the redirect, which swaps a few fields on the frame.
- Per frame it is drawn in: one quad in the main pass.
- A continuous component on screen brings a host frame every tick, and a host
  frame replays the whole tree's recordings (it re-records none of them). One
  continuous tile makes the whole interface a 60 Hz replay; that is cheap for a
  tree of widgets, and `setAnimationMaxFps` is the knob where it is not.
- Off screen: its `update()` and nothing else.

### GPUView and GPUComponent side by side

| | `GPU::GPUView` | `UI::GPUComponent` |
| --- | --- | --- |
| `render(Frame&)`, `update(FrameTime)` | the view's own frame | the host's frame, redirected; `update` runs off screen too |
| Where it composites | a native view, over its parent | in the tree: clipped, ordered, faded, transformed, captured |
| `sampleCount()` default | 4 | 1 |
| `setDepth`, `setStencil` | yes | yes, the same rules |
| `setTransparent` | alpha reaches the window beneath | alpha reaches the components beneath |
| `setContinuous` | a display link of its own | the host's one display link |
| `setMaxFps` | yes (`FramePacer`) | yes (`FramePacer`), under the host's `setAnimationMaxFps` |
| `renderNow()` | renders the view now | renders the host's frame now |
| `backingScale()`, `onBackingScaleChanged` | the display's | the host's, or a snapshot's |
| `onDeviceRestored` | yes, plus the protected `deviceRestored()` | yes, after the host has rebuilt |
| `copyDrawable` | needs `setReadableDrawable` | always |
| `setFramesInFlight`, `renderFrame` | yes | the host's |
| `setOpacity`, `setTransform` | — | composite-time, not a re-render |
| `targetChanged(Frame&)` | — | before the first render, on a new target description and after a device restore |
| `isOnScreen`, `getVisibleBounds`, culled/hidden | — | yes |
| `setGpuTiming`, `lastGpuMilliseconds` | `Device::lastFrameTimings` by label | per component, every pass under its own label |
| Input | `Graphics::MouseEvent` | `UI::MouseEvent`, with `rawDelta`, `pressure`, `scrollPhase` |

### Why not a native child GPUView

A `GPUView` added as a child of the host is the obvious alternative, and it is
the wrong one everywhere, for reasons each platform states differently:

- **macOS and iOS.** A child `GPUView` is an `NSView`/`UIView` with a
  `CAMetalLayer` of its own above the host's: every widget the host draws is
  beneath it, so no popup, tooltip or `ComboBox` list can cover it, a
  `Viewport` scrolling it is not clipped, and no ancestor's opacity, transform
  or capture reaches it.
- **Windows.** A child window with its own swapchain has the airspace problem:
  it is above everything its parent draws, sibling z-order among child windows
  is fragile, and nothing the host's tree clips applies to it.
- **Linux.** Each presenting view is a `wl_subsurface` or a child xcb window
  above the host's surface — on top, unclipped by the tree, ordered by the
  window system rather than by the tree.
- **Android.** A window has one presenting surface, the activity's
  `ANativeWindow`, so a second `GPUView` cannot present at all.

And on all of them it is a swapchain, a present and a frame per view, each
paced and submitted on its own. A component is one more set of passes on the
frame that was being built anyway. GPU rendering inside a component tree goes
through `GPUComponent`; `Layer::onRender` stays the lighter tool for a single
pass the host opens and sizes, with no frame, no `update` and no clock.

### Tests and the example

`Tests/UI/GPUComponentTests.cpp` (`GPUComponent/`) covers rendering and
compositing, the frame's answers, clipping, opaque and transparent, compute
feeding a draw, multisampled depth, stencil, exact resizing, culling and the
visible bounds, hidden components keeping their texture, captures, scale
changes, continuous mode on and off screen, reparenting, `setMaxFps`,
`renderNow`, device restore (and that it is a target change), `targetChanged`
firing only when the target does, the disjoint rendered, culled and hidden
counts, GPU timing off and on, `copyDrawable`, tree-order sampling and the new
`MouseEvent` fields; the GPU cases self-skip without a device and the
multisampled ones without 4-sample support, and the targets are small enough
to run on lavapipe. `Tests/GPU/FrameRedirectTests.cpp` (`FrameRedirect/`) is
the redirect on its own, and `Tests/Graphics/FramePacerTests.cpp`
(`FramePacer/`) the divider.

`Apps/UI/GPUComponents` is the stress test: six kinds of tile — a lit,
depth-tested mesh orbited with `rawDelta`, compute particles, multisampled
lines toggled between 1 and 4 samples, a transparent plasma, a feedback loop
over app-owned textures with a `copyDrawable` thumbnail, and an on-demand tile
counting its renders — in a scrolling grid of up to 64 (the culling test),
stacked with widgets, a popup, a captured fading group and a transformed tile,
and added, deleted and reparented while they animate. The footer reads the
host's own figures — frame rate, CPU and GPU time, components rendered, culled
and hidden,
layers rendered, components painted — and its second line is the proof that a
culled continuous tile keeps updating while it stops rendering.
`GPUComponents out.png [tab]` renders one tab headless into an image.
