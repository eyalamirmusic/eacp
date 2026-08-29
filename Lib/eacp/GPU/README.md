# GPU

Metal on Apple platforms, D3D12 on Windows, behind one API — and a shader EDSL
that makes a shader a C++ struct rather than a string literal per backend.

Everything here is main-thread only, like the rest of eacp, and every public
type hides its backend behind a `Pimpl`, so nothing Metal or D3D leaks into a
header an app includes.

## The pieces

| | |
| --- | --- |
| `Device` | The process-wide device and queue. `Device::shared()`, and `isValid()` on a machine with no GPU |
| `GPUView` | A `View` that owns a swapchain and hands you a `Frame` each tick |
| `Frame` | One frame's command buffer. Presents and commits on destruction |
| `RenderPass` | Records draws. Ends its encoder on destruction |
| `Buffer` | Vertex, index and storage buffers |
| `Texture` | 2D textures: uploaded, wrapped zero-copy from a camera buffer, or rendered into |
| `RenderPipeline` | A compiled pipeline state |
| `CommandBuffer` / `ComputePass` | The compute path — off-screen, blocking or not; `Frame::beginCompute` puts one on a frame |
| `Codegen/` | The shader EDSL and the MSL / HLSL emitters |

## A shader

`define()` records a graph of value handles. Nothing in it is text: the emitters
turn that one source into MSL and into HLSL, so the two backends cannot drift
apart on a shader an app wrote once.

```cpp
#include <eacp/GPU/GPU.h>

using namespace eacp;
using namespace eacp::GPU;

struct Vertex
{
    float position[2];
};

EACP_SHADER_VALUE(Vertex, Float2)

struct Waves final : ShaderProgram
{
    Waves() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto uv = varying(position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(0.5f + 0.5f * sin(uv.x() * 8.f + time), uv.y(), 0.f, 1.f));
    }

    Uniform<Float> time;

    EACP_SHADER(time)
};
```

`compile()` runs from the most-derived constructor: it walks the uniform members
that `EACP_SHADER` names and then calls `define()` through the vtable. After
that, `prepare(sampleCount)` builds the library and the pipeline, and
`pass.draw(shader)` binds everything and issues the draw.

The uniform block is bound only to the stage that reads one. Which stage that is
comes from the same walk the emitter declares the block from, so a bind cannot
disagree with the signature it is aimed at — and a stage that never declared it
is not bound at all, which is what Metal's validation layer otherwise reports as
an unused binding. App code that takes `draw(program)` apart to draw its own
geometry should call `pass.setUniforms(program)` rather than the two per-stage
setters, for the same reason.

### Naming the pipeline's state

`prepare` also takes a `RenderPipelineDescriptor`, which is the form to reach for
once more than one of these is not the shader's own choice:

```cpp
auto descriptor = RenderPipelineDescriptor {};
descriptor.sampleCount = sampleCount();
descriptor.depth = true;
descriptor.blendMode = BlendMode::AlphaBlend;
descriptor.cullMode = CullMode::Back;

program.prepare(descriptor);
```

The program fills in its own library and vertex layout, so those two fields are
ignored. The positional `prepare(sampleCount, depth, topology, blend, format)`
still exists and means exactly the same thing; it just says less at the call
site, and the fields past `depth` are usually the *target's* answers rather than
the shader's.

### Blending past the four named modes

`BlendMode`'s presets are what a UI, a sprite or a glyph wants. What they do not
cover is content whose *author* chose the equation — a material system, where
"modulate by what is behind me" is something written in a file that the renderer
has to honour rather than approximate. `blend` takes the equation itself and
wins over `blendMode` when set:

```cpp
auto blend = BlendState {};
blend.enabled = true;
blend.sourceColor = BlendFactor::DestinationColor;   // `blend filter`
blend.destinationColor = BlendFactor::Zero;
blend.sourceAlpha = BlendFactor::DestinationAlpha;
blend.destinationAlpha = BlendFactor::Zero;

descriptor.blend = blend;
```

`blendStateFor(mode)` writes a preset out in the same terms, and is what both
backends build from — so a preset means one thing, stated once.

`colorWriteMask` is beside it and independent of it: which channels reach the
attachment after the blend. `ColorWriteMask::none()` is a pass that updates the
depth or stencil plane and leaves the picture alone, which is what a shadow
volume being counted needs; per-channel masking has no workaround at all and is
why the field exists rather than the trick that used to stand in for it.

### Face culling, and which way round front is

`CullMode::None` is the default: both faces rasterise, which is what a mesh whose
winding is not known to be consistent needs. Under `Front` or `Back` a
wrongly-wound triangle does not draw wrongly — it does not draw at all.

**A triangle whose vertices run counter-clockwise in clip space — the space
`setPosition` writes, with y up — is front-facing.** That is glTF's convention,
and it is stated here in clip space rather than in the image because the viewport
flips y on the way and reverses the answer.

It is worth stating at all because both backends' own defaults read "clockwise is
front-facing", which is the opposite of the convention above. What they do not
differ on is what winding means: clip-space y is up and the framebuffer origin is
top left on each, so the NDC-to-screen mapping reverses winding by the same
amount on both and one convention is spelled the same way twice —
`MTLWindingCounterClockwise` on one side, `FrontCounterClockwise = TRUE` on the
other. `Tests/GPU/CullModeTests.cpp` is what fails if either drifts.

### What the EDSL has

- `Float`, `Float2/3/4`, `Float2x2`, `Float3x3`, `Float4x4` — built from their
  columns, transposed, and their determinant taken; multiplied by a vector on
  either side, which is a different product each way, and scaled by a scalar,
  which is neither. There is no `inverse`, and that is the languages rather
  than this: GLSL has one, MSL and HLSL do not
- `Int` and `Int2/3/4` — signed, with `%`, the bitwise set, the shifts, the
  comparisons, and the explicit crossings `toInt` / `toFloat`
- `Bool` and `Bool2/3/4` — what a comparison yields, collapsed by `any()` /
  `all()`, compared with each other, and crossed into a number with `toInt` /
  `toFloat`. Comparing two vectors is the operator itself, componentwise,
  because that is what both shading languages give a pair of vectors
- `UInt` for the compute thread id, a buffer index, and the slot an atomic add
  reserved — compared against each other and against unsigned literals
- Every swizzle of up to four components, on all three families, as one node
- The intrinsic set, spelled the way the languages underneath spell it —
  `rsqrt`, `atan2`, `mix` — rather than the way GLSL does, and taking a float
  literal in any argument position: `smoothstep(0.0, w, d)` mixes a literal edge
  with a computed one, `min(0.0, g)` puts the literal first, `step(d, 0.0)`
  second. A literal is anchored on the graph whichever argument is a handle
  brought, so which positions accept one is not a question the EDSL has an
  opinion about
- Statements: `var`, `select`, `ifThen`, `loop`, `breakLoop`, `continueLoop`.
  A `var` takes any handle and any matrix
- Compute-only: `atomicAdd`, `sharedArray<T, N>`, `barrier`,
  `threadIndexInGroup` — see the compute section
- `Array<T, N>` with a subscript, at a literal or a computed index
- Texture reads: `sample`, `sample` at a chosen level, and `fetch` at texel
  coordinates

There is no aggregate type and none is needed: the EDSL is embedded in C++, so a
struct of handles is a C++ struct.

```cpp
struct Hit
{
    Float distance;
    Float3 albedo;
};
```

### What it deliberately refuses

`ShaderBuilder::uniform<T>()` static_asserts rather than leaving these to a
comment, because each is a case where the two backends disagree about the
packing *inside* a value and no padding between fields can bridge it:

- `Bool` and the boolean vectors — MSL packs a `bool` into a byte, an HLSL
  cbuffer gives it four
- `Float2x2` and `Float3x3` — MSL packs a `float2x2` as two `float2` columns,
  16 bytes; an HLSL cbuffer gives every matrix row a register and takes 32.
  `Float4x4`, which both agree on, is the matrix to send

Send a `Float` and compare it; send a `Float4x4`. `Int` and the integer vectors
*are* uniforms — both languages give a signed integer four bytes and pack it
where they pack a float.

## Pipeline state

`prepare(sampleCount)` covers the common settings positionally. Everything else
a pipeline can be told goes through the descriptor form, which is the same
`RenderPipelineDescriptor` a hand-written shader fills in:

```cpp
shader.prepare({.sampleCount = sampleCount(),
                .depth = true,
                .cullMode = CullMode::Back});
```

**Depth is three fields, not one.** `depth` says the pipeline has a depth
attachment — the view has to have one too (`setDepth(true)`), and both backends
reject a draw whose pipeline disagrees with the pass about that. `depthCompare`
and `depthWrite` are what to do with it, and they come apart where it matters:
translucent geometry tests against the opaque depth already written and must not
write its own, or the nearer of two translucent surfaces hides the further one
instead of blending over it.

```cpp
opaque.prepare({.sampleCount = 1, .depth = true});                    // the default: LessEqual, writing
glass.prepare({.sampleCount = 1, .depth = true, .depthWrite = false}); // tests, does not write
```

**Culling is off by default, and the front face is counter-clockwise in clip
space** — glTF's convention, spelled out under "Face culling, and which way round
front is" above. `frontFace` is there for the geometry that does not arrive in
it: a mesh wound the other way, an instance mirrored by a negative scale, or an
inside-out shape like a skybox, none of which should need its indices rewritten.

```cpp
skybox.prepare({.sampleCount = 1, .cullMode = CullMode::Back,
                .frontFace = Winding::Clockwise});
```

Culling is pipeline state on D3D12 and encoder state on Metal. eacp hides that:
`RenderPass::setPipeline` applies both the mode and the winding on every bind, so
a pass that draws a culled mesh and then a full-screen quad gets the same picture
either way — `PipelineStateTests` covers that, and `CullModeTests` covers the
convention itself.

## Viewport, and how it differs from a scissor

`setScissorRect` clips: geometry outside the rect is thrown away, and what
survives is where it always was. `setViewport` **remaps**: clip space lands on
the rect instead of on the whole target, so the same vertices are drawn
somewhere else, at some other size.

```cpp
pass.setViewport({0.f, 0.f, width / 2.f, height});   // left pane
scene.drawFrom(leftCamera, pass);
pass.setViewport({width / 2.f, 0.f, width / 2.f, height});  // right pane
scene.drawFrom(rightCamera, pass);
pass.clearViewport();
```

That is split screen, a shadow map into one tile of an atlas, or a thumbnail —
none of which a scissor can do, because a scissor at the right-hand rect would
delete the geometry rather than move it. Both take pixels with the origin at the
top-left, like `Graphics::Rect`.

The optional `near`/`far` remap the depth a fragment writes. A viewport of
`[0.5, 1]` puts everything drawn through it behind everything drawn at the
default `[0, 1]`, whatever the geometry's own z says — which is how a layer gets
forced behind or in front of something it does not otherwise sort against.

**A rect that is empty or not wholly inside the render target is ignored**, not
clamped — the same rule `Texture::update` applies to regions, for the same
reason. A clamped scissor still shows the caller what they asked for; a clamped
viewport keeps drawing and silently squashes the picture into a rectangle nobody
chose, which looks like a bug in the caller's own maths. Neither backend forces
this: Metal accepts an out-of-target viewport happily. It is eacp's choice, and
`ViewportTests` is what holds the two backends to it.

## Rendering into a texture

A texture created with `TextureDescriptor::renderTarget` can be drawn into and
then sampled. It is a **pass on the frame you were already given**, not a frame
of its own:

```cpp
void render(Frame& frame) override
{
    {
        auto into = frame.beginPass(target, {{0.f, 0.f, 0.f, 1.f}});
        into.draw(writer);
    }

    auto pass = frame.beginPass();
    pass.draw(reader);          // reader.image = target
}
```

Passes on one command buffer are ordered by the queue, so a texture written by
an earlier one is legal to sample in a later one and neither backend needs a
fence to say so. That is the whole reason this is a pass rather than a frame:
`OffscreenTarget` — the snapshot path `View::renderToImage` rides on — blocks
until the GPU has finished, and a multi-pass effect would stall once per pass.

A texture cannot be sampled by the same pass rendering into it. Two of them and
a swap is the answer to that, which is what a feedback buffer is made of.

The pipeline has to agree with what it draws into: `prepare(...)` takes a
`PixelFormat`, and a program targeting a texture passes
`pixelFormatFor(itsFormat)`. Neither backend takes a draw whose pipeline
disagrees with its attachment.

### Depth

A target drawing a 3D scene needs a depth buffer, and asks for one on the same
descriptor:

```cpp
auto texture = TextureDescriptor {};
texture.renderTarget = true;
texture.depth = true;                                 // the pass gets one

auto pipeline = RenderPipelineDescriptor {};
pipeline.sampleCount = 1;                             // a texture pass never MSAAs
pipeline.depth = true;                                // the pipeline tests it
pipeline.colorFormat = pixelFormatFor(texture.format);

program.prepare(pipeline);
```

The buffer belongs to the target, is created with it and dies with it, so there
is no second lifetime to keep in step. Every pass into the texture clears it to
the far plane and stores nothing.

The two flags have to agree. A pipeline that declares depth drawing into a
target that has none is a validation error on Metal and an untested draw on
D3D12 — and on Apple silicon it *appears* to work, because the tile memory is
there whether or not anything attached it. Do not read that as permission;
`Texture::hasDepth()` is what a pipeline should be built from.

Render targets are still single-sampled. A texture target has nothing to resolve
into — the texture is what a resolve would produce — so a pipeline drawing into
one passes `sampleCount` 1 even when the same shader draws multisampled into the
drawable.

## Compute

A kernel is a `ComputeProgram`: storage buffers and uniforms as members, the
body in `define()`, dispatched over one index per element. Two places take one.

The grid comes from what the body asks for. `threadId()` gives a single index
and is dispatched with `dispatch(count)`; `threadPosition()` gives an `x` and a
`y` and is dispatched with `dispatch(width, height)`, in 8×8 groups. A kernel
takes one or the other — the generated entry point has one shape — and the two
extents are bounds-checked for you, so a grid that is not a multiple of the
group is safe to dispatch.

```cpp
void define() override
{
    auto p = threadPosition();
    write(output, p.y * stride + p.x, toFloat(p.x));
}

pass.dispatch(kernel, width, height);
```

`Device::makeCommandBuffer()` is the off-screen path — compute with no frame
around it. `commit()` submits and waits; `commitAsync()` submits and returns a
`Threads::Async<void>` that resolves once the GPU is done:

```cpp
auto commands = device.makeCommandBuffer();

{
    auto pass = commands.beginCompute();
    pass.dispatch(kernel, count);
}

commands.commitAsync().then([&] { /* output is ready */ });
// ...the CPU carries on here, while the kernel runs
```

Nothing about correctness changes between the two. `Buffer::read()` orders
behind the submission itself, so a read before the `Async` resolves is still
right — it just waits by hand for what the overlap was there to avoid.

`Frame::beginCompute()` is the other one: a compute pass on the frame's own
command buffer, ordered with its render passes the way two render passes are.
That is what lets a kernel's output feed the draw that consumes it, with the
data never reaching the CPU:

```cpp
void render(Frame& frame) override
{
    {
        auto compute = frame.beginCompute();
        compute.dispatch(integrate, particleCount);   // writes `state`
    }

    auto pass = frame.beginPass();
    draw.setInstanceBuffer(1, state, particleCount);  // reads the same buffer
    pass.drawInstanced(draw, particleCount);
}
```

`setInstanceBuffer` is `setInstances`' counterpart for data the program does not
own: the bytes a kernel wrote as a flat float array are read by the vertex stage
at the per-instance stride `instanceInput()` declared. One buffer, two views of
it, no copy.

### Atomics

`Uniform<AtomicBuffer>` is a storage buffer of **unsigned integers** every
thread may read-modify-write at once. `atomicAdd` adds to one element and gives
back what it held *before*, so threads that never meet come away with distinct
numbers — which is how a kernel hands out slots of a shared array:

```cpp
struct Bin final : ComputeProgram
{
    void define() override
    {
        auto id = threadId();
        auto slot = atomicAdd(counts, tileFor(id), 1u);

        ifThen(slot < capacity, [&] { write(items, slot, toFloat(id)); });
    }

    Uniform<AtomicBuffer> counts;   // uint elements
    Uniform<OutputBuffer> items;
    Uniform<UInt> capacity;
    EACP_SHADER(counts, items, capacity)
};
```

It is spelled as a statement, not an expression, and that is the two languages
rather than a choice: MSL's `atomic_fetch_add_explicit` returns the old value,
but HLSL's `InterlockedAdd` writes it through an out parameter and cannot appear
inside a larger expression. Naming the result is the only shape both can print.

The ordering is relaxed — the read-modify-write cannot be interleaved, and
nothing is said about how other memory either side of it is ordered. That is all
a counter needs; a kernel needing the second thing needs a barrier.

**The elements are integers.** The same `GPU::Buffer` bound to an `InputBuffer`
in a later kernel reads those bits as floats and yields nonsense. Read it back
with `counts.load(index)`, or have the kernel that finishes with it write the
values somewhere a float buffer can be read from. It binds like an output
otherwise, and takes a slot from the same counter.

### A dispatch the GPU sized

`dispatchIndirect` takes its threadgroup counts out of a buffer an earlier
kernel wrote, so a stage whose size depends on what the stage before it found
costs no readback — the number never reaches the CPU:

```cpp
{
    auto pass = commands.beginCompute();
    pass.dispatch(count, capacity);        // counts into `arguments`
}
{
    auto pass = commands.beginCompute();
    pass.dispatch(prepare, 1);             // count -> DispatchArguments
}
{
    auto pass = commands.beginCompute();
    pass.dispatchIndirect(consume, arguments, capacity);
}
```

`DispatchArguments` is the three **threadgroup** counts both backends read, at
the same size and in the same order. A kernel that counted 1000 items writes
`(1000 + threadGroupWidth - 1) / threadGroupWidth`, not 1000. Writing them means
writing integers, so the buffer is a `Uniform<AtomicBuffer>` and
`write(arguments, 0u, groups)` is the store.

The last argument is what the generated bounds guard compares against, and it
cannot be the real count — nothing on the CPU knows it. Pass the **capacity**.
The guard then stops nothing short, and a kernel that must not run past the real
count reads it from a buffer and returns itself. Both guards matter: this one
keeps threads inside the allocation, the kernel's own keeps them inside the
data. The grid is rounded up to whole groups either way, so the tail of the last
group runs and has to be harmless.

Each stage is its own pass. Threads of one dispatch are ordered against each
other by nothing but the end of that dispatch, so a kernel reading what the
previous one counted has to be in a later pass.

1D only. A 2D indirect dispatch would take a width and a height beside an offset
and could not be told apart from this one; nothing has needed it.

### Threadgroup memory

`sharedArray<T, N>()` is memory one dispatch group has in common: every thread
in the group reads and writes it, no thread outside sees it, and it is gone when
the group is. `threadIndexInGroup()` is what indexes it, and `barrier()` is what
makes one thread's writes visible to the rest:

```cpp
void define() override
{
    auto lane = threadIndexInGroup();
    auto scratch = sharedArray<Float, 64>();

    write(scratch, lane, input[threadId()]);
    barrier();

    // every thread now holds what all 64 of them fetched
    write(output, threadId(), scratch[lane ^ 1u]);
}
```

Nothing initialises it — what it holds before the group writes it is undefined,
which is why every use starts by filling it and waiting. Reading is a subscript;
writing goes through the same `write()` the buffers and textures use, because a
write is a statement and has to land where it was written.

**A barrier must be reached by every thread in the group or by none.** One
inside an `ifThen` that some threads take and others do not is undefined in both
languages, and undefined here means a hang rather than a wrong answer. Diverging
*after* a barrier is ordinary control flow; diverging *around* one is not.

That rule reaches the dispatch too: the emitted bounds guard returns early, so a
kernel with a barrier may only be dispatched over a whole number of groups —
`ComputeProgram` asserts rather than leaving it to the caller to remember. Round
the count up to a multiple of `ComputePass::threadGroupWidth` (or of
`threadGroupSize2D` in both axes) and guard the writes instead.

The declaration is the one place the two backends are not the same shape twice:
MSL's `threadgroup` is a local of the kernel function, HLSL's `groupshared` is a
global, so the same array lands on opposite sides of the entry point.

A buffer whose elements are records rather than single floats is read and
written a record at a time. `read2`/`read3`/`read4` take N consecutive floats
starting at `index * N`, and `write` has the matching `Float2`/`Float3`/`Float4`
overloads — the index is in records on both sides, so a kernel over a struct of
four floats never spells the stride:

```cpp
auto particle = state.read4(index);           // position.xy, velocity.xy
write(next, index, float4(newPosition, newVelocity));
```

Underneath it is still N scalar accesses over a run of floats, deliberately: a
retyped `float4` binding would buy one wide store and cost the CPU-side element
size that makes those same bytes bindable as a per-instance vertex stream.

A command buffer has one open encoder at a time, so let a pass end before
beginning the one that reads what it wrote. `Apps/GPU/ComputeParticles` is the
worked example, and `Apps/GPU/AsyncCompute` times the two commits against each
other.

A `write()` happens **where it is written**: one inside an `ifThen` runs only
when the condition holds, and one inside a `loop` runs every iteration. That is
worth stating because it was not always true — stores used to be collected and
emitted after the body, so a guarded write ran unconditionally and a looped one
ran once afterwards on the counter's final value. Both compiled and neither
complained; `Tests/GPU/StorePlacementTests.cpp` is what now says otherwise.

### Textures a kernel writes

The other thing a kernel produces is an image, and it reaches the fragment stage
with no new machinery at all: once a `Texture` is written by a kernel, the
`setFragmentTexture` that was always there samples it in a later pass on the
same frame.

A texture opts in the way a render target does, and only in a format a typed
store is guaranteed for — `RGBA8Unorm`, `RGBA16Float`, `RGBA32Float`. Notably
**not** `BGRA8Unorm`, the drawable's own format and the first one most people
reach for; asking for it yields an invalid texture rather than a kernel whose
writes go nowhere.

```cpp
auto descriptor = TextureDescriptor {};
descriptor.width = 512;
descriptor.height = 512;
descriptor.format = TextureFormat::RGBA8Unorm;
descriptor.computeWrite = true;

struct PaintPlasma final : ComputeProgram
{
    void define() override
    {
        auto p = threadPosition();
        write(target, p.x, p.y, float4(colourAt(p), 1.f));
    }

    Uniform<WritableTexture2D> target;      // bound as the kernel's output
    Uniform<Texture2D> source;              // sampled or fetched, if it needs one
    EACP_SHADER(target, source)
};
```

Read and written textures take slots from one counter — Metal binds both to one
texture index space — so a kernel reading one and writing another gives them
distinct indices. `Apps/GPU/ComputeImage` is the worked example: a kernel paints
a 512×512 texture every frame and the next pass samples it full-screen.

### Buffers a shader stage reads

The third way a kernel's output reaches a draw. `setInstanceBuffer` hands the
vertex stage one record per instance and a written texture hands the fragment
stage an image; a `Uniform<InputBuffer>` on a `ShaderProgram` hands either stage
the *whole* buffer, to subscript at an index it worked out:

```cpp
struct DrawFromPalette final : ShaderProgram
{
    void define() override
    {
        auto position = vertexInput(&Vertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(palette.read3(record), 1.f));
    }

    Uniform<InputBuffer> palette;   // bound whole, read by index
    Uniform<UInt> record;
    EACP_SHADER(palette, record)
};

draw.palette = computed;            // the buffer a kernel filled
draw.record = 3;
pass.draw(draw);                    // binds it to both stages
```

The same `InputBuffer` a kernel declares, and the same `read2`/`read3`/`read4`
record reads — what differs is only that no store makes the graph a kernel, so
it emits a vertex/fragment pair. Read-only here: writing stays the compute path's
job. Each stage declares only the buffers its own expressions read, and the
program binds to both, so a buffer works wherever `define()` reaches for it.

Reach for this when the thing being read is not an image and does not line up one
record per instance — a lookup table, a record picked by an id the shader
computed. When it *is* one record per instance, `instanceInput` is still the
idiomatic path.

## Mipmaps

```cpp
auto descriptor = TextureDescriptor {};
descriptor.width = 1024;
descriptor.height = 1024;
descriptor.mipmapped = true;

auto albedo = Device::shared().makeTexture(descriptor, pixels);
```

What this buys is the picture, not speed. A texture minified without mips samples
a scattering of individual texels, and *which* texels changes as the camera
moves — so a tiled floor or a detailed model shimmers and crawls at distance, and
no filtering at level 0 fixes it, because the information being aliased was
thrown away before the filter saw it. Off by default: a UI atlas or a video frame
is never drawn smaller than it is and would pay a third more memory for levels
nothing reads.

**The chain is built on the CPU, by eacp, for both backends.** Metal has
`generateMipmapsForTexture` and D3D12 has no equivalent at all — a chain there
means a compute shader, a UAV per level and a root signature to bind them. So the
choice was a GPU chain on one backend against a hand-written one on the other,
which is two filters producing two pictures for the same texture, or one filter
producing the same bytes for both. Only the second can be checked by a test, and
this library has been wrong about a cross-backend detail often enough to prefer
the version that can be.

A texture created with no pixels — a render target, a kernel output — gets no
chain, since there is nothing to build one from. `update()` rebuilds it;
`update(region, ...)` does not, because a partial upload cannot know what the
rest of the texture holds. Ask a texture what it got with `mipLevels()`.

No new `TextureSampling` configuration is involved: mip filtering on a
single-level texture is what both APIs do anyway, so the four configurations
still cover everything. That is also where a long-standing divergence was found —
D3D12's static samplers had always declared `MIN_MAG_MIP_LINEAR`, while Metal
left `mipFilter` at its default of `NotMipmapped`. Nothing could see it while no
texture had a second level.

## Texture formats

| Format | Notes |
| --- | --- |
| `RGBA8Unorm`, `BGRA8Unorm` | The ordinary ones |
| `R8Unorm` | One byte per pixel, sampled as `(r, 0, 0, 1)` — masks, palette indices |
| `RGBA16Float` | The float format to reach for |
| `RGBA32Float` | When the mantissa really is the point |

The float formats are not an optimisation. Eight bits per channel cannot hold a
value above 1 and quantise everything below it, so a pass that feeds back into
itself — a trail, a fluid, a running average — loses a little of its state every
frame and settles into a flat colour it can no longer leave.

Prefer `RGBA16Float`. Neither backend guarantees a device can *filter* a full
float texture, so a shader sampling one anywhere but at a texel centre would
come back nearest-neighbour on some machines and bilinear on others; half
filters everywhere eacp runs and holds far more range than a colour needs.

There are no mips: a texture has one level, so `sample(t, uv, level)` reads it
whatever level it asks for.

## Sampling

How a texture is sampled belongs to the *shader*, not to the `Texture`, which is
a deliberate break from the obvious design and has a Windows driver bug behind
it. See [`SAMPLERS.md`](SAMPLERS.md).

## Reading pixels back

`View::renderToImage` renders off-screen and hands back a `Graphics::Image`. It
is what the GPU tests check their output with, and it is worth knowing that what
comes back is what Core Animation composites — which is **premultiplied**. A
fragment left at alpha 0.25 comes back with its colour divided by four, and two
values that differed before that division can arrive equal after it. Write an
opaque alpha, or compare two renders rather than either against a number.

## Windows

The D3D12 backend is less exercised than the Metal one. Notes worth having:

- Samplers are static samplers in the root signature, not descriptor tables —
  again, see `SAMPLERS.md`
- Resource Binding Tier 1 hardware requires *every* descriptor table the root
  signature declares to be populated before a draw, even ones the shader never
  reads, so unused texture slots are seeded with a null descriptor
- Buffers decay to `COMMON` after every `ExecuteCommandLists` and are implicitly
  promoted on first use; textures do not, so a texture's state is tracked for
  its whole lifetime rather than per recording
