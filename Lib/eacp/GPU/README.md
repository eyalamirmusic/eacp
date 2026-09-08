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

### The CPU types a shader value is fed from

A vertex field or a uniform can be any CPU type whose shape the shader layer
knows. `float`, `float[N]` and `std::array<float, N>` are built in; a type of
your own says so with `EACP_SHADER_VALUE` (or a `using ShaderValue = Float3;`
member), and `Core/Maths` arrives already registered — `Maths::Vec2`, `Vec3`,
`Vec4` and a column-major `Maths::Mat4`, all packed exactly as the float2 /
float3 / float4 / float4x4 they stand for.

That is the whole point of them: the same value does the CPU-side geometry and
crosses to the GPU with nothing to repack or transpose.

```cpp
using namespace eacp::Maths;

struct Vertex
{
    Vec3 position;
    Vec3 normal;
};

// ... in the view:
auto view = Mat4::lookAt(eye, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
auto projection = Mat4::perspective(aspect, radians(50.f), 0.1f, 200.f);

shader.viewProjection = projection * view;   // Uniform<Float4x4>
shader.lightDirection = normalize(eye);      // Uniform<Float3>
```

`Mat4` is right-handed with a `[0, 1]` depth range — what both backends clip
against, and what `ShaderProgram::perspective` builds — so a matrix assembled on
the CPU and one assembled inside `define()` mean the same thing. Apps/GPU has
both: `Teapot` and `Maze` send scalars and let the shader build the matrices,
`CubeMap` and `StencilShadows` build them here and send the result.

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
- `UInt` for the compute thread id, a buffer index, an element of an integer
  buffer, and the slot an atomic add reserved — compared against each other and
  against unsigned literals, and carrying the whole integer operator set the
  signed scalar does: `%`, the bitwise set, the shifts and `~`, each against
  another `UInt` or an unsigned literal on either side. `unsignedInteger(n)` is
  the literal itself as a handle, the unsigned sibling of `constant`, `boolean`
  and `integer`
- `UInt2/3/4` — the unsigned vectors, carrying that same set componentwise
  wherever the signed ones carry theirs, arithmetic wrapping at 2^32 rather than
  overflowing a sign, plus the crossings `toUInt` / `toInt` / `toFloat` and the
  bitcasts `asUInt` / `asFloat`
- Every swizzle of up to four components, on all four families, as one node
- The intrinsic set, spelled the way the languages underneath spell it —
  `rsqrt`, `atan2`, `mix` — rather than the way GLSL does, and taking a float
  literal in any argument position: `smoothstep(0.0, w, d)` mixes a literal edge
  with a computed one, `min(0.0, g)` puts the literal first, `step(d, 0.0)`
  second. A literal is anchored on the graph whichever argument is a handle
  brought, so which positions accept one is not a question the EDSL has an
  opinion about
- The transcendentals a network's activations are written out of: `sinh`,
  `cosh`, `tanh` and `log10`, which both languages have natively, and `erf` /
  `erfc`, which neither has at all — those two are emitted as a polynomial held
  to under 6e-7 absolute, so a shader can spell the exact GELU rather than the
  tanh approximation of it. The origin is exact, which the polynomial on its own
  is not: `erf` is odd across it bit for bit and zero at it, `erfc` is one
  there, and the two sum to one
- Statements: `var`, `select`, `ifThen`, `loop`, `breakLoop`, `continueLoop`.
  A `var` takes any handle and any matrix. `select` runs across every family —
  a float, an index, an integer vector, a mask — and takes a literal on either
  side of a scalar one; the condition is a scalar `Bool` in all of them, which
  is the conditional operator both languages already print
- Compute-only: `atomicAdd`, `shared<T>(count)`, `barrier`, `localId` — see the
  compute section
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

`ShaderBuilder::varying<T>()` refuses one type for a different reason: a `Bool`
varying. GLSL allows no boolean stage input or output, and no `flat` qualifier
changes that, so there is no source the emitter could print that would compile.
Carry an `Int` across and test it, or carry the comparison's operands and
compare in the fragment stage. An `Int` or `UInt` varying, signed or unsigned
vectors included, crosses uninterpolated — the emitter writes `flat`,
`nointerpolation` or `[[flat]]` for it on its own, since every dialect requires
that of an integer.

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
pipeline.sampleCount = texture.sampleCount;           // 1 unless it multisamples
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

### Multisampling a target

`TextureDescriptor::sampleCount` above 1 grows a multisampled colour texture
beside the target: the pass renders into that one and resolves into the target
at the end of *every* pass, so what a shader samples, what `read()` reads and
what a blit copies is always the resolved picture.

```cpp
auto texture = TextureDescriptor {};
texture.renderTarget = true;
texture.stencil = true;                  // the depth buffer comes at the same count
texture.sampleCount = 4;

auto target = Device::shared().makeTexture(texture);   // invalid if 4 is refused
```

Three things follow from it and are worth knowing before reaching for it.

The count has to reach every pipeline that draws there —
`RenderPipelineDescriptor::sampleCount = target.sampleCount()` — and both
backends reject a draw where the two disagree.

A count the device cannot render at makes the texture **invalid** rather than
quietly dropping to 1, since a silent drop would leave every pipeline compiled
against a number the pass does not have. `Device::supportsSampleCount()` is how
to pick one before building anything.

The samples are *kept* as well as resolved, so a pass that does not clear loads
the multisampled attachment back rather than the flattened picture — which is
what makes a suspended pass (`DepthAction::Resume`) and a mid-frame copy of the
target work at 4 samples the same way they do at 1. `sampleableDepth` costs a
second depth buffer here: the shaders eacp generates declare a `depth2d`, so the
depth plane is resolved into a single-sampled twin and that is what
`setFragmentDepthTexture` binds.

## Compute

A kernel is a `ComputeProgram`: storage buffers and uniforms as members, the
body in `define()`, dispatched over one index per element. Two places take one.

The grid comes from what the body asks for. `threadId()` gives a single index
and is dispatched with `dispatch(count)`; `threadPosition()` gives an `x` and a
`y` and is dispatched with `dispatch(width, height)`, in 8×8 groups;
`threadPosition3()` adds a `z` and is dispatched with
`dispatch(width, height, depth)`, in 4×4×4 groups — what anything natively
indexed by three numbers wants, an attention score by (key, query, head) among
them, rather than a third axis folded into a row on the host. A kernel takes one
of the three — the generated entry point has one shape — and every extent is
bounds-checked for you, so a grid that is not a multiple of the group is safe to
dispatch.

`threadId2()` and `threadId3()` are those same two positions as one value — a
`UInt2` and a `UInt3` that swizzle, compare and compute like any other vector
handle — with `localId2()`/`localId3()` and `groupId2()`/`groupId3()` beside
`localId()` and `groupId()` on the same terms. Either spelling of a rank fixes
it, so `threadId2()` and `threadPosition()` sit in one kernel and a 2D index
next to a 3D one still does not.

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

### In place

An elementwise stage rewrites the buffer it was handed rather than filling a
second one. The buffer is bound once, to an output slot, and the kernel reads
the element it is about to store to:

```cpp
void define() override
{
    auto i = threadId();
    auto x = output[i];

    write(output, i, 0.5f * x * (1.0f + erf(x * 0.70710678f)));
}
```

Within one thread, statements run in the order they were written, so the read
observes what the element held. Another thread's store is visible only once the
dispatch has ended, so this is for a 1:1 stage and not for one that reads its
neighbours.

Binding one `GPU::Buffer` to an input slot **and** an output slot of the same
kernel is the form that does not port: D3D12 needs the resource in a different
state for each of those two bindings, and the second bind transitions it out
from under the first. It would not read as an in-place kernel anyway — a value
read through one slot keeps its name across a store to another, so the second
use of it is the value from before the store.

### Timing a pass

A pass given a label is timed by the hardware, on a command buffer exactly as on
a frame. The numbers come off the buffer itself once the GPU has finished it —
after `commit()`, or after the `Async` from `commitAsync()` has resolved:

```cpp
{
    auto pass = commands.beginCompute("attention");
    pass.dispatch(attention, count);
}

commands.commit();

for (const auto& pass: commands.timings().passes)
    log(pass.label, pass.milliseconds);
```

`timings().milliseconds` is the buffer end to end. An unlabelled pass is not
timed and does not appear, and a command buffer with no labelled pass at all
builds no timestamp resources; `supportsPassTimings()` says whether this device
can break a buffer down by pass, as `Device::supportsPassTimings()` does for a
frame.

### Zeroing a buffer

`fill` writes a byte over a whole buffer or over a range of one, on the GPU:

```cpp
commands.fill(cache);                                        // zeroed
commands.fill(BufferRange {&cache, rowBytes * step, rowBytes}, 0xff);
```

It is recorded on the command buffer like a pass, and ordered like one: a kernel
dispatched after the fill reads what the fill wrote, and a fill after a kernel
overwrites what the kernel wrote. The offset and the length must be multiples of
4, and no pass may be open. Nothing reaches the host, which is the point — a
cache re-zeroed between passes used to be an upload of zeros per pass.

### Part of a buffer

A storage-buffer member takes a `BufferRange` as readily as a whole `Buffer`,
and so do `ComputePass::setInputBuffer` and `setOutputBuffer`. The kernel's
element zero is the element at the offset, so one allocation can be written a
row at a time:

```cpp
kernel.keys = BufferRange {&cache, rowBytes * step, rowBytes};
pass.dispatch(kernel, rowElements);     // writes cache[step], leaves the rest
```

The offset must be a multiple of `Device::storageBufferOffsetAlignment()` — four
on Metal and D3D12, which take any word-aligned offset, and the device's own
limit on Vulkan, where the offset goes into a descriptor (16 on Mesa's lavapipe,
up to 256 by the spec). So a row a kernel is bound over is rounded to that
rather than to the element size:

```cpp
const auto stride = Device::shared().storageBufferOffsetAlignment();
kernel.keys = BufferRange {&cache, row * stride, stride};
```

`range.bytes` is not enforced: what stops a kernel short is the count passed to
`dispatch`. A range that names no buffer, starts off that grid, or starts at or
past its buffer's end, binds nothing.

The render side takes a range wherever the compute side does:
`RenderPass::setVertexBuffer` and `drawIndexed` over the geometry, which want
only a word-aligned offset, and `setVertexStorageBuffer` and
`setFragmentStorageBuffer` over the buffer a stage subscripts, which want the
device's alignment like a kernel's slot — the latter being what a
`Uniform<InputBuffer>` on a `ShaderProgram` binds through. A draw handed an
unbindable index range draws nothing.

### Buffers of integers

`Uniform<UIntInputBuffer>` and `Uniform<UIntOutputBuffer>` are the pair above
with `uint` elements: the subscript yields a `UInt` and `write` takes one.
Everything else is the same — the same slot counter, the same
`setInputBuffer`/`setOutputBuffer`, whole buffers or ranges alike — and both
backends declare them beside the float pair, `device const uint*` /
`device uint*` on Metal and `StructuredBuffer<uint>` /
`RWStructuredBuffer<uint>` on HLSL.

What they are for is data that is not a number to compute with: the token ids a
gather looks rows up by, the index an argmax arrived at, a count. A float
buffer carries those only as bits to cast, and only while they stay under 2^24.

They read and write records the way the float pair does: `read2`/`read3`/`read4`
yield a `UInt2`/`UInt3`/`UInt4`, `write` takes one, and the index counts records
on both sides.

```cpp
struct Gather final : ComputeProgram
{
    void define() override
    {
        auto i = threadId();
        write(rows, i, table[ids[i / width] * width + i % width]);
    }

    Uniform<UIntInputBuffer> ids;
    Uniform<InputBuffer> table;
    Uniform<OutputBuffer> rows;
    Uniform<UInt> width;
    EACP_SHADER(ids, table, rows, width)
};
```

One kernel's `UIntOutputBuffer` is the next one's `UIntInputBuffer` on the same
`GPU::Buffer`, so a decoder's ids go from the step that picked them to the step
that looks them up without reaching the CPU. A render stage reads one too, on
the terms below. There is no signed sibling: `Int` indexes constant arrays, and
a storage buffer of them has not been wanted.

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
in a later kernel reads those bits as floats and yields nonsense. Bind it to a
`UIntInputBuffer` instead — which is how a later kernel reads what the counting
one left — or read it back with `counts.load(index)`. It binds like an output
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

The offset the arguments are read at — the last parameter, for a buffer holding
several grids — must be a multiple of four and leave a whole `DispatchArguments`
behind it. One that does not dispatches nothing.

Each stage is its own pass. Threads of one dispatch are ordered against each
other by nothing but the end of that dispatch, so a kernel reading what the
previous one counted has to be in a later pass.

1D only. A 2D or 3D indirect dispatch would take its extents beside an offset
and could not be told apart from this one; nothing has needed it.

### Threadgroup memory

`shared<T>(count)` is memory one dispatch group has in common: every thread in
the group reads and writes it, no thread outside sees it, and it is gone when
the group is. `localId()` is what indexes it, and `barrier()` is what makes one
thread's writes visible to the rest:

```cpp
void define() override
{
    auto lane = localId();
    auto scratch = shared<Float>(64);

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
`threadGroupSize2D` / `threadGroupSize3D` in every axis) and guard the writes
instead.

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
size that makes those same bytes bindable as a per-instance vertex stream. It is
one write above them all the same — every component is stored the value the
record held before the first of them ran — so a record read out of an output and
rearranged back into it swaps its components rather than broadcasting one:

```cpp
auto pair = output.read2(i);
write(output, i, float2(pair.y(), pair.x()));
```

Every read takes an unsigned literal as well as a computed index — `input[0]`,
`input.read4(0u)` — so the one element a whole dispatch broadcasts from needs no
`var()` to carry its index, the same courtesy `AtomicBuffer::load` extends to a
shared counter.

**An output is readable too.** `output[i]` and its `read2`/`read3`/`read4` are
the subscript the store already is: both backends declare an output writable
(`device float*` on Metal, `RWStructuredBuffer<float>` on HLSL), so nothing new
is bound and nothing new is declared. A softmax is the case that asks for it —
normalising wants the exponentials the kernel just wrote, not `exp()` evaluated a
second time:

```cpp
write(output, i, exp(input[i] - peak));
write(output, i, output[i] / total);
```

What that promises is read-after-write **within one thread**, in the order the
statements were written. Another thread's store is visible only once the dispatch
has ended, exactly as it is for the count `AtomicBuffer::load` hands back.

A resource member holds a pointer, so it takes a named buffer or texture and
refuses a temporary outright: `kernel.input = device.makeBuffer(...)` would point
into something destroyed at the semicolon, and it is a compile error rather than
a wrong picture.

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
the buffer *itself*, to subscript at an index it worked out:

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
it emits a vertex/fragment pair. A `Uniform<UIntInputBuffer>` reads here on the
same terms. Read-only either way: writing stays the compute path's job. Each
stage declares only the buffers its own expressions read, and the program binds
to both, so a buffer works wherever `define()` reaches for it.

A `BufferRange` binds here as it does on a kernel: `draw.palette = BufferRange
{&computed, rowBytes * row, rowBytes}` makes the shader's element zero the
element at the offset, under the rules in *Part of a buffer*.

Reach for this when the thing being read is not an image and does not line up one
record per instance — a lookup table, a record picked by an id the shader
computed. When it *is* one record per instance, `instanceInput` is still the
idiomatic path.

### fp16 weights, kept packed

There is no `Half` value type and there is not going to be one: the Windows
backend compiles HLSL through FXC at `cs_5_0`, where `half` is a synonym for
`float` and there is no 16-bit arithmetic at all, so the same declaration would
mean two different things on the two backends. What *is* portable, and what a
model's weights actually want, is fp16 **storage** with fp32 arithmetic — half
the buffer, half the bandwidth, and every value widened before it is used:

```cpp
void define() override
{
    auto i = threadId();
    write(output, i, weights.readHalf(i) * input[i]);   // fp16 in, fp32 maths
}
```

A float storage buffer is a run of floats on both backends, so a packed word
arrives as a float whose value is meaningless and whose bits are the payload.
These are the way in and out of that:

| call | what it gives |
| --- | --- |
| `input.readHalf(i)` | element `i` of a buffer of halves, widened to a `Float`. `i` counts halves, so an N-weight buffer is walked `0..N-1` |
| `input.readHalf2(i)` | both halves of word `i` as a `Float2`, `.x` the low bits |
| `unpackHalf2(bits)` | the same, from a `UInt` already in hand |
| `packHalf2(pair)` | two floats narrowed and packed into a `UInt` |
| `writeHalf2(out, i, pair)` | that word stored at `i` — `readHalf2` reads it back |
| `asUInt(f)` / `asFloat(u)` | a value's bits rather than its value, both ways |

Size the buffer in whole words: `readHalf` fetches the word at `i / 2`, so an
odd count of halves reads past its last byte on the final element.

`readHalf` emits a two-argument helper — the word and which half of it — rather
than unpacking both and selecting: MSL and HLSL each reach the wanted half with
a single shift. `unpackHalf2` and `packHalf2` are helpers for the reason
`callName` cannot serve them, MSL bitcasting a `half2` where HLSL calls
`f16tof32`/`f32tof16` against a shift. Only the helpers a graph calls are
emitted into it.

Widening is exact on both backends, subnormals and infinities included, and so
is a round trip through `packHalf2` of anything fp16 can hold. **Narrowing a
value it cannot hold is the one place they differ**, alongside `round()`: Metal
converts per IEEE — nearest-even, and a finite magnitude past 65504 becomes an
infinity — while D3D specifies round-to-zero and saturates that magnitude to the
largest finite half instead. Round before narrowing if the answer has to be the
same on both.

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

**Or hand over a chain of your own**, which is `TextureDescriptor::mipLevels`:

```cpp
descriptor.mipLevels = mipLevelCount(1024, 1024);   // 11 levels in `pixels`

auto albedo = Device::shared().makeTexture(descriptor, pixels);
```

`pixels` is then those levels tightly packed, level 0 first, each one
`levelBytes(format, mipExtent(width, i), mipExtent(height, i))` — the layout
`buildMipChain` already produces, and `mipChainBytes` sizes the block. eacp
uploads them as they arrive and runs no filter of its own.

Two callers want this, and for different reasons. A block-compressed texture has
no other way to have a chain at all — 4x4 blocks cannot be averaged — and every
`.dds` file carries the one its compressor built. And a caller whose own filter
differs on purpose: Doom 3's mip builder preserves a zero border, so a projected
light's low levels stay dark at the edge where an unweighted average spills light
past it.

It is a descriptor field rather than an `update()` overload because both APIs fix
a texture's level count when the resource is created. `mipmapped` beside it is
refused rather than resolved — the two say opposite things about who builds the
chain — as are a count above `mipLevelCount`, null pixels, a render target, a
kernel output and a cube.

No new `TextureSampling` configuration is involved: mip filtering on a
single-level texture is what both APIs do anyway, so the four configurations
still cover everything. That is also where a long-standing divergence was found —
D3D12's static samplers had always declared `MIN_MAG_MIP_LINEAR`, while Metal
left `mipFilter` at its default of `NotMipmapped`. Nothing could see it while no
texture had a second level — and neither could `sample(t, uv, level)`, which
names a level instead of letting the hardware pick one from the derivatives, and
read level 0 whatever it asked for because level 0 was all there was.

## Texture formats

| Format | Notes |
| --- | --- |
| `RGBA8Unorm`, `BGRA8Unorm` | The ordinary ones |
| `R8Unorm` | One byte per pixel, sampled as `(r, 0, 0, 1)` — masks, palette indices |
| `RG8Unorm` | Two, sampled as `(r, g, 0, 1)` — an NV12 frame's chroma plane |
| `RGBA16Float` | The float format to reach for |
| `RGBA32Float` | When the mantissa really is the point |
| `R32Float` | One full-precision channel — a depth copy, a distance field |
| `BC1RGBA`, `BC2RGBA`, `BC3RGBA`, `BC7RGBA` | Block-compressed — DXT1/3/5 and BC7 |

The float formats are not an optimisation. Eight bits per channel cannot hold a
value above 1 and quantise everything below it, so a pass that feeds back into
itself — a trail, a fluid, a running average — loses a little of its state every
frame and settles into a flat colour it can no longer leave.

Prefer `RGBA16Float`. Neither backend guarantees a device can *filter* a full
float texture, so a shader sampling one anywhere but at a texel centre would
come back nearest-neighbour on some machines and bilinear on others; half
filters everywhere eacp runs and holds far more range than a colour needs.

The block-compressed formats are for content that **arrived** compressed — a
`.dds` file, an atlas some tool produced. eacp neither compresses nor
decompresses: the blocks go to the device as they came off disk, which is what
saves the decode at load and the four- or eightfold texture memory afterwards. A
4x4 block is one 8-byte record (BC1, so an eighth of RGBA8) or one 16-byte record
(BC2, BC3, BC7, so a quarter), and every size is therefore counted in whole
blocks — `levelBytesPerRow`, `levelRows` and `levelBytes` are what to measure an
upload with, and `bytesPerPixel` answers 0 for them because there is no such
number.

```cpp
descriptor.format = TextureFormat::BC1RGBA;   // 8 bytes per 4x4 block
descriptor.mipLevels = levelsInTheFile;       // the compressor's chain

if (Device::shared().supportsBlockCompression())
    texture = Device::shared().makeTexture(descriptor, fileBytes);
```

Ask the device first: a texture in a format it refuses is invalid rather than
quietly something else, exactly as a refused `sampleCount` is. Every Mac and
every Direct3D device answers yes; an Apple-family iOS GPU mostly does not.

A compressed texture cannot be a render target or a kernel output, `read()` and
`update(region, ...)` are no-ops on one, `update(pixels)` takes the same packed
block the constructor took with `bytesPerRow` at 0, and `mipmapped` gives it
exactly one level — the CPU filter averages texels and a block is not four
numbers to average. `mipLevels` is how it gets a chain. There are no sRGB
variants, eacp having no sRGB formats at all.

## Sampling

How a texture is sampled belongs to the *shader*, not to the `Texture`, which is
a deliberate break from the obvious design and has a Windows driver bug behind
it. See [`SAMPLERS.md`](SAMPLERS.md).

## Driver quirks

Two more D3D12 operations are known to be refused by a shipping driver — the
Parallels virtual GPU fails a command list that resolves a multisampled depth
plane, and removes the device outright on a region read-back — and the backend
routes around both when it finds it is on such a driver. It finds out by
trying: before the real device is created, a throwaway device records each
operation and is asked to close the list, and a refusal sets the matching flag
in `DriverQuirks`. Nothing is identified by name, so a fixed driver drops the
workaround by itself and an unknown driver with the same gap picks it up.
`EACP_D3D12_QUIRKS=1` sets every flag without asking, which is how the
fallback paths are run against WARP.

## Reading pixels back

`View::renderToImage` renders off-screen and hands back a `Graphics::Image`. It
is what the GPU tests check their output with, and it is worth knowing that what
comes back is what Core Animation composites — which is **premultiplied**. A
fragment left at alpha 0.25 comes back with its colour divided by four, and two
values that differed before that division can arrive equal after it. Write an
opaque alpha, or compare two renders rather than either against a number.

`Texture::read` is the other half, and the one an app that composes its frame
into a render target wants: the texture's own pixels, in its own format, with no
compositor in between and no second render. It is `update()` backwards — rows
tightly packed at the format's `bytesPerPixel` unless a stride says otherwise,
row 0 at the top, a region overload beside the whole-texture one.

**It reads what has been committed, not what has been recorded**, which is the
rule `Buffer::read` already carries and the one that catches people. A frame's
passes reach the GPU when the frame ends, so this:

```cpp
void render(Frame& frame) override
{
    { auto pass = frame.beginPass(target); pass.draw(scene); }

    frame.flush();           // <- without this, the read is a frame behind
    target.read(pixels.data());
}
```

`Frame::flush()` sends everything recorded so far and carries on recording, so
the read that follows it sees the passes above it. No pass may be open when it
is called — a command buffer takes one encoder at a time — and it costs Metal's
*frame* timing, which is read off a command buffer and after a flush there are
two of those. Nothing else needs it: two passes on one frame already see each
other's results without it.

Both calls block until the GPU has finished. That is what a read-back is; it is
not something to put in a frame loop.

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
- A buffer's storage says which of two very different writes it gets.
  `BufferStorage::Device` — the default, and every buffer an app makes — is a
  default-heap resource filled by a staged copy: a memcpy into the recording's
  upload arena, a barrier to `COPY_DEST`, a `CopyBufferRegion` and a barrier
  back before the draw. `BufferStorage::Streaming`, which is what
  `StreamingBuffers` asks for its arenas, is an `UPLOAD`-heap resource mapped
  once and kept mapped, bound as vertex, index or constant data straight out of
  that mapping: the write is the memcpy and nothing is recorded at all. Upload
  heaps are permanently in `GENERIC_READ`, so none of those barriers exists to
  be recorded either — which is why a renderer streaming hundreds of ranges a
  frame pays for hundreds of copy commands on one and none on the other. What
  buys it is the rule `StreamingBuffers` already keeps: no arena is written
  while a frame that drew from it can still be on the GPU

## Linux

The Vulkan backend draws, on screen and off. `Device`, `Buffer`,
`ShaderLibrary`, `ComputePipeline`, `ComputePass`, `CommandBuffer`,
`GpuTimestamps`, `Texture`, `RenderPipeline`, `RenderPass` and both `Frame`
constructors are real. `GPUView::renderNativeContent` renders into an off-screen
target and reads it back — the path every pixel-comparison test rides — so all
of `Tests/GPU` (bar the Metal-only `TextureInteropTests.mm`) and
`Tests/GPUWidgets` run on lavapipe with no display at all; and a `GPUView` in a
`Graphics::Window` presents through a `VK_KHR_swapchain` over a Wayland surface.
It is built on every Linux build, exactly as the Metal and D3D12 backends
are on theirs; `-DEACP_BUILD_GRAPHICS=OFF` is the only thing that leaves it
out.

Notes worth having:

- **Nothing links the loader.** `volkInitialize()` opens `libvulkan.so.1` by
  name at runtime, so the build needs no Vulkan package and the same binary runs
  on a machine with no driver — `Device::isValid()` is false there, which is
  what every GPU test already self-skips on. The headers, `volk` and
  VulkanMemoryAllocator are CPM-fetched (`CMake/FindVulkanBackend.cmake`) and
  fetched on no other platform.
- **Vulkan 1.3 core is the floor**, plus five features asked for by name:
  `timelineSemaphore`, `synchronization2`, `dynamicRendering`,
  `descriptorBindingPartiallyBound` and `shaderStorageImageWriteWithoutFormat`
  (the emitter declares a written texture as a `writeonly image2D` with no
  format qualifier). A device missing one is not used, rather than used until it
  fails.
- **eacp ships its own shader compiler here**, which it does on neither other
  backend: GLSL 450 through glslang into SPIR-V, at a fixed ~2 MB per binary and
  a one-time ~90 ms symbol-table build that `VulkanShared` pays at device
  creation so it never lands in a frame.
- **Memory is sub-allocated by VMA**, not one allocation per buffer:
  `maxMemoryAllocationCount` is commonly 4096, so the committed-resource model
  D3D12 uses would run a scene out of allocations long before it ran out of
  memory.
- **A second `Device` shares the queue.** A D3D12 command queue is created on
  demand; a `VkQueue` comes out of a family with a driver-decided count, and
  lavapipe offers one. So the queue lives in `VulkanShared` behind a mutex, and
  what keeps two Devices independent is everything else — their own command
  pools, timeline semaphores, upload arenas, constant rings and descriptor
  pools. `nativeQueue()` is therefore the same handle for every Device here.
- **Every recording ends with a global memory barrier.** Consecutive submissions
  on a queue execute in order but are not automatically visible to one another,
  and one barrier per submit is a rounding error against the dozens a frame
  would otherwise need. It is what makes the per-recording use tracking in
  `transitionForUse` correct, and it is the analogue of D3D12 buffers decaying
  to `COMMON` after every `ExecuteCommandLists`.
- **A storage buffer's offset is the device's**, which is why the rule is
  `Device::storageBufferOffsetAlignment()` rather than a constant four. Vulkan
  writes the offset into a descriptor, and a descriptor may name no offset off
  `minStorageBufferOffsetAlignment` — 16 on lavapipe, and up to 256 by the spec,
  against the four Metal and D3D12 each take. A range off the device's grid
  binds nothing here, exactly as one past the buffer's end does, on a kernel's
  slots and on `setVertexStorageBuffer`/`setFragmentStorageBuffer` alike, so a
  caller that sub-allocates by row asks the device for the step — which is what
  the ranged cases in `Tests/GPU/UIntBufferTests.cpp` and
  `ComputeBufferRangeTests.cpp` do.
- **`CommandBuffer::fill` is `vkCmdFillBuffer`**, whose word is the byte
  repeated four times, so the offset and the length are on the same four-byte
  grid the API documents and the length is clamped to the buffer's end. Nothing
  orders it by hand: the fill is a transfer write like any other, so the same
  `transitionForUse` tracking that orders a dispatch against an upload orders it
  against the passes either side.
- **An off-screen command buffer is timed by the query pool a frame is.**
  `CommandTimer` drives the same `GpuTimestamps` — one slot rather than four,
  the pass's pair written at `TOP_OF_PIPE` and `BOTTOM_OF_PIPE` around the
  encoder as `Frame::timePass` writes them, and the pool reset and the buffer's
  own two queries recorded by the first labelled pass. A command buffer that
  labelled nothing creates no pool.
- **A pass is one `vkCmdBeginRendering`; there is no `VkRenderPass`.**
  `DepthAction` is the attachment's load and store ops — `Clear` is
  `CLEAR`/`DONT_CARE`, `Keep` is `CLEAR`/`STORE`, `Resume` is `LOAD`/`STORE`,
  never Vulkan's own suspend/resume. A multisampled target draws into its
  companion image and resolves through the attachment's resolve fields at the
  end of every pass, so the texture always holds the resolved picture; a
  sampleable depth on such a target resolves with `SAMPLE_ZERO`, which is what
  Metal does and why the shader fallback D3D12 needed does not exist here.
- **Barriers are hoisted to pass boundaries**, because Vulkan forbids one inside
  a rendering instance. Between passes every colour image rests in a layout a
  pass can sample (`SHADER_READ_ONLY_OPTIMAL`; `GENERAL` for a `computeWrite`
  texture, since a sampler reads that too; the multisample companion stays
  `COLOR_ATTACHMENT_OPTIMAL`), an upload leaves the image there the moment the
  copy is recorded, and a pass moves its attachments in at begin and back out at
  end. One global barrier before `vkCmdBeginRendering` orders every earlier copy
  and dispatch on the recording against the draws, and after that no bind
  records anything. An upload made while a pass is open takes a recording of
  its own, submitted ahead of the frame — the thing `Texture-Windows.cpp` does
  for textures and Metal forbids outright.
- **One descriptor set per draw, elided when nothing changed.** The render set
  is shared by every pipeline (`descriptorBindingPartiallyBound`, so only the
  slots actually bound are written); the uniform block is one
  `UNIFORM_BUFFER_DYNAMIC` at binding 0 with the block's offset in the constant
  ring as the dynamic offset. The emitter writes exactly one block for both
  stages, so `setVertexBytes` and `setFragmentBytes` write the same descriptor
  and the last one wins — safe because `RenderPass::setUniforms` hands both the
  same bytes, and documented at that call site. Samplers travel with the image
  in the descriptor write (`VulkanShared::getSampler`) rather than being
  immutable in the layout, which would have needed a layout per shader.
- **A texture slot has one binding number and two possible descriptor types.**
  The binding map (`Codegen/ShaderBindings.h`) gives a slot one binding whether
  a kernel samples it or writes it, matching the Metal indices; Vulkan gives a
  binding one type. So a compute pipeline reflects its SPIR-V
  (`spirvTextureBindings`) and builds a descriptor-set layout of its own naming
  each declared slot as the `COMBINED_IMAGE_SAMPLER` or `STORAGE_IMAGE` the
  module declared; a kernel that binds no texture shares the one layout in
  `VulkanShared`. A render shader only ever samples, so its set needs no such
  split.
- **Pipelines are built through one `VkPipelineCache`** held by `VulkanShared`
  and persisted to `$XDG_CACHE_HOME/eacp/pipelines-<pipelineCacheUUID>.bin`
  (`$HOME/.cache/eacp/` when unset): loaded at device creation when its header
  names this device, written back through a temp file and rename at teardown,
  and silently skipped on any failure. There is no hash cache above it, because
  a `RenderPipeline` or `ComputePipeline` is one object and one create call
  and nothing in the backend makes an equal one twice. Mesa's lavapipe stores
  nothing in its cache, so on the software lane the file is a 32-byte header.
- **A texture created without pixels is transitioned at creation** into the
  resting layout its use tracking claims, through the same recording an upload
  takes, so a bind before the first write never names a layout the image is
  not in. And a multisampled, sampleable-depth target is refused at creation
  on a device whose depth-resolve modes lack `SAMPLE_ZERO`
  (`VulkanShared::resolvesDepthBySampleZero`) rather than built with an
  undefined read; no such device has been seen.
- **NDC y is the one axis Vulkan differs on**, and the fix is a negative
  viewport height — applied at pass begin, in `setViewport` and in
  `clearViewport`, and nowhere else — so `Winding::CounterClockwise` maps
  straight to `VK_FRONT_FACE_COUNTER_CLOCKWISE`, cull mode and front face are
  baked into the pipeline, and `CullModeTests`, `ViewportTests` and
  `CoordinateSpaceTests` pass unchanged. See `plan.md` §3.5 for why the two
  other fixes are wrong.

### The swapchain

`GPUView` asks `Graphics::requestViewSurface` for a `ViewSurface`
(`Graphics/View/View-Linux.h`) and creates a `VkSurfaceKHR` over the
`wl_display` and `wl_surface` the window backend reports on it. That record —
two opaque pointers, a pixel size, a scale and five hooks — is the whole of what
`eacp-gpu` knows about Wayland: it neither links nor includes libwayland, and
`VK_USE_PLATFORM_WAYLAND_KHR` (`CMake/FindVulkanBackend.cmake`, `PUBLIC` so
`volk.c` sees it too) is what makes `vkCreateWaylandSurfaceKHR` reachable.
`VK_KHR_surface` + `VK_KHR_wayland_surface` on the instance and
`VK_KHR_swapchain` on the device are enabled only where they are offered, and
`VulkanShared::supportsPresentation()` says whether they were — a headless ICD,
or a loader with no WSI, leaves a `GPUView` rendering off-screen exactly as it
did before there was a swapchain.

- **A swapchain image is a `VulkanTextureData` with one flag set.**
  `presentable` makes `restingUse()` answer `PRESENT_SRC_KHR`, so the image
  leaves every pass ready for `vkQueuePresentKHR` and `RenderPass::end` needs no
  swapchain case at all. The acquired image's tracked layout is reset to
  `UNDEFINED` each frame — its contents are undefined anyway — at
  `COLOR_ATTACHMENT_OUTPUT` rather than at no stage, so the first pass's
  transition is ordered behind the acquire semaphore, which is waited on there.
- **The multisample and depth buffers are one pair for the whole swapchain**,
  not one per image: both are scratch space within a frame, and the layout each
  is in is lent to whichever image the frame renders into and taken back
  afterwards. They are built by the same
  `createVulkanMultisampleCompanion`/`createVulkanDepthCompanion` a render-target
  `Texture` uses, so a multisampled window and a multisampled texture cannot
  drift apart. `setSampleCount`/`setDepth`/`setStencil` rebuild them at the next
  frame.
- **One acquire semaphore per frame in flight, one render-finished semaphore per
  swapchain image.** The second half is the one that is easy to get wrong: the
  present waits on the image's semaphore, so a second frame reaching the same
  image while the first present was outstanding would reuse a semaphore that is
  still in play. `framesInFlight()` is clamped to `[1, imageCount - 1]`, and the
  CPU throttle is the context timeline — a slot's acquire semaphore is not
  handed out again until the value its last frame submitted has passed. Nothing
  waits per frame beyond that, and `vkDeviceWaitIdle` happens only on a
  swapchain rebuild and on teardown.
- **Frames are paced by the compositor, not by a clock.** There is no
  `DisplayLink` here. Continuous mode renders, asks for a `wl_surface.frame`
  callback (before the present, which is the commit that carries the request),
  and renders again when `ViewSurface::onFrameDone` says the compositor took the
  last frame — so a hidden or occluded window, which gets no callbacks, renders
  nothing, and the main thread never blocks inside `vkAcquireNextImageKHR`. The
  acquire is given a 100 ms timeout rather than `UINT64_MAX` for the same
  reason. `setMaxFps` uses the divider `DisplayLink::setMaxFps` documents: a
  tick that arrives too early presents nothing and asks for the next callback
  again, and `ViewSurface::requestFrameCallback` commits the subsurface on its
  own when no present will, so the loop carries across the skip with no timer
  beside it.
- **Present modes**: `MAILBOX` where the surface offers it, so a renderer faster
  than the display drops frames instead of blocking; `FIFO` otherwise, which the
  spec guarantees. Format `B8G8R8A8_UNORM` + `SRGB_NONLINEAR` where offered
  (matching what the off-screen snapshot renders into and what the other two
  backends give their swapchains), else the first offered; composite alpha
  `OPAQUE` else the first offered; `minImageCount + 1` images clamped to
  `maxImageCount`; `preTransform` taken as the surface's own. Wayland reports
  `currentExtent` as `0xFFFFFFFF` — there is no server-side surface size — so the
  extent comes from the record's `pixelWidth`/`pixelHeight`.
- **Rebuilds** are marked and done at the next frame, so a live resize that
  reports twenty sizes builds one swapchain: `onResized`, and `OUT_OF_DATE` or
  `SUBOPTIMAL` from either the acquire or the present. A `SUBOPTIMAL` acquire is
  drawn and presented first — it handed over an image and signalled the
  semaphore, and dropping it would leave that semaphore signalled. `onLost`
  destroys the swapchain, the semaphores, the companions and the `VkSurfaceKHR`
  synchronously, before the `wl_surface` goes: a swapchain outliving its surface
  is a use-after-free inside the driver, not an error code.
- **`VK_ERROR_DEVICE_LOST` stops the view and does not restart it.** It is
  logged once, the swapchain and surface are torn down, and `onDeviceRestored`
  never fires — rebuilding the `VkDevice` would mean rebuilding every `Buffer`,
  `Texture` and pipeline made on the old one, and the machinery D3D12 has for
  that (a live-view registry, a device replacement inside `Device::Native`) does
  not exist here. That is the one place this backend is behind the Windows one.
- **`renderNativeContent` is independent of all of it**: it renders into a
  `Texture` of its own through the waiting off-screen `Frame`, works whether or
  not a swapchain is up, and is what the whole headless GPU suite rides on.

### Running it

```bash
cmake -G Ninja -B build
EACP_REQUIRE_GPU=1 EACP_VK_SOFTWARE=1 ctest --test-dir build
```

| Variable | What it does |
| --- | --- |
| `EACP_VK_SOFTWARE=1` | Prefers a `PHYSICAL_DEVICE_TYPE_CPU` device — Mesa's lavapipe. The mirror of `EACP_D3D12_WARP`, and for the same reason: a conformant reference implementation is how you tell your bug from the driver's. It inverts the preference order rather than filtering, so a machine whose only device is a real GPU still gets one. |
| `EACP_REQUIRE_GPU=1` | Makes `GPUTests` fail when no device came up. Every other GPU test self-skips without one and ctest scores that as a pass, so a lane whose driver was never installed reports a full green suite that ran nothing; `DevicePresenceTests` is the one case that does not skip, and it prints the device's name either way. |
| `EACP_VK_VALIDATION=1` | Enables `VK_LAYER_KHRONOS_validation` with a debug-utils messenger that logs warnings and errors through `LOG`. Off by default — the layer costs several times the driver's own time per call. |
| `EACP_REQUIRE_DISPLAY=1` | The swapchain's sibling of `EACP_REQUIRE_GPU`. `Tests/GPU/PresentTests-Linux.cpp` needs a compositor, and every case in it self-skips without one — which ctest scores as a pass. This makes those cases fail instead, so a lane whose Weston session did not come up says so. Set it wherever the suite is run under a compositor; leave it unset everywhere else. |
| `EACP_HEADLESS=1` | Not Vulkan's, but it belongs here: it is what tells the window backend to build no surface, and therefore what the present tests read to decide there is nothing to present to. |

CI runs the suite on lavapipe with `EACP_VK_SOFTWARE`, `EACP_REQUIRE_GPU` and
`EACP_VK_VALIDATION` set and no display server. The lavapipe ICD manifest is
named per architecture (`lvp_icd.x86_64.json` on an x86-64 runner,
`lvp_icd.json` in an arm64 container), so nothing sets `VK_DRIVER_FILES` — the
loader finds it from the ICD directory.

The present tests need a compositor, which the CI container gets from Weston's
headless backend — `with-weston <command>` in the `Dockerfile` runs a command
inside one:

```bash
docker run --rm -e EACP_VK_SOFTWARE=1 -e EACP_REQUIRE_GPU=1 \
    -e EACP_REQUIRE_DISPLAY=1 -v "$PWD":/workspace eacp-ci-linux \
    with-weston ctest --test-dir build-ci-linux --output-on-failure
```
