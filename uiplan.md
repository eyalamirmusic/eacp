# The examples, through the component tier

`Apps/Graphics` holds ten example apps, and every one of them builds its
interface out of native objects: an `eacp::Graphics::View` per element, a
`ShapeLayerView` per rectangle, a `TextLayerView` per string. That is the tier
`UI::Component` exists to avoid — a real interface has hundreds of elements and
one native view apiece pays AppKit or DirectComposition for layout, tracking and
hit testing on every one.

The component tier is now good enough to hold them: `Apps/UI/ComponentTree`
draws 48 channel strips and 240-odd components in a handful of draws, and
`Apps/UI/SVGDocument` draws whole vector documents with gradients, clipping and
group opacity. What it has never done is *be an application* — take a keystroke,
edit a string, drag something. The examples are what will find that out, and
each one converted is a feature the tier either has or is about to grow.

**The decisions, taken before any of it was built:** each example is converted in
place and the native version deleted; per-element fonts are fixed properly, in
`eacp-text`, rather than worked around again; and the text editing Todo needs is a
real component-tier `TextEditor` rather than the native `TextInput` overlaid on
the host.

# What each example actually needs

Read by what it demonstrates rather than by what it draws, because that is what
survives the conversion. A demo of a tray icon is still a demo of a tray icon;
only its content view moves.

| example | what it demonstrates | what the tier is missing |
|---|---|---|
| CursorShapes | the pointer changing shape per *region* of one view | nothing — `Component::setMouseCursor` is exactly this |
| EmbeddedViewDemo | an eacp UI inside a host-provided NSView | nothing — `ComponentHost` is a `GPUView` |
| VideoRecorderDemo | recording a view to H.264 | nothing, if `renderToImage` reaches a `GPUView` |
| TrayApp | a tray icon toggling a borderless panel | two font sizes |
| IpcDemo | two processes sharing state over IPC | two font sizes |
| GUI | shapes, gradients, text, an animation | fonts, and somewhere to put a frame callback |
| MenuBarApp | native menus driving live state | fonts, keyboard |
| KeyInspector | key events and the clipboard | **keyboard, focus**, a monospace font |
| Todo | checkable rows edited in place | **keyboard, focus, a text editor** |
| ComplexGUI/TaskBoard | cards dragged between columns | **drag and drop**, an overlay above the tree |

Two gaps dominate, and neither is a widget.

**A host carries one font.** `Text::TextRenderer` bakes a family at construction
and rebuilds its atlas when the size changes, so a family and a size are
properties of the *host* and only `FontStyle` varies per call. Almost every
example above mixes at least a bold heading with a smaller caption, and
KeyInspector wants a monospace face in a window whose stock face is proportional.
This is the same wall the SVG module hit — `svgplan.md` named the honest fix and
took the cheap one, a `TextRenderer` per (family, size) swapped in around a run at
a batch break each way, which is what `SVGComponent` still carries.

**Nothing in the tier has ever seen a key.** `Component` has six mouse handlers
and no keyboard at all, no focus, and `ComponentHost` does not override
`View::keyDown`. Four examples need it, and one of them needs a text editor on
top of it.

# Rung 1 — fonts

Fix it where `svgplan.md` said to fix it: **key the atlas on the face as well as
the glyph.** The packing, growth, upload and eviction in `GlyphAtlas` are
portable logic that does not care how many faces feed it; what is single-font is
the *key* and the one `GlyphSource` behind it.

- `GlyphAtlas` holds a table of faces, each a `(family, pointSize)` resolved
  against the current device scale, created on demand. The key becomes
  `(face, codepoint, style)`; everything else in the class is untouched.
- The scale moves up into the atlas, since it is the atlas that now builds
  rasterizers. A changed scale clears, which is what `TextRenderer` already does
  a level higher by replacing the whole atlas.
- `TextRenderer::draw` and `measure` take a font. Its existing single-font API
  stays as the default face, so `ComponentHost` and the video apps need no
  change.

Then in the tier: **a `Font` is painter state, stacked like a colour.**
`g.setFont({family, size, style})` with a `ScopedFont` to match `ScopedState`, and
`drawText`, `measureText`, `lineHeight` and `ascent` all read it — the last three
being the part that matters, since a centred caption has to measure the glyphs
that will actually be drawn.

One draw for all of it, which is the point. A heading, a caption and a monospace
log in one paint are one instanced draw out of one texture, where the workaround
they replace is three atlases and four batch breaks.

**And then delete the workaround.** `Graphics::setTextRenderer`,
`ScopedTextRenderer` and `SVGComponent`'s font cache with its spare-list pruning
exist only because the renderer held one font. That is about a hundred lines
removed from the SVG module and a batch break per font removed from every
document that mixes them, and it retires an item `svgplan.md` has been carrying
since rung 1.

**Converted in this rung:** CursorShapes, EmbeddedViewDemo, VideoRecorderDemo,
TrayApp, IpcDemo, GUI.

# Rung 2 — the keyboard

A component tier without a keyboard is a tier for dashboards. The work is
ordinary and the design is not, in one respect: **who gets the key.**

- `UI::KeyEvent`, mirroring `Graphics::KeyEvent` the way `UI::MouseEvent` mirrors
  its native sibling.
- `Component::keyDown` / `keyUp`, returning a verdict rather than void — the same
  shape `mouseWheelMove` already uses, so an unconsumed key carries on up the
  tree and a component that ignores the keyboard needs no code to let its parent
  have it. This is what makes a shortcut on a root work while a text editor deep
  inside it holds focus.
- Focus: `setWantsKeyboardFocus`, `grabKeyboardFocus`, `focusGained`/`focusLost`,
  and one focused component per host. Mouse-down moves focus to the deepest
  component that wants it, which is what `grabsFocusOnMouseDown` does natively.
- Tab traversal, in the order children were added.

`ComponentHost` overrides `keyDown`/`keyUp` and routes. The native view already
gets the events; nothing below the tier changes.

**Converted in this rung:** KeyInspector, MenuBarApp.

# Rung 3 — editing text

`UI::TextEditor`: a caret, a selection, mouse placement and drag-selection,
arrows and Home/End, backspace and forward delete, and the clipboard. Single
line to begin with, because that is what every example here uses and because
wrapping is a layout problem the tier has not needed yet.

The reason to build it rather than overlay the native `TextInput` is that an
overlay never teaches the tier anything: the keystrokes, the selection and the
caret all stay on the other side of the boundary, and the first interface that
wants an editor inside a scroll panel finds a native view that does not scroll
with it.

A `Checkbox` comes with this rung, being what Todo's rows actually are.

**Converted in this rung:** Todo.

# Rung 4 — dragging

TaskBoard drags a card out of one column and into another, and the tier has the
two halves of that already: a press captures the mouse to a component, so drags
and the release reach it wherever the pointer travels, and `toFront` orders
siblings. What is missing is the part between — a payload, a target that can say
whether it will take it, and something drawn under the pointer that is above
every column rather than clipped inside the one it started in.

So: a `DragAndDropContainer` on an ancestor, `DragAndDropTarget` on whatever
accepts, and the dragged image drawn by the container in its own space. Which is
JUCE's shape, and it is JUCE's shape because the alternative — every column
knowing about every other — is what the example currently does by hand.

**Converted in this rung:** ComplexGUI/TaskBoard.

# What this plan will probably get wrong

- **That the font work is contained.** It is a key change in a class built around
  one face, and the thing most likely to bite is metrics: `lineHeight` and
  `ascent` are asked of the renderer with no font in hand by callers that
  currently cannot have one.
- **That returning a verdict from `keyDown` is obviously right.** It is right for
  shortcuts and wrong-feeling for a text editor, which consumes nearly
  everything; the risk is that every editor grows a `return true` at the bottom
  and the bubbling is decorative.
- **That the text editor is one rung.** Selection, clipboard and caret blinking
  are each small; IME is not, and neither is bidirectional text. Both are out of
  scope here and should be said to be, before the first non-Latin string arrives.
- **That deleting the native examples costs nothing.** They are the only working
  demonstrations of `ShapeLayerView`, `TextLayerView` and `View::paint` outside
  the SVG builder, and once they are gone the native tier's example coverage is
  whatever `SVGBuilder` happens to exercise.

# Rung 1, as built

A record rather than a plan. The font work is done, the workaround it was meant
to replace is deleted, and six of the ten examples are converted in place.

## What shipped

| where | what |
|---|---|
| `Text/Font.h` | `Font` — family, size, style — and `sameFace`, the tolerance two sizes are matched with |
| `Text/GlyphAtlas.{h,cpp}` | a table of faces, each a (family, size) built through a `FaceFactory`; the key is now (face, glyph, style); `setScale` rebuilds every face in place and drops what the old display rasterized |
| `Text/TextRenderer.{h,cpp}` | `draw` / `measure` / `lineHeight` / `ascent` per font, the single-font calls kept as the default face |
| `UI/Common.h` | `Font` and `FontStyle`, aliased the way `Color` and `Rect` already were |
| `UI/Graphics` | `setFont` / `setFontSize` / `setFontStyle` as stacked state, and `setTextRenderer` / `ScopedTextRenderer` deleted |
| `UI/Widgets` | `Label::setFont` / `setFontSize` / `setFontStyle` |
| `UI/Host/ComponentHost` | one `Font` where a family and a size used to be separate members, and `setFontFamily` no longer throws the renderer away |
| `SVG/SVGComponent` | the `DocumentFont` cache, its spare list and `findOrAddFont` deleted; a run carries a `UI::Font`; `font-weight` and `font-style` read, which the face now has somewhere to go |
| `Apps/GPU/GlyphAtlas` | a display change is `setScale` rather than a new atlas |
| `Tests/Text` | 11 more cases, 1031 in the project |

## What the plan got right, and what it did not have to say

- **The key change was the whole change.** Packing, growth, upload, the dirty
  rectangle and the generation counter were all untouched, exactly as the plan
  guessed: what was single-font was the key and the one source behind it.
- **The metrics worry was real and landed elsewhere.** The plan expected trouble
  from `lineHeight`/`ascent` being asked with no font in hand. What actually bit
  is that resolving a face *builds* the atlas, so `atlas->metrics(style,
  faceFor(font))` reads the pointer before the call that creates it. Two
  statements instead of one, and the same trap is waiting for anyone who writes
  that expression the obvious way.
- **The cache SVG is losing was more than a workaround.** `clearContent` handed
  the old renderers to the next build as a spare list so a resize would not
  re-rasterize every glyph in the document at a slightly different size. None of
  that is needed now — a face is looked up in the atlas and a size the document
  has stopped using simply stops being asked for — so about a hundred lines and
  their reasoning went with it.
- **`font-weight` came free and was not in the plan at all.** A `Font` carries a
  style, and the builder had nowhere to put one before; documents write
  `font-weight="bold"` on headings constantly and it had been silently dropped.

## What the conversions found

- **A cursor is a component, not arithmetic.** CursorShapes computed which band
  the pointer was in from its x. It is five components with five cursors now, and
  the hit test that decides who gets a click decides what the pointer looks like
  — the demo lost its only piece of geometry code.
- **A circle is a rounded rectangle.** IpcDemo drew its dots as a `Path`
  rebuilt on every arrival. A corner radius of half the side is the same circle
  out of the same distance field as every other rectangle in the tree, so the
  dots cost no mask, no rasterization and no path at all.
- **An animation is two floats and a repaint.** The old `AnimatedView` moved a
  `ShapeLayer` and set its opacity, which is retained state the window server
  owns. The component holds the state and `paint()` reads it, so nothing is
  retained between frames and a frame of the animation costs the quad it draws.
  No animation utility turned out to be needed for this rung — a `DisplayLink`
  member calling `repaint()` is the whole of it.
- **The native-feature demos did not notice.** TrayApp, EmbeddedViewDemo and
  VideoRecorderDemo kept their tray icon, their host-provided NSView and their
  recorder untouched: a `ComponentHost` is a `GPUView` is a `View`, so what those
  three demonstrate is unchanged and only their content moved.

## What is not verified, and why

`VideoRecorderDemo` **builds but was not run.** Nothing in this environment gets
a `DisplayLink` tick for a process launched into the background, so the demo
never reaches the call that starts recording — which is true of the version
before the conversion as well, so it is the environment and not the port. What
can be said without running it is structural: the recorder's snapshot mode calls
`View::renderToImage`, `GPUView` overrides the two hooks that serves, and a
`ComponentHost` is a `GPUView` — so the capture path is the GPU one, which is the
one that works, rather than the layer one.

Screen capture is also unavailable here (it returns black), so **none of the six
conversions has been looked at**. What stands behind them instead is the test
suite, which now covers the font path end to end: two sizes and two families of a
real face in one atlas, and `TextRenderer` measuring the face it was handed
rather than its default.
