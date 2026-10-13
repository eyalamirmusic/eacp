# GPU

Metal on Apple platforms, D3D12 on Windows and Vulkan on Linux, behind one
API — and a shader EDSL that makes a shader a C++ struct rather than a string
literal per backend. The same kernel the EDSL records also runs with no device
at all: `CpuCompute/` interprets it on the calling thread ("Running a kernel on
the CPU" below), and `Lib/eacp/ML` lifts tensor-level compute written against
the same `Buffer`s into a Core ML program that the Apple Neural Engine can run
(`Lib/eacp/ML/README.md`).

Everything here is main-thread only, like the rest of eacp, and every public
type hides its backend behind a `Pimpl`, so nothing Metal or D3D leaks into a
header an app includes.

More precisely: everything belongs to the thread that made the `Device` it came
from, and `Device::shared()` belongs to the main thread whichever thread asked
for it first — every `GPUView` and every `Frame` drives that one from there. A
worker that wants the GPU without queueing behind the main thread makes a
`Device` of its own (`auto worker = GPU::Device();`) and keeps the whole chain —
buffers, pipelines, command buffers — on that thread; reaching `Device::shared()`
from there to compile a kernel is allowed and does not move its ownership.
`Device::assertOwningThread()` is the rule as a debug assertion, and creating a
buffer, reading or updating one, beginning a frame, and submitting, waiting on
or reading back a command buffer all call it. It is one thread-id compare behind
an `assert`, so a release build pays for nothing but the call.

## The pieces

| | |
| --- | --- |
| `Device` | The process-wide device and queue. `Device::shared()`, and `isValid()` on a machine with no GPU |
| `GPUView` | A `View` that owns a swapchain and hands you a `Frame` each tick, or on demand from inside another view's render (`renderFrame`) |
| `Frame` | One frame's command buffer. Presents and commits on destruction; `Frame::Redirect` points it at a texture |
| `RenderPass` | Records draws. Ends its encoder on destruction |
| `Buffer` | Vertex, index and storage buffers |
| `Texture` | 2D textures: uploaded, wrapped zero-copy from a camera buffer, or rendered into |
| `RenderPipeline` | A compiled pipeline state |
| `CommandBuffer` / `ComputePass` | The compute path — off-screen, blocking or not; `Frame::beginCompute` puts one on a frame |
| `Codegen/` | The shader EDSL and the MSL / HLSL / GLSL emitters, plus `ComputeKernel`, the device-free half of a `ComputeProgram` |
| `CpuCompute/` | `eacp-cpu-compute`: the interpreter that runs a recorded compute kernel on the CPU, allocation-free, on the calling thread |
| `Spirv/` | `eacp-spirv`: glslang wrapped as a GLSL-to-SPIR-V compiler, built where the Vulkan backend ships it |
| `Vulkan/` | The Linux backend — see "Linux" below |

## A shader

`define()` records a graph of value handles. Nothing in it is text: the emitters
turn that one source into MSL, into HLSL and into GLSL 450 (compiled to SPIR-V
by glslang on Linux), so the three backends cannot drift apart on a shader an
app wrote once.

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

### Vertex formats past the packed five

The EDSL's packed field types (`UNorm8x4`, `Float16x2`, `Float16x4`,
`SNorm16x2`, `SNorm16x4`, in `Codegen/PackedVertex.h`) are the short list, and
each one still reads as a float in `define()`. A hand-written shader — or a
renderer translating someone else's layout, which is what the bgfx backend does
with `AttribType` — names its attributes in a `VertexLayout` instead, and has
the rest of what mesh formats ship: every 8- and 16-bit integer at one, two or
four components, normalised (`UByte2Norm`, `Byte4Norm`, `UShort4Norm`,
`ShortNorm`, ...) or not (`UByte4`, `Short2`, `UShort`, ...), a lone `Half`,
`UInt1010102Norm`, and the 32-bit `UInt`/`Int` at one to four.

```cpp
descriptor.vertexLayout.attribute(VertexFormat::Float3, 0)          // position
    .attribute(VertexFormat::Byte4Norm, 12)                          // packed normal
    .attribute(VertexFormat::UByte4, 16)                             // bone indices
    .attribute(VertexFormat::UByte4Norm, 20);                        // bone weights
descriptor.vertexLayout.stride = 24;
```

**A format without `Norm` is an integer, and the shader declares it as one**:
`uint4 indices [[attribute(2)]]`, `uint4 indices : TEXCOORD2`,
`layout(location = 2) in uvec4 indices` — `int` for the signed ones.
`isIntegerVertexFormat` says which formats those are. That is what D3D12 and
Vulkan hold a pipeline to, a UINT format feeding a float input being undefined
there; Metal also converts an integer format into a float input (`UByte4` as
`float4` reads 0..255), which a shader meant for one backend may lean on and one
meant for three must not. The other way round — a `Norm` format into a `uint4`
— Metal refuses outright, which is the pipeline `29-debugdraw` lost.

A format with fewer components than the shader declares fills in 0, 0, 1, on
every backend. There is still no three-component format below 32 bits, D3D12
having none: pad to four, which is the size bgfx gives such an attribute anyway.
`UInt1010102Norm` holds x, y and z in ten bits each from the lowest and w in the
top two, all normalised; Metal has no integer form of it, so neither does eacp.
`VertexFormatTests` draws every format into an RGBA32F target and reads each
value back exactly.

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
- `saturatingTanh`, which is `tanh` with its two tails answered rather than
  computed: exactly ±1 from an argument of ten outward, the native builtin
  inside it.
  What the native one does with a large argument is the driver's business, and
  eacp compiles its Metal library with no `MTLCompileOptions` — so fast math is
  on, `tanh` is evaluated through `exp`, and `tanh(990)` comes back NaN. A tanh
  GELU cubes its input on the way in, so an activation of thirty arrives as
  exactly that argument. Float32 resolves nothing between `tanh(9.011)` and one,
  so every argument answered with a constant is one whose correctly rounded tanh
  is that constant: it is a repair of the tails, not a different function. Reach
  for it wherever the argument is not bounded by construction
- Statements: `var`, `select`, `ifThen`, `loop`, `breakLoop`, `continueLoop`.
  A `var` takes any handle and any matrix. `select` runs across every family —
  a float, an index, an integer vector, a mask — and takes a literal on either
  side of a scalar one; the condition is a scalar `Bool` in all of them, which
  is the conditional operator both languages already print
- Compute-only: `atomicAdd`, `shared<T>(count)`, `barrier`, `localId`, the
  group-wide `groupSum` / `groupMax` / `groupMin` and their SIMD-group-scoped
  siblings `simdSum` / `simdMax` / `simdMin`, and the SIMD-group matrix —
  `simdGroupIndex`, `simdMatrix`, `multiplyAccumulate` and the `write` that
  stores a fragment — see the compute section
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
comment, because each is a case where the backends disagree about the
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
`ViewportTests` is what holds every backend to it.

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

With `sampleableDepth` too, a later pass reads the plane with
`setFragmentDepthTexture` and a kernel with `ComputePass::setInputDepthTexture`,
once the pass that drew it has ended with `DepthAction::Keep`. The plane has
the target's shape, so an array, cube or cube-array target's reads as one —
`depth2d_array`, `depthcube`, `depthcube_array` in MSL, the matching
`Texture2DArray<float>` on D3D12, `sampler2DArray`, `samplerCube`,
`samplerCubeArray` on Vulkan — level 0 of every slice, the same on all three.

**One slice of it as a 2D depth texture** is the `...Slice` bind:

```cpp
pass.setFragmentDepthTextureSlice(shadowArray, 3, 0);        // layer 3, slot 0
pass.setVertexDepthTextureSlice(cubeTarget, 4, 1, sampler);  // +Z face
compute.setInputDepthTextureSlice(shadowArray, 1, 0);
```

The shader declares a plain `depth2d` (`Texture2D<float>`, `sampler2D`), which
is what a shader written for one shadow map at a time wants when the maps live
in one array. Slice 0 of a 2D target is the ordinary bind; a slice the plane
lacks binds nothing. Metal hands a 2D view of the slice and Vulkan a 2D image
view of the layer, each made on first use and kept; D3D12 has no `TEXTURE2D`
view past slice 0, so it binds a one-slice `TEXTURE2DARRAY` view made with the
target, which a strict driver wants declared `Texture2DArray<float>` and read
at layer 0.

**A depth plane from another target.** A depth plane belongs to the first
colour target and follows its level and slice, which is no help to a pass into
one layer of an array that tests against a plain 2D depth buffer, or into a
volume, which has no depth plane at all. `RenderPassDescriptor::depthTarget`
names the render target whose plane to attach instead, and which image of it:

```cpp
auto shadow = RenderPassDescriptor {};
shadow.depthAction = DepthAction::Keep;
shadow.depthTarget = {&depthOnly, 0, 0};      // texture, level, slice

auto face = ColorTarget {&layers};
face.slice = 2;                                // colour into layer 2

auto pass = frame.beginMultiTargetPass(std::span {&face, 1}, shadow);
```

The depth target is a render target with depth, single-sampled (and so then is
the pass), with that level the pass's size; a layered pass covers its
`layerCount` slices of it from `slice`. Anything else is refused like a
mismatched colour target. It is also how a pass takes an array target's own
plane at another slice than its colour's. A later pass samples the plane
through the depth target, whole or a slice at a time.

### Depth without a fragment stage

A depth prepass, a shadow map or an occluder pass runs no fragment shader at
all. A source whose fragment entry is empty builds a vertex-only library, and a
pipeline over it has a vertex stage alone:

```cpp
auto library = device.makeShaderLibrary(
    ShaderSource::msl(source).withVertex("vertexMain").withFragment({}));

auto pipeline = RenderPipelineDescriptor {};
pipeline.library = &library;
pipeline.depth = true;
pipeline.colorFormat = pixelFormatFor(texture.format);
```

It writes depth and stencil and nothing else: Metal's `fragmentFunction` is
nil, D3D12's `PS` is empty and Vulkan's pipeline has one stage, and on all
three every colour attachment's write mask is forced to none. Depth is a plane
of a colour target, so the pass still has that target and the pipeline still
names its format; it is left as the pass's load found it. A named fragment
entry the source does not have is still an invalid library or pipeline, as
before.

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

### Several targets at once

`Frame::beginMultiTargetPass` renders into up to `maxColorAttachments` (8)
textures in one pass — a G-buffer, an OIT accumulation pair — each target its
own attachment, with its own clear:

```cpp
const ColorTarget targets[] = {
    {&albedo, {0.f, 0.f, 0.f, 1.f}},
    {&normals, {0.5f, 0.5f, 1.f, 0.f}},
    {&history, {}, false},                 // loads what was there
};

auto pass = frame.beginMultiTargetPass(targets, {.label = "gbuffer"});
```

The overload over `std::span<const Texture* const>` clears every target as the
descriptor says, one colour for all. Every target must be a render target, and
all must share a size and a sample count; anything else, one image of a texture
named twice, or more than eight is refused with a trace and the pass records
nothing. Depth, stencil and multisampling are the first target's, as for one
target, unless `RenderPassDescriptor::depthTarget` names another (see "Depth");
every multisampled target resolves into itself as the pass ends. A Vulkan
device offering fewer attachments than asked for refuses the pass too.

One texture may be two targets of a pass when they name different images of it
(see the next section): two layers of an array — a deferred renderer's
G-buffer kept in one — two faces of a cube, two images of a volume, or runs of
slices that do not meet. The same image twice, or two runs sharing a slice, is
refused, and so is a multisampled texture twice, having one image. Metal
attaches each image of the one texture, D3D12 binds an RTV per image, and
Vulkan puts two views of the one image in the rendering info or framebuffer,
the layout tracker moving the image once.

The pipeline describes attachment 0 with the fields it always had
(`colorFormat`, `blendMode`/`blend`, `colorWriteMask`) and attachments 1 and up
in `extraColorTargets`, a `ColorTargetState` each — format, blend and write mask
per attachment, so one attachment can accumulate while the next replaces
(independent blend; on Vulkan a device without `independentBlend` refuses a
pipeline whose attachments differ). The fragment shader writes one output per
attachment: `[[color(n)]]` members of a returned struct in MSL, `SV_Target<n>`
in HLSL, `layout(location = n) out` in GLSL. The shader graph emits one output
today, so a multi-target program is a hand-written `ShaderSource`.

### A level, a slice, or several layers at once

A `ColorTarget` also names which image of its texture the pass draws into:
`level` is the mip, `slice` the layer, the cube face (`layer * 6 + face` on a
cube array) or, on a volume, the z of that level. The pass is that level's size
— `targetWidth()` says so — and the target's depth buffer is attached at the
same level and slice, so a cube's caller-sized chain, a shadow map array or a
volume is rendered one image at a time:

```cpp
auto face = ColorTarget {&environment};    // a Cube with mipLevels and depth
face.level = 2;
face.slice = 4;                            // +Z

auto pass = frame.beginMultiTargetPass(std::span {&face, 1});
```

`layerCount` above 1 is layered rendering: one pass covers that many slices
from `slice`, and the vertex shader picks the layer of each primitive —
`[[render_target_array_index]]` in MSL, `SV_RenderTargetArrayIndex` in HLSL,
`gl_Layer` in GLSL (`GL_ARB_shader_viewport_layer_array`) — counting from
`slice`, so the same shader fills any run. A primitive that writes none lands in
the first. `Device::supportsLayeredRendering()` says whether a vertex shader
may write the index: every Mac and Apple5 on Metal, D3D12's
`VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation`,
Vulkan's `shaderOutputLayer` on the core floor and
`VK_EXT_shader_viewport_index_layer` below it, each enabled where offered.

Every target of one pass takes the same `layerCount`, and the sizes compared
across targets are their levels' sizes. A level, a slice or a run the texture
lacks is refused like a mismatched size, and so is a run on a volume that does
not start at z 0. A volume may now be a render target, without depth: no API
has a 3D depth buffer. A multisampled target has the one level and slice.

Metal attaches the level, slice or depth plane directly and a run through a 2D
array view of it; D3D12 makes an RTV and a DSV per image or run the first time a
pass names it, a `TEXTURE3D` RTV on a volume; Vulkan a 2D or 2D array view of
the image and of its depth companion (a volume is created
`2D_ARRAY_COMPATIBLE`), with `layerCount` on the rendering info and the
framebuffer. A pass rendering into one level while sampling another of the
same texture is not supported: D3D12 and Vulkan track a texture's state and
layout whole rather than per level, so a downscale chain built from passes goes
through a second texture, or runs as kernels (below); a plain filtered chain is
`Frame::generateMipmaps` (see "Mipmaps").

### Redirecting a frame

A renderer written for a `GPUView` finds its target through the frame —
`beginPass()` with no texture, `pixelSize()`, `logicalSize()` — and so draws
only into a drawable. `Frame::Redirect` points the frame's own target at a
texture for as long as it lives, and that same code then renders into the
texture, on the frame's command buffer and in order with everything else the
frame records:

```cpp
void render(Frame& frame) override
{
    {
        auto redirect = Frame::Redirect {frame, panelTexture, backingScale, "panel"};
        drawPanel(frame);      // written against a drawable, unchanged
    }

    auto pass = frame.beginPass();
    pass.draw(compositor);     // samples panelTexture
}
```

While it is in force:

| Call | Answers |
| --- | --- |
| `beginPass(descriptor)` | `beginPass(target, descriptor)` |
| `pixelSize()` | the target's size |
| `backingScale()` | the scale the redirect was given |
| `logicalSize()` | the two above divided |
| `drawableFormat()` | the target's format |
| `copyDrawable(texture)` | a texture-to-texture copy of the target, with no `setReadableDrawable` needed |
| `isValid()` | the recording is open and the target is a valid render target |
| `isRedirected()` | true |

Nothing else changes. `beginPass` on a named texture or on several targets,
`beginCompute`, the copies, `generateMipmaps`, `flush()`, the frame's timings
and `Device::frameIndex` are the frame's as they always are, so a compute pass
the redirected code dispatches feeds its redirected pass exactly as it would
have fed the drawable, and the passes after the scope see what it drew. The
drawable is untouched while a redirect is in force, and nothing presents the
target: it is a texture like any other once the scope ends.

The target is a single-level 2D render target. Its depth, stencil and sample
count are the pass's, exactly as for `beginPass(texture)`, so a pipeline drawing
there is built against the texture's — and above one sample the pass resolves
into it as it ends, so what is sampled afterwards is the resolved picture
("Multisampling a target"). A texture that is not a render target leaves the
frame `isValid()` false.

**A redirect begins and ends with no pass open**, the rule `beginCompute` and
`flush` already state: a pass is bound to the target it began on, and one
spanning the scope would draw into the wrong one on either side. Redirects
nest, each restoring the target and scale before it, and a `flush()` inside one
flushes the whole frame, not the redirected part.

A fourth argument, a timing label, times every pass begun under the redirect —
compute and texture passes included, labelled or not — in
`Device::lastFrameTimings()`: an unlabelled pass as the label itself, a
labelled one as `label/its own`, so one redirected renderer's share of a frame
is the passes whose label starts with its own. Empty, the default, leaves
passes as they are, which is also what an inner redirect with no label does to
the passes under it. The label is a `string_view` and has to outlive the
redirect, and the passes count against the frame's
`GpuTimestamps::maxTimedPasses` like any others ("Timing a pass").

It is a scope rather than a second frame on purpose. A nested `Frame` would
take a command buffer of its own, flush the outer one to begin and present or
wait to end ("Several views in one render"); a redirect is a target, a scale
and a timing scope swapped on the frame that was already open. It is what
`UI::GPUComponent` renders through — the host's one frame, pointed at each
component's texture in turn
(`Lib/eacp/UI/README.md`). The logic is all in `Frame.cpp`; each backend only
implements the native halves (`nativePixelSize`, `beginNativePass`,
`nativeDrawableFormat`, `copyNativeDrawable`, `isNativeValid`).
`Tests/GPU/FrameRedirectTests.cpp` (`FrameRedirect/`) runs it on every backend.

## Compute

A kernel is a `ComputeProgram`: storage buffers and uniforms as members, the
body in `define()`, dispatched over one index per element. Two places take one.

The grid comes from what the body asks for. `threadId()` gives a single index
and is dispatched with `dispatch(count)`, in groups of 64; `threadPosition()`
gives an `x` and a `y` and is dispatched with `dispatch(width, height)`, in 8×8
groups; `threadPosition3()` adds a `z` and is dispatched with
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

### The group a kernel is dispatched in

Those three shapes are the defaults, not the only ones. A kernel that wants a
different group says so before `compile()`, by handing a `ThreadGroupShape` to
the base constructor, and everything downstream follows it: the emitted
`[numthreads(...)]` and `layout(local_size_...)`, and the threadgroup the pass
dispatches in.

```cpp
struct RowSum final : ComputeProgram
{
    RowSum() : ComputeProgram({256}) { compile(); }

    void define() override
    {
        auto lane = localId();                     // now runs to 256
        auto scratch = shared<Float>(groupShape().x);
        ...
    }
};
```

`{256}` is a 1D group of 256; `{16, 16}` is the 2D group a tiled product wants;
a third number is the depth. `ComputeProgram()` — the constructor everything
here used until now — keeps the stock shape, so a kernel that says nothing is
dispatched exactly as before, and `ComputePass::threadGroupWidth`,
`threadGroupSize2D` and `threadGroupSize3D` still spell what "nothing" means.

`groupShape()` is the shape this kernel really has, and is what a shared tile
and a lane's stride are sized against. Read it inside `define()` **after** the
body has asked for its thread index: a kernel that named no shape resolves one
from its rank, and the rank is what `threadId()` or `threadPosition()` fixes.

Wider is not automatically faster. A group-per-row sum over 1500 floats runs
1.7× *slower* at 256 lanes than at 64 when there are 1500 rows to sum, because
64 lanes already saturate memory bandwidth and the extra lanes only add barriers
— and 1.4× faster at 32 rows, where there is not enough work to fill the machine
and a wider group is what hides the latency. It is a knob to measure, not one to
turn up.

The group is also a device limit: Metal reports `maxTotalThreadsPerThreadgroup`
per pipeline, and a kernel asking for more than that is reported at `prepare()`
rather than dispatching nothing.

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

### Waiting for one command buffer

What `Buffer::read()` waits for is the *newest* submission, and so for every
earlier one too. That is the right default — the buffer has no way to know
which submission wrote the bytes being asked for — and it is exactly wrong for
a loop that keeps more than one command buffer in flight: step k's result
cannot be read while step k+1 is running, because the read waits for both.

`wait()` is the scoped version. It blocks until *this* command buffer's work
has finished and for nothing submitted after it, returns at once when the work
has already landed, and does nothing at all on a buffer that was never
committed. `isComplete()` asks the same question without blocking.
`CommandBuffer::read()` is `Buffer::read()` scoped the same way: that wait, and
then the copy.

```cpp
commands.submit();                        // no wait, nothing to resolve
commands.wait();                          // this buffer only
commands.read(output, values.data(), bytes);
```

`submit()` is the third way to hand work over, beside `commit()` and
`commitAsync()`. The `Async` the latter returns settles on the message thread,
so a loop that blocks in `wait()` and never gives that thread a turn would
never see it resolve; `submit()` is that submission with the completion half
left to `wait()`. All three are the same submission, and a second one on the
same command buffer does nothing whichever was used.

Which is what a pipelined loop of small steps is built out of — record and
submit step k+1 while step k is still running on the GPU, then wait for step k
and read it:

```cpp
// Two in the air at a time: one running, one being recorded. A CommandBuffer
// is neither copyable nor movable, so the ring holds them in place.
std::optional<CommandBuffer> inFlight[2];

for (auto step = 0; step < stepCount; ++step)
{
    auto& commands = inFlight[step % 2].emplace(device);

    {
        kernel.step = (unsigned) step;
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.submit();

    // One behind: the CPU has recorded and submitted step k+1 before it asks
    // the GPU for step k, so the two overlap instead of taking turns.
    if (step > 0)
        inFlight[(step - 1) % 2]->read(state, &results[step - 1], sizeof(float));
}
```

Command buffers on one queue run in the order they were submitted, so step k+1
reads what step k wrote without anything being said about it — the dependency
is the queue's, not the caller's. A command buffer whose work is still running
may be destroyed; what may not happen is reading its output without a `wait()`
or a `read()` of its own first.

The scoped read is a memcpy after the wait on Metal, where a storage buffer is
CPU-visible. On D3D12 and Vulkan the copy out of a device-heap buffer is itself
a submission, which the in-order queue puts behind whatever was submitted in
between, so the wait is scoped there and the copy is not. It is still the right
call — it is the only shape a readback has on those backends — but a loop that
reads every step will overlap less there than it does here.

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

**Within one pass, the dispatches are ordered.** Every dispatch sees the writes
of every dispatch recorded before it in the same pass, on all three backends, so
a chain of stages is a chain of `dispatch` calls and does not need a pass each.
Between passes the ordering is the queue's, which is the same promise a second
time.

### Dispatches that overlap

That ordering is not free: a dispatch waits for the one before it to drain even
when the two share nothing, and for a stage whose grid is a few hundred threads
the wait is most of what the stage costs. A chain of sixty small kernels pays it
sixty times.

`DispatchOrder::Concurrent` lifts it, and `ComputePass::barrier()` puts it back
exactly where one stage does read what another wrote:

```cpp
auto pass = commands.beginCompute("attention", DispatchOrder::Concurrent);

for (auto head = 0; head < heads; ++head)
{
    scores.output = BufferRange {&allScores, head * stride, stride};
    pass.dispatch(scores, keys);       // the heads share nothing
}

pass.barrier();                        // every score is written before...
pass.dispatch(combine, width);         // ...anything reads one
```

Everything recorded before a `barrier()` completes — its buffer and texture
writes visible — before anything recorded after it begins. A `barrier()` after
every dispatch is the serial pass again and measurably slower than one, so this
is worth reaching for where there is real independence and not otherwise.

The end of the pass is a barrier of its own: what the next pass, a `fill` or a
`Buffer::read` sees from a serial pass, it sees from a concurrent one.
`barrier()` in a serial pass is a no-op, so a chain can be written once and
switched between the two by its argument alone. `Frame::beginCompute` takes the
same second argument and means the same thing by it, and an unlabelled pass
spells the label it is not giving: `beginCompute({}, DispatchOrder::Concurrent)`.

This is not the `barrier()` a kernel body calls. That one is inside a single
dispatch, across the threads of one group — see **Threadgroup memory** below.

### What a pass recorded

`ComputePass::recorded()` is the pass's own tally: a `Recorded` of
`pipelineSets`, `dispatches` (1D, 2D and 3D alike, the program forms included),
`indirectDispatches` and `barriers`. It counts what was asked for, not what the
backend emitted — a `barrier()` in a serial pass, which records nothing, still
counts, and so does a dispatch the pass dropped for want of a pipeline — so a
chain written once and switched between the two orders counts the same either
way:

```cpp
auto pass = commands.beginCompute({}, DispatchOrder::Concurrent);
chain.recordInto(pass);

check(pass.recorded() == ComputePass::Recorded {.pipelineSets = 3,
                                                .dispatches = 3,
                                                .barriers = 2});
```

The counting is in `ComputePass.cpp`, built on every platform: each public
call bumps an integer and hands over to the backend's `encode*` half, so it is
the same number on Metal, D3D12 and Vulkan and costs a dispatch nothing worth
measuring. It is what a
test pins a net's structure with, where the alternative was faking an encoder.
`CommandBuffer` keeps no total across its passes; a test sums the passes it
began. `Tests/GPU/RecordedCountsTests.cpp` is the suite.

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
after `commit()` or `wait()`, or after the `Async` from `commitAsync()` has
resolved:

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

A frame or a command buffer times its first `GpuTimestamps::maxTimedPasses`
labelled passes, which is 128 — enough to give every kernel of a net a label of
its own. Past that a pass runs exactly as it would have and is simply not
timed, so the tail is missing from the breakdown rather than the buffer being
wrong. The ceiling is a fixed pool of two timestamps per pass, so it costs a
slot 2 KB of samples on Metal and a 258-entry query heap with its readback
buffer on D3D12 and Vulkan — paid only once a labelled pass has asked for it.

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

### Byte counts are 64-bit

Every byte count and offset on the buffer API is a `std::int64_t` —
`Buffer::size()`, the constructor's count, `read` and `update`, `BufferRange`'s
`offset` and `bytes`, `Device::makeBuffer`, `CommandBuffer::read`,
`ComputePass::setBytes` and the indirect-dispatch offset. A single buffer is
routinely past what an `int` holds: a language model's weight shard is
gigabytes, and a batch of logits reaches two of them at a few thousand rows, at
which point an `int` count wrapped silently and allocated something small and
negative instead of failing.

Signed rather than `std::size_t`, so a negative offset arriving from a caller's
own arithmetic stays negative and the guards that reject it keep working, and so
that mixing a count with the `int` element counts the rest of the API uses needs
no cast in either direction. Shader-side indexing is untouched and stays 32-bit:
what is wide is the host's description of the allocation, not the index a thread
computes.

Most call sites need no change — a `sizeof` or an `int` widens on its own. What
does need one is a count read back *out*: `int bytes = buffer.size();` narrows
where `auto` does not.

### A buffer over memory you already have

`Device::makeBufferOverMemory` takes an `ExternalMemory` — a pointer, a length
and a callback — and makes a buffer over those bytes rather than a copy of them:

```cpp
auto mapped = std::make_shared<MemoryMappedFile>(FilePath {weightsFile});

auto weights = device.makeBufferOverMemory(
    {const_cast<std::uint8_t*>(mapped->bytes().data()),
     (std::int64_t) mapped->size(),
     [mapped] {}},                       // holds the mapping open
    BufferUsage::Storage);

kernel.layer = BufferRange {&weights, tensor.offset, tensor.bytes};
```

That is what it is for: one mapping of a large file becomes one buffer, every
tensor in it a `BufferRange`, and the pages arrive from the page cache as the
GPU first touches them. The address must sit on `Buffer::memoryPageSize()`,
which a mapping of a whole file already does; the length may be anything, and
`size()` reports the count given rather than the page it is rounded up to
underneath. `Buffer::isPageAligned` answers the contract before the call, and a
descriptor that fails it makes an invalid `Buffer` rather than a quietly copied
one on every backend — so a call site written on one is one the others take.

`Buffer::canAdoptMemory(device)` says which of the two actually happened. True
on Metal, where a shared-storage `MTLBuffer` is built straight over the host
pages, so the caller and the GPU look at the same bytes in both directions and
nothing is copied; the callback then runs when the buffer is destroyed. False on
D3D12 and Vulkan, whose device heaps are not host memory: the same call copies,
and the callback runs as soon as the copy has been taken. Worth asking before
mapping a file the size of a model, since where it is false the bytes are paid
for twice.

### Writing a buffer the GPU may be reading

`Buffer::update` is ordered after everything submitted to the device before the
call, the same way `read` is: a kernel still writing those bytes has finished
before the host's arrive, and the host's are the ones that stay. The wait is
paid only where the write is a bare memcpy into memory the GPU can see — on
Metal, whose buffers are all shared storage, and on a host-mapped
`BufferStorage::Streaming` buffer anywhere. A device-storage write on D3D12 and
Vulkan is a copy recorded into the command stream, which the stream itself
orders, and waits for nothing.

**That wait is new**, and it is a cost every existing caller now pays: an update
that used to be a bare memcpy on Metal is a memcpy behind a wait for the newest
submission. Code that was already right by construction gets its old cost back
by asking for the unordered call by name — which is what `StreamingBuffers`,
`GPUWidgets`' coverage batch and the `Apps/GPU` samples in this tree were
changed to do.

`Buffer::updateUnordered` is that write with the wait given up, the caller
saying instead that no work the GPU still has in hand touches those bytes. There
are two ways to be able to say it. One is the frame loop: a renderer rewriting
its geometry every tick cannot stop in the middle of a frame to wait for the
newest submission — that is the CPU and the GPU taking turns rather than
overlapping — so it buys the ordering another way. `StreamingBuffers` is that
other way, and never hands out bytes from an arena a frame still in flight was
drawn from, which is why its own writes go through the unordered call. The other
is a caller that has ordered by hand and knows more than a `Buffer` can: it
waited on the command buffer that wrote those bytes, or read them back, or is a
step-by-step loop where the writer finished long ago and only a later, unrelated
submission is still running.

`CommandBuffer::update` is that second case with the wait built in and scoped to
one command buffer — `Buffer::update`'s sibling exactly as `CommandBuffer::read`
is `Buffer::read`'s. It waits for *this* command buffer and then writes, so a
loop keeping two in flight can overwrite step k's buffer without draining step
k+1:

```cpp
commands.submit();                             // step k
trailing.submit();                             // step k+1, still running

commands.update(state, patch.data(), bytes);   // waits for step k alone
```

Where the writer is known, that is the better call than either of the two on
`Buffer`.

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
the same size and in the same order. A kernel that counted 1000 items divides by
the group width the *consuming* kernel was compiled for — its
`groupShape().x`, 64 unless it asked for another — and writes
`(1000 + width - 1) / width`, not 1000. Writing them means writing integers, so
the buffer is a `Uniform<AtomicBuffer>` and `write(arguments, 0u, groups)` is
the store.

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

Threads of one dispatch are ordered against each other by nothing but the end of
that dispatch, so a kernel reading what the previous one counted has to be a
later dispatch — a later pass, as above, or a later dispatch in the same one.

1D only. A 2D or 3D indirect dispatch would take its extents beside an offset
and could not be told apart from this one; nothing has needed it.

### Indirect draws

A draw can take its counts from a buffer the GPU wrote, the render-side twin of
`dispatchIndirect`: a kernel fills `DrawArguments` (or `DrawIndexedArguments`)
records, and the next pass draws from them without the CPU learning a number.

```cpp
{
    auto compute = frame.beginCompute();
    compute.dispatch(cull, objectCount);     // writes DrawArguments records
}

auto pass = frame.beginPass();
pass.setPipeline(pipeline);
pass.setVertexBuffer(vertices);
pass.drawIndirect(arguments);                       // one record at offset 0
pass.drawIndirect(arguments, 0, objectCount, 32);   // a run, 32 bytes apart
pass.drawIndexedIndirect(indices, IndexFormat::UInt16, arguments, offset);
```

The records are Metal's `MTLDraw(Indexed)PrimitivesIndirectArguments`, Vulkan's
`VkDraw(Indexed)IndirectCommand` and D3D12's `D3D12_DRAW(_INDEXED)_ARGUMENTS`:
four 32-bit integers (vertex count, instance count, first vertex, first
instance), or five for the indexed form (index count, instance count, first
index, a signed base vertex, first instance) — 16 and 20 bytes, the same on all
three. Any buffer holds them; a `Storage` buffer is what a kernel writes. The
transition from a kernel's write to an argument read is made for you: D3D12
moves the buffer to `INDIRECT_ARGUMENT`, Vulkan's barrier before rendering
already covers the draw-indirect stage, Metal tracks it itself.

A run of `drawCount` records `stride` bytes apart (zero meaning packed) is one
`vkCmdDrawIndirect` where the device has `multiDrawIndirect`, one
`ExecuteIndirect` on D3D12 (a command signature per argument type and stride,
made once and kept), and a loop of single draws on Metal. A record with zero
instances or vertices draws nothing. Offsets and strides are multiples of four,
the stride is at least one record, and every record lies inside the buffer;
anything else is refused with a trace and draws nothing.

`drawIndirectCount` and `drawIndexedIndirectCount` also read the number of
draws from a buffer (a `uint32`, clamped to `maxDrawCount`):
`vkCmdDrawIndirectCount` on Vulkan where the device has it, `ExecuteIndirect`
with a count buffer on D3D12. Metal has no such draw, so
`Device::supportsIndirectCount()` is false there and the calls are refused with
a trace.

### Occlusion queries

A query counts how many samples the draws inside it got past the depth and
stencil tests, which is what a renderer asks of an object's bounding box before
deciding whether to draw the object itself. The counts land in an
`OcclusionQueries` set — N 64-bit results — that the pass's descriptor names,
because Metal binds them to the pass as it begins:

```cpp
auto queries = OcclusionQueries {device, 64};   // all zero to start

auto pass = frame.beginPass({.occlusionQueries = &queries});
pass.draw(scene);

pass.beginOcclusionQuery(7);                     // OcclusionMode::SampleCount
pass.draw(boundingBox);
pass.endOcclusionQuery();

pass.beginOcclusionQuery(8, OcclusionMode::AnySamples);
pass.draw(otherBox);
pass.endOcclusionQuery();
```

**The results are written as the pass ends and are readable once the GPU has
finished that frame**, which in a frame loop is a frame or more later.
`isComplete()` asks without blocking; `read(span, first)` and `result(index)`
wait for the frame that last wrote the set, where it has been submitted — a
frame still being recorded is not waited for, and a read inside it sees the
previous results. A query no pass began keeps what it held, so one set can be
filled a few indices at a time across passes and frames; bgfx's
`setCondition`, which decides on the CPU from last frame's answer, needs nothing
more. The set is one buffer, not a ring: a renderer reading frame N while frame
N+1 writes the same indices keeps a set per frame in flight, as the frame timer
keeps a slot per frame, or `isComplete()` is asking about the frame still
running.

`SampleCount` is the number of samples — pixels times the target's sample
count — and exact where `Device::supportsOcclusionSampleCounts()` says so: Metal,
D3D12, and Vulkan with `occlusionQueryPrecise`, which eacp enables wherever it
is offered. `AnySamples` is zero or not zero, and the not-zero is not a count.
`Device::supportsOcclusionQueries()` is true on every device of all three.

One query is open at a time, and an index is begun at most once per pass; a
second `begin` while one is open, an index already counted in the pass, one
outside the set, and any query on a pass whose descriptor named no valid set are
refused with a trace and count nothing. A query still open at the end of the
pass is closed there. Metal sets `setVisibilityResultMode` Counting or Boolean
at the query's offset of the pass's `visibilityResultBuffer` and Disabled at the
same offset — Metal writes every offset a mode was set at, so closing at zero
would zero query 0 in every pass. D3D12 begins `OCCLUSION` or
`BINARY_OCCLUSION` queries out of one occlusion heap and resolves the ones the
pass used into a mapped readback buffer as it ends. Vulkan begins queries of an
occlusion pool, with `VK_QUERY_CONTROL_PRECISE_BIT` for a count, copies the ones
the pass used into a host buffer once rendering has ended, and resets them
there for the next use; the first pass naming a set resets the whole pool
before it begins rendering.

### How many buffers a kernel binds

A kernel the codegen layer writes binds at most
`ComputePass::portableBufferSlots` (eight) storage buffers, which every backend
has. A hand-written kernel may bind up to `Device::maxComputeBufferSlots()`:
sixteen (`ComputePass::maxBufferSlots`) on Metal, where storage buffers are
`buffer(0)` … `buffer(15)` under the uniform block at `buffer(16)`; sixteen on
Vulkan unless the device's per-stage storage-buffer limit is lower, with slots
8 … 15 at bindings 17 … 24, above the uniform block, so no binding the first
eight use moved (`vulkanComputeBufferBinding`); and eight on D3D12, whose
compute root signature spends 60 of its 64 DWORDs on two root CBVs, a root SRV
and a root UAV per slot and the texture and sampler tables. A bind at a slot
the device does not have is dropped.

### Threadgroup memory

`shared<T>(count)` is memory one dispatch group has in common: every thread in
the group reads and writes it, no thread outside sees it, and it is gone when
the group is. `localId()` is what indexes it, and `barrier()` is what makes one
thread's writes visible to the rest:

```cpp
void define() override
{
    auto lane = localId();
    auto scratch = shared<Float>(groupShape().x);

    write(scratch, lane, input[threadId()]);
    barrier();

    // every thread now holds what the whole group fetched
    write(output, threadId(), scratch[lane ^ 1u]);
}
```

`groupShape()` is what to size the tile against rather than a literal, since it
follows the shape the kernel asked for — 64 for one that asked for nothing.

**How much there is to spend is a device question, and one the EDSL answers.**
`Device::maxThreadgroupMemory()` is the budget in bytes — Metal's
`maxThreadgroupMemoryLength`, a flat 32 KB at D3D's `cs_5_0`, Vulkan's
`maxComputeSharedMemorySize`, whose spec floor of 16 KB is therefore what a
kernel may assume anywhere. `ComputeProgram::threadgroupMemoryBytes()` is the
other half: what this kernel takes, its `shared<>` arrays plus the scratch the
emitter adds behind them for a reduction and for a SIMD-group matrix, counted as
the worst of the three backends since a kernel is written once.
`fitsThreadgroupMemory(device)` is the two compared, and `prepare()` names an
overspend in the log — both numbers — before the backend reports it as a
pipeline that would not build, which on Metal happens after the library compiled
clean and points at the wrong thing.

Nothing initialises it — what it holds before the group writes it is undefined,
which is why every use starts by filling it and waiting. Reading is a subscript;
writing goes through the same `write()` the buffers and textures use, because a
write is a statement and has to land where it was written.

**A barrier must be reached by every thread in the group or by none.** One
inside an `ifThen` that some threads take and others do not is undefined in both
languages, and undefined here means a hang rather than a wrong answer. Diverging
*after* a barrier is ordinary control flow; diverging *around* one is not.

That rule reaches the dispatch too: a kernel that barriers gets **no** early
bounds guard, since a barrier below a return some threads took is exactly the
divergence the rule forbids. Every thread of every group therefore runs the
whole body, and the dispatch rounds the grid up to whole groups — so the tail of
the last group runs on indices past the data, and the kernel has to hold its own
stores, typically with `ifThen(id < gridCount(), ...)`. Size the grid in
multiples of `groupShape()` where the tail would otherwise compute nonsense, and
guard the writes either way.

The declaration is the one place the backends are not the same shape twice:
MSL's `threadgroup` is a local of the kernel function, HLSL's `groupshared` and
GLSL's `shared` are globals, so the same array lands on opposite sides of the
entry point.

A buffer whose elements are records rather than single floats is read and
written a record at a time. `read2`/`read3`/`read4` take N consecutive floats
starting at `index * N`, and `write` has the matching `Float2`/`Float3`/`Float4`
overloads — the index is in records on both sides, so a kernel over a struct of
four floats never spells the stride:

```cpp
auto particle = state.read4(index);           // position.xy, velocity.xy
write(next, index, float4(newPosition, newVelocity));
```

Underneath, the `write` overloads are N scalar accesses over a buffer that is
still a run of floats. `write2`/`write3`/`write4` lay the same bytes down as
**one** store, at the same record index:

```cpp
write4(next, index, float4(newPosition, newVelocity));   // one store on Metal
```

The two coexist because the wide store is not the trade it was once taken for.
It does not retype the binding — it reinterprets the *address being written*,
which is the same pointer cast `read4` makes at the address being read — so an
output written wide is still a run of floats, still bindable as a per-instance
vertex stream with no CPU-side element size to agree on. The alignment contract
is `read4`'s too: the pointer is a `packed_float4`, wanting four-byte alignment
and not sixteen, so any offset `Device::storageBufferOffsetAlignment()` lets a
`BufferRange` start at is one a wide store can write to. On HLSL and GLSL, which
have nothing to reinterpret, `write4` prints the four subscripts `write` already
prints — over a value named once first, so the whole record is evaluated before
any part of it reaches memory and `write4(out, i, f(out.read4(i)))` means what it
says.

The *read* of a read-only buffer is one load where the dialect has a spelling
for one. On Metal `input.read4(i)` is the sixteen bytes fetched through a
`packed_float4` pointer — packed and not `float4`, because a ranged bind's
offset only has to sit on `Device::storageBufferOffsetAlignment()`, which is
four bytes there, and a `float4` load wants sixteen. HLSL and GLSL have nothing
to reinterpret — a `StructuredBuffer<float>` and an std430 block of floats are
runs of scalars — so they emit exactly the componentwise construct, with the
base index named once. Giving them a real vector load would mean changing the
binding itself: a raw `ByteAddressBuffer` SRV on D3D12, with every scalar read
respelled as `asfloat(buffer0.Load(i * 4))`, or a second block aliasing the same
binding in GLSL. Neither is worth what it would cost the scalar path, which is
what almost every kernel reads through.

`OutputBuffer`'s vector reads stay scalar on every backend, and that is not an
oversight: an output may hold what this very thread stored into it a statement
ago, and the subscript through the pointer that was written is what orders the
two. A load through a second pointer of another type has nothing saying it may
not be hoisted above the store.

A record read is one write above its stores all the same — every component is
stored the value the record held before the first of them ran — so a record read
out of an output and rearranged back into it swaps its components rather than
broadcasting one:

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
beginning the next one — which is a rule about encoders and not about
visibility, since a dispatch already sees what an earlier dispatch in the same
pass wrote. `Apps/GPU/ComputeParticles` is the worked example, and
`Apps/GPU/AsyncCompute` times the two commits against each other.

A `write()` happens **where it is written**: one inside an `ifThen` runs only
when the condition holds, and one inside a `loop` runs every iteration. That is
worth stating because it was not always true — stores used to be collected and
emitted after the body, so a guarded write ran unconditionally and a looped one
ran once afterwards on the counter's final value. Both compiled and neither
complained; `Tests/GPU/StorePlacementTests.cpp` is what now says otherwise.

### What the graph shares, and what it will not move

Two calls that build the same value get the same node, so the emitter prints it
once and names it. Three kinds take that: **constants**, **pure binaries** — the
write's `gid * 4u` and the read's are one node — and **reads of read-only
buffers**. That last one is what makes two `readHalf(scale, i)` calls at one
index a single load rather than two, however far apart in a kernel they were
written, and it is what a hand-unrolled inner loop that fetches the same scale
per lane depends on.

An **output's** reads are never shared, and that is not an omission. An output
may hold what this very thread stored a statement ago — the whole point of
`output[i]` — so two reads of one element with a store between them are two
different values and stay two loads. The slot's declared access is what decides:
an `InputBuffer` cannot be stored to by anything the EDSL can express, so what
its elements hold is fixed for the dispatch.

Which is why **one `GPU::Buffer` must not be bound to an input slot and an
output slot of the same kernel** — a rule `InputBuffer` already states, and one
this sharing now has teeth behind: the emitter orders a read against the stores
to *its slot*, so two reads of an input either side of a store through an output
slot that happens to name the same buffer are merged into one load above that
store. A kernel that computes in place declares one `OutputBuffer` and reads it.

Sharing a node is **not** licence to move it. A node's name is handed out where
the statement being emitted evaluates it anyway, so a read used only inside a
`loop` body or an `ifThen` is named inside that body and issued there — a
loop-invariant read written inside a loop stays inside it, and a read written
under a guard stays under the guard. And a read subscripted by a mutable local
is not shared at all: the index is a `var()` read, which makes the read impure,
which is what keeps a row walk from collapsing into one load of the counter's
first value. `Tests/GPU/HoistingTests.cpp` pins all four of these.

### Reducing over the group

`groupSum`, `groupMax` and `groupMin` are the fold a shared tile was being
hand-written for. Every thread of the group contributes a value, and every
thread gets the reduction over the whole group back — not a partial, and not
only the leader:

```cpp
void define() override
{
    auto i = threadId();
    auto x = input[i];

    auto mean = groupSum(x) / width;
    auto centred = x - mean;
    auto variance = groupSum(centred * centred) / width;

    write(output, i, centred * rsqrt(variance + 1.0e-5f));
}
```

They take a `Float` or a `UInt`, they may be called as many times as a kernel
needs, and the result is an ordinary value: divide it, feed it to the next one,
carry it into a loop or a store.

**Like a barrier, a reduction must be reached by every thread in the group or
by none** — it *is* a barrier, several of them on some backends, so one inside
an `ifThen` that only some threads take hangs rather than answering wrongly.
And for the same reason a kernel that reduces loses its early-return bounds
guard, exactly as one that barriers does: dispatch it over a whole number of
groups and guard the stores against `gridCount()` yourself.

A 1D, a 2D and a 3D kernel all reduce over the *whole* group — 64 threads, an
8×8 tile and a 4×4×4 block are one group each and fold to one number — rather
than over a row or a plane of it.

What each backend emits is the fastest form it has. Metal reduces within each
SIMD group with `simd_sum`/`simd_max`/`simd_min` and combines those few
partials through a scratch array; HLSL under FXC has no wave intrinsic at
`cs_5_0` and GLSL is held to what a driver compiles with no subgroup extension,
so both walk a `groupshared` tree with a barrier per halving step. The scratch
is the emitter's own, declared once per kernel and only where a reduction asked
for it — and on Metal only where a *whole-group* fold asked, since a SIMD-scoped
one needs none.

A group of `simdWidth` threads is not a special case of that: whether it is one
SIMD group is a property of the compiled pipeline
(`ComputePipeline::threadExecutionWidth`) and not of the EDSL, and an Intel Mac
runs a kernel at eight or sixteen lanes. The wide fold therefore keeps its
combine at every group size — `simdCount` is whatever the hardware gave — which
is what it was always for.

**`simdSum`, `simdMax` and `simdMin` are the same three narrowed to one SIMD
group.** Every thread is handed the fold of the `simdWidth` threads it shares a
SIMD group with, so a group of several comes out holding one answer per SIMD
group rather than one for the group. That is what a fold inside a per-tile loop
wants — attention walking its keys a SIMD group at a time has nothing to say to
the rest of the threadgroup yet — and on Metal it is a single instruction with
no threadgroup memory and no barrier, against the two barriers the wide fold
costs.

They are still collective, and the rule is the wide fold's: every thread of the
*threadgroup* reaches one or none does, and a kernel holding one loses its
bounds guard. That is because the other two backends still reach it through the
scratch, narrowed to the folding thread's own block of lanes — HLSL has no wave
intrinsic at `cs_5_0`, and GLSL's `subgroupAdd` is both an extension no lane
here is held to and the wrong fold: a subgroup is whatever width the device says
it is (eight on Mesa's lavapipe), where `simdWidth` is thirty-two everywhere by
construction, exactly as the SIMD-group matrix fixes it. An intrinsic that folded
a different number of threads under the same name would be worse than no
intrinsic. The group has to be a whole number of SIMD groups, or narrower than
one — the same rule a fragment is under, with the one allowance that a group
smaller than a SIMD group *is* its own SIMD group.

**How many threads one folds is the one promise these three cannot keep on
their own.** The emulated backends fold exactly `simdWidth` lanes, because that
is what the emitted tree walks. Metal folds the *hardware* SIMD group, which is
what `simd_sum` is collective over — thirty-two on every Apple GPU, eight or
sixteen on an Intel one, where the same call folds a quarter of what the
arithmetic around it assumes. eacp needs the two to agree and cannot know until
there is a pipeline, so `ComputeProgram::prepare` compares the compiled kernel's
`threadExecutionWidth` against `simdWidth` and says so in the log when they
differ — for a kernel using `simdSum`/`simdMax`/`simdMin` or a `SimdMatrix`.
A kernel that has to be right at any width wants `groupSum`/`groupMax`/
`groupMin` instead.

**Uniform control flow, not merely a uniform arrival.** Every thread must reach
a fold with the same branches taken. `loop(condition, body)` takes an ordinary
per-thread `Bool`, so a fold inside a loop some lanes leave earlier than others
is divergent: the emulated backends hang at the barrier inside the fold, and
Metal's intrinsic tolerates it silently and folds only the lanes still running —
which is the worse of the two, being right on the machine it was written on and
a hang on the next. Bound the loop by something uniform across the group and
guard the body's stores instead.

### The SIMD-group matrix

`SimdMatrix` is an 8×8 patch of a float matrix held between the registers of a
whole SIMD group rather than by any one thread, and `multiplyAccumulate` is the
product of two of them added into a third. It is the primitive a blocked matrix
product is written out of, and on Metal it is one instruction:

```cpp
struct Product final : ComputeProgram
{
    // 256 threads, so eight SIMD groups; each owns a 32 x 16 block of a
    // 64 x 64 tile of C as eight accumulator fragments.
    Product() : ComputeProgram({256, 1, 1}) { compile(); }

    void define() override
    {
        auto tile = shared<Float>(64 * 32 + 32 * 64);
        auto simd = simdGroupIndex();

        auto rowOffset = (simd % 2u) * 32u;
        auto columnOffset = (simd / 2u) * 16u;

        SimdMatrix accumulators[8];

        for (auto& accumulator: accumulators)
            accumulator = simdMatrix();          // a fragment of zeroes

        // ... stage a 64 x 32 slab of A and a 32 x 64 slab of B into `tile`,
        // then barrier() ...

        auto left = simdMatrix(tile, rowOffset * 32u, unsignedInteger(32u));
        auto right = simdMatrix(tile, 2048u + columnOffset, unsignedInteger(64u));

        multiplyAccumulate(accumulators[0], left, right);

        // ... and back out through the same tile, or straight to the buffer
        write(output, at, cRowStride, accumulators[0]);
    }
};
```

Eight calls, and they are the whole vocabulary:

| call | what it is |
| --- | --- |
| `simdGroupIndex()` | which SIMD group of the threadgroup this thread is in, numbered from the flat local index. What places the block of the output a SIMD group owns |
| `simdMatrix(fill)` | a fragment every element of which is that value — the zero an accumulator starts from. `fill` is a literal, not an expression |
| `simdMatrix(tile, offset, rowStride)` | a fragment read from an 8×8 patch of a threadgroup array: element (r, c) at `offset + r * rowStride + c` |
| `simdMatrix(buffer, offset, rowStride)` | the same out of a storage buffer, input or output |
| `simdMatrixHalf(buffer, offset, rowStride)` | a fragment read straight out of a buffer of packed fp16, the offset and the stride counting in halves. An operand only |
| `simdMatrixBFloat16(buffer, offset, rowStride)` | the same for packed bf16 |
| `multiplyAccumulate(acc, left, right)` | `acc += left * right`, over the three fragments |
| `write(buffer, offset, rowStride, fragment)` | the patch written back, addressed the way the load addresses one. `write(tile, ...)` is its threadgroup sibling |

`ComputeProgram::simdWidth` is how many threads a SIMD group holds and
`simdMatrixWidth` the side of a fragment — 32 and 8. Both are constants of the
EDSL rather than of the device: Metal's own are the same numbers, and the
backends that emit the fallback below define theirs to match, so a kernel's
tiling arithmetic is one arithmetic everywhere.

What that costs is four rules, all of them the shape a SIMD-group intrinsic
already has:

- **A fragment is loaded and stored whole.** There is no element of one to
  subscript and no per-element guard to put on one, so the 8×8 patch has to be
  inside the array. A tile hanging off the edge of a ragged shape goes back
  through threadgroup memory and is copied out element by element — which costs
  nothing measurable, since the copy is once per tile against a slab loop.
- **The offset and the stride are the same on every lane.** They address one
  patch for the group, not one per lane. A value derived from `localId()` is
  not one of those; `simdGroupIndex()` and `groupPosition()` are.
- **Every thread of the group reaches every operation on one**, the way it
  reaches a barrier. A kernel holding a fragment therefore loses its
  early-return bounds guard exactly as one that barriers does, and bounds its
  own stores.
- **The threadgroup has to be a whole number of SIMD groups** — a multiple of
  `simdWidth` threads. The stock 64 and 8×8 are; a shape named by
  `ComputeProgram({...})` is asserted on.

**Metal is the fast path and the other two are correctness.** MSL has
`simdgroup_float8x8` and the three intrinsics, so each call above is one line of
emitted source. HLSL at `cs_5_0` under FXC has no wave matrix operation and
neither does GLSL without an extension no lane here is held to, so both spread
a fragment over the 32 lanes of what would have been the SIMD group the way
Metal does: lane *l* holds the pair of elements at row *l* / 4, columns
(*l* % 4) × 2 and the next, as two floats of its own. The fill, the load and the
store are that pair moved, every lane its own, so no store needs a lane picked
to make it. The product is the one operation that needs what other lanes hold:
it stages both operands whole in a `groupshared` scratch of the emitter's own,
one slice per SIMD group, and between two barriers each lane takes its row of
the left against its two columns of the right — the exchange a wave intrinsic
makes in registers, made through threadgroup memory instead. That is why the
third rule above is a barrier's rule: on these two backends every product
holds one. The price is those barriers and the staging, and it is a price
worth naming: a kernel whose throughput matters on Windows or Linux wants the
register-tiled form beside this one, not this one.

The fallback was first written the other way round, the whole 64 floats in
every thread and the product computed 32 times over, and it fell to a limit
FXC keeps that no other compiler here does: every per-thread array counts
against one budget of 4096 registers for the kernel, and the blocked product
below, holding a few dozen fragments at once, went past it — `error X4505`
after seconds of compiling, on every Windows lane. Two floats a fragment is
what keeps a kernel of any reasonable width under that budget.

`Tests/GPU/SimdMatrixTests.cpp` holds the worked blocked product — a 64×64 tile,
a 32-deep slab, clamped loads and a guarded copy-out — checked against a scalar
reference on whole tiles, on a ragged shape and at a transformer's own
[1500, 384] × [384, 1536].

#### A weight read where it lies

A checkpoint ships its weights in sixteen bits, and the product above wants
floats, so a tiled kernel widens a tile of them into threadgroup memory before
it can load a fragment: twice the memory the weights occupy, and two barriers
around the staging. `simdMatrixHalf` and `simdMatrixBFloat16` remove all three.
They read the 8×8 patch out of the packed buffer directly, and the offset and
the row stride count in those sixteen-bit elements — a bf16 weight matrix's row
stride is the number of columns it has, not half of it, which is the convention
`readHalf` and `readBFloat16` already set. The buffer is an ordinary
`InputBuffer`; what is packed is its contents, not its declared type.

On Metal the patch becomes a `simdgroup_half8x8` or a `simdgroup_bfloat8x8`,
loaded through the buffer's pointer reinterpreted, and it stays that type
through the product: MSL's `simdgroup_multiply_accumulate` takes mixed operands
into a float accumulator, so there is no widening step between the load and the
multiply. That is what makes it free — a staged activation against a packed
weight is one instruction.

```cpp
auto accumulator = simdMatrix();
auto activations = simdMatrix(tile, tileOffset, slabStride);
auto weights = simdMatrixBFloat16(weightBuffer, row * columns, columns);

multiplyAccumulate(accumulator, activations, weights);
```

A packed fragment is an **operand and nothing else**. It cannot be an
accumulator — sixteen bits would lose what the sum is being accumulated in —
and it cannot be written back, there being no instruction that stores one.
Both are asserts, not compile errors the shader compiler reports.

**The native path is Metal's alone.** `simdgroup_half8x8` is Metal 2.3 and lands
on the macOS 11 floor eacp builds against; `simdgroup_bfloat8x8` is Metal 3.1
and needs macOS 14 or iOS 17. So there are two queries and not one:

```cpp
if (Device::shared().supportsBFloat16SimdMatrix())
    // build the kernel that loads the weight packed
else
    // build the kernel that stages it into a shared<Float> tile
```

Both are false on D3D12 and Vulkan, which have no wave matrix operation to
lower to. **Both calls still build there and still compute the right thing**:
a packed load becomes each lane widening the two elements it holds, through the
same helper a scalar `readBFloat16` goes through, and the fragment is the float
pair the fallback always was. What the query answers is whether the load is
*native* — one instruction, nothing widened — and so whether it is worth
shaping a kernel around. It is not the question of whether the kernel compiles.

Those are two questions and eacp keeps them apart.
`ComputeProgram::fitsPackedSimdMatrix` is the second one, and it is false only
where the shader would genuinely not compile: on Metal, when the device says
no. On the other two backends it is true regardless.

The choice belongs **outside** the kernel, at the point where it is built, and
never inside one as a branch. The staging path carries barriers that the packed
path does not, and a barrier some threads in a group reach and others do not is
undefined — the two cannot be the arms of one `if`. A Metal kernel built
against the wrong answer is refused by `ComputeProgram::prepare()`, which names
the query and leaves an invalid pipeline, rather than handed to a shader
compiler that would complain about a type instead. A refused program reports
`ComputeProgram::isValid() == false`, and dispatching one is a no-op rather than
a crash — `ComputePass` drops a dispatch whose pipeline never bound.

`EACP_NO_PACKED_SIMD_MATRIX=1` makes both queries answer no on a device that
would have said yes. It is how the staged path stays exercised on hardware that
never takes it, and how a kernel's two shapes can be run against each other on
one machine.

**The float operand is not narrowed** — measured, not promised. A mixed
`simdgroup_multiply_accumulate` could in principle bring both operands to the
packed one's format before multiplying, which would quietly cost the
*activation* eight significand bits and give back more than the staging ever
saved. On an M5 Max under macOS 26 it does not: a left operand of 1 + 2⁻¹²
against eight packed ones accumulates to 8.001953125, where narrowing would
have given a flat 8. `Tests/GPU/SimdMatrixTests.cpp` asserts this for both
formats, so a device or a driver that behaves otherwise fails the suite rather
than silently losing precision. It is a measurement on the hardware to hand and
nothing in the Metal specification requires it, so treat a new part as unmeasured
until the suite has run on it.

### Textures a kernel writes

The other thing a kernel produces is an image, and it reaches the fragment stage
with no new machinery at all: once a `Texture` is written by a kernel, the
`setFragmentTexture` that was always there samples it in a later pass on the
same frame.

A texture opts in the way a render target does, and only in a format the
device can store to. `Device::supportsComputeWrite(format)` is the answer:
every uncompressed format but the sRGB ones and `RGB9E5Float` on Metal, the driver's
typed-UAV-store bit on D3D12, `VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT` on Vulkan —
so `R8Unorm`, `RG8Unorm`, `R16Float`, `RG16Float`, `R32Float`, `RG32Float`,
the 16-bit unorms, `RGB10A2Unorm`, `RG11B10Float` and the `Uint` and `Sint`
formats on most hardware, beside the three every device takes
(`RGBA8Unorm`, `RGBA16Float`, `RGBA32Float`, which the free
`supportsComputeWrite(format)` names for code that cannot ask). Notably
**never** `BGRA8Unorm`, the drawable's own format and the first one most
people reach for, nor an sRGB or a block format, nor `RGB9E5Float`; asking for one the device
refuses yields an invalid texture rather than a kernel whose writes go nowhere.

Past that set the device decides, and `Device::supportsComputeWrite(format)`
says what it decided: `R8Unorm` is writable on Metal, on a Vulkan device
reporting storage-image support for it, and on a D3D12 device whose driver
reports a typed UAV store to it (which feature level 11.0 requires). A kernel
writes a `float4` either way and a one-channel texture keeps the first
component, which is how the UI's coverage atlas holds one byte per texel where
it can and four where it cannot.

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
distinct indices.

Reading goes through `setInputTexture` in any format. Reading the texel a
kernel is about to write *through the bind it writes with* — `access::read_write`
in MSL, a load from an `RWTexture2D` in HLSL, `imageLoad` from an `image2D`
declared with the format's qualifier in GLSL — is a second question, and
`Device::supportsComputeReadWrite(format)` answers it: Metal's read-write tier
decides there (tier 1 is `R32Float`, `R32Uint` and `R32Sint`; tier 2, every
Apple-silicon Mac, adds `RGBA8Unorm`, `RGBA16Float`, `RGBA32Float`, `R16Float`
and `R8Unorm` and the `Uint` and `Sint` formats of those shapes; no tier takes a
two-channel or a packed format), D3D12's
`D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD`, and on Vulkan the storage-image bit
again. That is the per-format `IMAGE_READ`/`IMAGE_WRITE` pair bgfx's caps
want. An integer image is declared `texture2d<uint, ...>`,
`RWTexture2D<uint>` or `uimage2D` with `r32ui`, and never filtered.

A kernel reads a render target's **depth** with
`setInputDepthTexture(renderTarget, slot)` — the compute twin of
`setFragmentDepthTexture`, on the input slots: the target was created with
`sampleableDepth`, the pass that drew it ended with `DepthAction::Keep`, and the
kernel declares the target's kind as a depth texture (`depth2d`,
`depth2d_array`, `depthcube`; `Texture2D<float>`, `Texture2DArray<float>`;
`sampler2D`, `sampler2DArray`, `samplerCube`) and gets one float per read. That
is how a hi-Z pyramid's first level is copied out of the scene and how an
ambient-occlusion pass starts. `setInputDepthTextureSlice(renderTarget, slice,
slot)` hands it one slice of an array or cube target's plane as a `depth2d`
instead, as the fragment stage's slice binds do (see "Depth").

**A cube is written as a 2D array.** No shading language has a writable cube
type, so a kernel declares `texture2d_array<float, access::write>`
(`RWTexture2DArray`, `image2DArray`) and the pass binds the cube through
`setOutputTextureAsArray(cube, slot, level)` — its six faces, or a cube
array's `layer * 6 + face`, as array slices at one level. A plain
`setOutputTexture` binds the cube *as a cube*, which a 2D-array declaration does
not match (a Metal validation error). `setInputTextureAsArray` is the read
twin, and the draw that samples the result binds it as the cube it is:

```cpp
auto pass = frame.beginCompute();
pass.setPipeline(paintFaces);
pass.setOutputTextureAsArray(sky, 0);
pass.dispatch(sky.width(), sky.height(), 6);   // gid.z is the face
```

A `TwoDArray` binds through these as through the level binds; a `TwoD` or
`ThreeD` texture binds nothing.

A kernel can read and write single mip levels too, which is what a hi-Z or bloom
downscale is made of: `setInputTextureLevel(texture, slot, level)` and
`setOutputTextureLevel(texture, slot, level)` bind one level and every slice, as
the kind's own type, so level 0 of what the shader sees is `level` of the
texture. One dispatch per level, reading N - 1 and writing N:

```cpp
for (auto level = 1; level < hiZ.mipLevels(); ++level)
{
    auto pass = frame.beginCompute();
    pass.setPipeline(downscale);
    pass.setInputTextureLevel(hiZ, 0, level - 1);
    pass.setOutputTextureLevel(hiZ, 1, level);
    pass.dispatch(mipExtent(hiZ.width(), level), mipExtent(hiZ.height(), level));
}
```

Metal binds a texture view of the level, Vulkan a sampled or storage view of it,
D3D12 an SRV and a UAV of each level made with the texture (a render target or a
`computeWrite` one; anything else binds level 0 only). On D3D12 a read and a
write of one texture in a dispatch put one resource in two states, which its
whole-resource tracking cannot express: there the lower level is read
through a second `setOutputTextureLevel` and an `RWTexture2D` load, which
Metal and Vulkan take as well. `Apps/GPU/ComputeImage` is the worked example: a kernel paints
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
mean two different things on two of the backends. What *is* portable, and what a
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
| `input.readHalf4(i)` | four halves as a `Float4` — the two words starting at word `2 * i`, so `i` counts records of four and element `k` is `readHalf4(k / 4)` component `k % 4` |
| `unpackHalf2(bits)` | the same, from a `UInt` already in hand |
| `packHalf2(pair)` | two floats narrowed and packed into a `UInt` |
| `writeHalf2(out, i, pair)` | that word stored at `i` — `readHalf2` reads it back |
| `writeHalf4(out, i, quad)` | four halves narrowed into two words and laid down in one store at the index `readHalf4` counts in |
| `asUInt(f)` / `asFloat(u)` | a value's bits rather than its value, both ways — componentwise over a `Float2/3/4` and a `UInt2/3/4` as well as over the scalars |

Size the buffer in whole words: `readHalf` fetches the word at `i / 2`, so an
odd count of halves reads past its last byte on the final element. `readHalf4`
and `readBFloat16x4` want a whole number of *pairs* of words for the same
reason.

The four-wide pair is the width a weight walk wants, and it is not four
subscripts: both take their two words through `read2`, so on Metal the eight
bytes arrive in one `packed_float2` load and the unpacking is register
arithmetic over what came back. HLSL and GLSL fetch the two words as two
scalars, which is what their float buffers give — see the vector reads above
for why neither can reinterpret one.

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

### bf16 weights, kept packed

The same storage trick for the other 16-bit float, which is what a modern
checkpoint actually ships — Gemma, Llama and Mistral are all bf16 on disk.
bfloat16 is fp32 with the low sixteen mantissa bits dropped: eight exponent
bits against fp16's five, and seven mantissa bits against ten. The family
mirrors the fp16 one call for call:

```cpp
void define() override
{
    auto i = threadId();
    write(output, i, weights.readBFloat16(i) * input[i]);  // bf16 in, fp32 out
}
```

| call | what it gives |
| --- | --- |
| `input.readBFloat16(i)` | element `i` of a buffer of bfloat16s, widened to a `Float`. `i` counts bfloat16s |
| `input.readBFloat16x2(i)` | both bfloat16s of word `i` as a `Float2`, `.x` the low bits |
| `input.readBFloat16x4(i)` | four bfloat16s as a `Float4` — the two words starting at word `2 * i`, on the terms `readHalf4` sets |
| `unpackBFloat16x2(bits)` | the same, from a `UInt` already in hand |
| `packBFloat16x2(pair)` | two floats narrowed and packed into a `UInt` |
| `writeBFloat16x2(out, i, pair)` | that word stored at `i` — `readBFloat16x2` reads it back |
| `writeBFloat16x4(out, i, quad)` | four bfloat16s narrowed into two words and laid down in one store at the index `readBFloat16x4` counts in |

**Do not reach a bf16 weight through the fp16 path.** The two are not
interchangeable storage: with five exponent bits, fp16 flushes 1e-6 to a
subnormal and takes 1e30 to infinity, both of which are ordinary bf16 values.
A checkpoint routed through `readHalf` is not less precise, it is wrong.

Widening is a shift and a bitcast — the sixteen bits back at the top of a word,
nothing to rebias and no subnormal case — so it is exact and **bit-identical on
every backend**, which the fp16 widening is too. Unlike `packHalf2`, the
narrowing is bit-identical as well: no dialect has a bf16 instruction to hand
the rounding to, so `packBFloat16x2` does round-to-nearest-even in integer
arithmetic itself, and every backend emits the same arithmetic. A NaN is
quieted rather than rounded, so it cannot carry into the exponent and come back
as an infinity. `bfloat16FromFloat` / `bfloat16ToFloat` in `PackedScalars.h` are
the host side of the same encoding, for filling a buffer or checking one.

### int8 and int4 weights, kept packed

The same storage trick again, for weights that are not floats at all. A
block-quantized checkpoint stores a run of small integers and one scale per
block, so what a read owes is the **integer, exactly** — the scale is a multiply
the kernel does, since which scale belongs to which run is the format's business
and not a buffer read's:

```cpp
void define() override
{
    auto i = threadId();
    auto block = i / 32u;

    write(output, i, weights.readInt8(i) * scales[block]);  // int8 in, fp32 out
}
```

| call | what it gives |
| --- | --- |
| `input.readInt8(i)` / `readUInt8(i)` | element `i` of a buffer of bytes, widened to a `Float`. `i` counts bytes, so an N-weight buffer is walked `0..N-1` |
| `input.readInt8x4(i)` / `readUInt8x4(i)` | all four bytes of word `i` as a `Float4`, `.x` the low eight bits. `i` counts words, and element `k` is `readInt8x4(k / 4)` component `k % 4` |
| `input.readInt8x8(i)` / `readUInt8x8(i)` | eight bytes as a `Float4Pair`, one eight-byte load. `i` counts records of eight |
| `input.readInt8x16(i)` / `readUInt8x16(i)` | sixteen bytes as a `Float4Quad` — `.a .b .c .d` in address order, one sixteen-byte load. `i` counts records of sixteen |
| `input.readInt4x8(i)` / `readUInt4x8(i)` | the eight nibbles of word `i` as a `Float4Pair` — `.low` is nibbles 0..3 and `.high` nibbles 4..7. `i` counts words |
| `input.readInt4x16(i)` / `readUInt4x16(i)` | sixteen nibbles — the same eight bytes, one load — as a `Float4Quad`. `i` counts records of sixteen |
| `unpackInt8x4(bits)` / `unpackUInt8x4(bits)` | the same, from a `UInt` already in hand |
| `unpackInt4x8(bits)` / `unpackUInt4x8(bits)` | likewise for the eight nibbles |
| `packInt8x4(values)` | four `Int4` components packed into a `UInt`, `.x` in the low eight bits |
| `packUInt8x4(values)` | the same from a `UInt4` |
| `writeInt8x4(out, i, values)` / `writeUInt8x4(out, i, values)` | that word stored at `i` — `readInt8x4` reads it back |
| `writeInt8x8(out, i, low, high)` / `writeUInt8x8(out, i, low, high)` | eight bytes packed into two words and laid down in one store at the index `readInt8x8` counts in |
| `writeInt8x16(out, i, a, b, c, d)` / `writeUInt8x16(out, i, a, b, c, d)` | sixteen bytes packed into four words and laid down in one store at the index `readInt8x16` counts in |

The wide byte stores take integer vectors rather than the `Float4Pair` and
`Float4Quad` their reads hand back, and they carry the read's own component
names — `low`/`high`, `a`/`b`/`c`/`d`. It is the same reason `writeInt8x4` takes
an `Int4`: rounding a float back down to a byte is the caller's decision, and a
store that took floats would make it silently.

Signed values are two's complement: a byte over `[-128, 127]`, a nibble over
`[-8, 7]`. Size the buffer in whole words — `readInt8` fetches the word at
`i / 4`, so a count of bytes that is not a multiple of four reads past the last
one on the final element.

Every read in the family is on one index convention: `i` counts records of
whatever the name's last number says, and every record is the elements
`width * i` through `width * i + width - 1` of the buffer. So element `k` is
`readInt8(k)`, and component `k % 16` of `readInt8x16(k / 16)`, and the tests
assert exactly that.

### Pick the widest read, not the widest type

`readInt8x4` is correct and slow, and the reason is worth stating because it is
not obvious from the call: **four bytes is a four-byte load**. `readBFloat16x4`
fetches eight bytes in one load for its four weights, so an int8 kernel written
with `readInt8x4` issues the same number of loads as the bf16 kernel it replaced
for half the data — and stops being bound by bandwidth and starts being bound by
how fast it can issue loads. Halving the bytes then buys a fraction of what it
should. Measured downstream on gemma-2b: bf16 at 427–478 GB/s, int8 through
`readInt8x4` at 298–335 GB/s, a 1.32x decode speedup where halving the bytes
predicts 1.88x.

`readInt8x8` and `readInt8x16` are the fix. They take their words through one
record read — `read2` and `read4` — so the eight or sixteen bytes arrive in a
single vector load wherever the backend has one (a `packed_float2` or
`packed_float4` pointer on Metal, the componentwise construct on HLSL and GLSL),
and the widening is register arithmetic over the value it brought back.
`readInt8x16` is the sixteen-byte load `read4` already lowers to, holding
sixteen weights instead of four.

This is not something to leave to the shader compiler. The graph does share
reads of read-only buffers (see below), so four subscripts of *one* address are
one load — but four subscripts of four consecutive addresses are four different
values and stay four loads, which is exactly what walking a row byte by byte
does. One record read is one node instead, and the emitter names any node it
evaluates more than once, which is what puts the whole record in a local with
the four words as swizzles of it. `GPU/codegenWideInt8Reads` asserts the emitted
MSL has exactly one buffer subscript for a `readInt8x16`, and
`PackedQuantized/aWideReadCostsOneRecordRead` asserts on the real
`ComputeProgram` path that sixteen bytes cost what a `read4` of plain floats
costs.

`Float4Pair` is what an eight-wide read hands back, and `Float4Quad` a
sixteen-wide one, because no dialect has a float8 and inventing one in the EDSL
would leave nothing to emit it into. They are single structs rather than a
`readInt4x8Low` beside a `readInt4x8High` because one fetch should read as one
call — the sharing below would now collapse the two loads either way, but a call
site that fetches once should say so. One read, unpacked in registers:

```cpp
auto w = quantized.readInt4x8(block);
auto sum = dot(w.low, activations.read4(block * 2u))
         + dot(w.high, activations.read4(block * 2u + 1u));
```

`Float4Pair` names its halves `.low` and `.high`; `Float4Quad` letters its four
`.a .b .c .d` rather than inventing a `.lowMid`, which would read as an ordering
of magnitudes where this is an ordering of addresses. Inside a `Float4` the
components already run `x, y, z, w` in the order the bytes sit, so across the
quad `q.a.x` is the record's first element and `q.d.w` its sixteenth:

```cpp
auto w = quantized.readInt8x16(block);
auto sum = dot(w.a, activations.read4(block * 4u))
         + dot(w.b, activations.read4(block * 4u + 1u))
         + dot(w.c, activations.read4(block * 4u + 2u))
         + dot(w.d, activations.read4(block * 4u + 3u));
```

The widening is **bit-identical on every backend**, and that is why the sign
extension is written the way it is. A byte read as unsigned stands for
`(b ^ 0x80) - 128` and a nibble for `(n ^ 0x8) - 8` — arithmetic on values no
wider than the word, moving no bit into or out of a sign position, which MSL,
HLSL and GLSL all define identically. `as_type<char4>` would be Metal's answer
and nothing else's, and shifting a byte up into the sign bit and arithmetically
back asks each language what its own sign bit does under a shift. The same
arithmetic serves Metal and DirectX from one helper string, as `eacpErf` does.

There are four widening helpers for the whole family and no more:
`eacpUnpackInt8x4` and `eacpUnpackInt4x4` and their unsigned twins. The wide
reads are those helpers applied once per word of the record, so a
`readInt8x16` emits one load and four calls, and a `readInt4x16` one load and
four calls of the nibble helper — the high nibbles of a word being the same
helper over the word shifted down sixteen, which is a shift node in the graph
rather than a fifth helper.

`packInt8x4` takes an `Int4` rather than a `Float4` on purpose: narrowing a
float to an integer is a rounding decision and the three dialects each make
their own, so a kernel that has floats rounds them itself — with `round()` or
`floor()` — where the choice is visible. Only the low eight bits of each
component are stored, so a value outside the range wraps rather than saturating;
quantization clamps before this point and nothing is spent re-clamping in the
shader.

`int8x4FromBytes` / `uint8x4FromBytes` / `int4x8FromNibbles` /
`uint4x8FromNibbles` in `PackedScalars.h`, with `int8x4ToByte` and
`int4x8ToNibble` going the other way, are the host side of the same layout —
what a loader turning a quantized checkpoint into a storage buffer writes. They
are per word, and that is all the wide reads need: a record is a run of
consecutive words, so a buffer packed one word at a time reads back through
`readInt8x16` in the order it was written.

## Running a kernel on the CPU

`eacp-cpu-compute` (`CpuCompute/`, namespace `eacp::GPU::CpuCompute`) runs a
compute kernel on the calling thread, over plain arrays, with no device at all.
It is an interpreter over the graph the kernel already recorded — the one the
emitters print — so the kernel the GPU runs and the kernel the CPU runs are one
source and cannot drift. Two things want it. An audio plugin or a DSP process
whose filter bank is a `ComputeProgram` wants that same kernel inside its audio
callback, synchronously and without allocating, and on a machine where no
device came up — a Linux box with no Vulkan driver, where `ComputePass` drops
the work. And every numeric check of the emitters used to need a device; the
interpreter is a second oracle that runs on every CI lane and, where a device
exists, is cross-checked against the GPU's readback of the very same kernel. It
is not a replacement for the GPU on throughput.

The target links `eacp-gpu-codegen` and nothing device-related, so it builds
everywhere, a driverless Linux box and `-DEACP_BUILD_GRAPHICS=OFF` included. It
is optimised in every configuration, Debug too — `eacp_force_optimization`'s
`-O3 -ffp-contract=off` plus `-fno-math-errno` — since an interpreter at `-O0`
is of no use to an audio thread.

### The kernel, without the device

`ComputeProgram` is two classes. `ComputeKernel` (`Codegen/ComputeKernel.h`) is
everything that is not a device: the members, `define()` and its whole
vocabulary, `compile()`, `graph()`, `source()`, `groupShape()` — and it links
against `eacp-gpu-codegen` alone. `ComputeProgram` derives from it and adds
`prepare()`, the pipeline and the bind. So every existing kernel already *is* a
`ComputeKernel`, the executor takes it as it stands, and nothing about writing
or dispatching one on the GPU changed. A product that never touches a GPU
derives from `ComputeKernel` instead, writes the struct identically, and links
no `eacp-gpu`.

```cpp
struct GainKernel final : ComputeKernel     // or ComputeProgram: the same
{
    GainKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * gain);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<Float> gain;
    EACP_SHADER(input, output, gain)
};

// Once, off the audio thread: the plan and every word of its scratch.
auto kernel = GainKernel {};
auto executor = CpuCompute::Executor {kernel};
auto bindings = CpuCompute::Bindings {};

// Every block, on the audio thread: nothing here allocates.
bindings.set(kernel.input, std::span {in, frames});
bindings.set(kernel.output, std::span {out, frames});
kernel.gain = level;
executor.dispatch(bindings, static_cast<int>(frames));
```

### Binding and dispatching

`Bindings` is a fixed table of `ComputePass::portableBufferSlots` slots — eight,
the limit every GPU backend shares, kept so a kernel that runs here also runs
there — filled by typed
`set` overloads that take the kernel's own member, so the slot comes from its
handle: a `std::span<const float>` for an `InputBuffer`, `std::span<float>` for
an `OutputBuffer`, and `std::uint32_t` spans for `UIntInputBuffer`,
`UIntOutputBuffer` and `AtomicBuffer`. A `set` stores a pointer and a count and
nothing else, so a callback rebinds every block for free; the memory stays the
caller's and must outlive the dispatch. The handles `ShaderBuilder` hands back
take the same overloads, so a bare graph binds the same way. The check is at
dispatch, not at `set`: a slot the kernel reads or writes that is unbound, or
bound with the wrong element type or access, refuses the dispatch. A slot bound
to an empty span runs, reading 0 and dropping stores.

`Executor {kernel}` is the plan plus one workspace. The constructor decodes the
graph once, refuses what it cannot run, and allocates every word a dispatch
touches. A refusal is `isValid()` false with a `reason()` naming the construct,
logged once, there — and every dispatch then returns false. It refuses
textures (`Sample`, `Fetch`, a `WritableTexture2D` store), a render graph, more
than eight storage buffers, uniforms over 4 KB (`Plan::maxUniformWords`,
Metal's `setBytes` limit), a group over 2^20 threads, a SIMD-group matrix in a
group that is not a whole number of 32-lane SIMD groups, and a store into a
read-only slot. `Executor {graph}` takes a bare `ShaderGraph` instead, with its
uniforms set by slot through `setUniform`. The kernel must outlive the
executor, which is neither copyable nor movable.

The dispatch forms are `ComputePass`'s: `dispatch(bindings, count)`, `(bindings,
width, height)` and `(bindings, width, height, depth)` in threads, rounded up to
whole groups of `groupShape()` behind the same bounds guard, and
`dispatchIndirect(bindings, arguments, guardCount, offsetInElements)` for a 1D
kernel, reading `DispatchArguments` group counts from a `std::span<const
std::uint32_t>` another CPU dispatch may have written. Each returns whether it
ran: false only for an invalid plan, a kernel of another rank or a bad slot. An
extent of zero, or arguments holding no whole `DispatchArguments` at the
offset, run nothing and return true. Uniforms are read at dispatch: the executor
walks the kernel's members every time, so `kernel.gain = …` a moment before is
what it sees.

A dispatch can be split over threads of the caller's. `prepareDispatch` (and
`prepareDispatchIndirect`) takes the same arguments, resolves the bindings,
sizes the grid and copies the uniforms into a `PreparedDispatch` — a plain
value whose `groupCount()` groups are numbered x fastest — and `dispatchGroups
(prepared, first, count, workspace)`, which is `const`, runs any range of them.
The range is clamped to `[0, groupCount())`, so equal shares with a short last
one need no arithmetic at the edge. Each thread brings a `Workspace` of its own,
made from `executor.plan()` ahead of time. A workspace made from another plan
or a dispatch prepared by another executor is refused — false, nothing run —
since each plan carries a serial both record. There is no pool: the threads are
the host's workers or a render thread. `dispatch` itself is one prepare plus
`dispatchGroups` over the whole range in the executor's own workspace.

### What each construct becomes

A thread group runs in lockstep: every value is an array over the group's
lanes, each statement runs for all of them before the next, and `if` and
`loop` narrow an execution mask. Arithmetic runs on every lane, masked or not —
which is why every operation below is total — and only stores, atomics and
assignments are masked.

| Construct | On the CPU |
| --- | --- |
| `threadId`, `localId`, `groupId` | lane `l` is the flat local index, x fastest, as the emitter's `lane` is |
| the bounds guard | lanes past the extent start inactive; a kernel with a barrier has no guard and every lane of a partial group runs, as on the GPU |
| `shared<T>` | one array per group, zero-filled at group start |
| `barrier()` | nothing: statement order already is the barrier |
| `groupSum`, `simdSum`, `simdMax`, … | a fold over the group or each 32-lane block in the fallback's halving order, so a float sum rounds as it does on D3D12 and Vulkan |
| `atomicAdd`, atomic loads | `std::atomic_ref`, relaxed, lanes in ascending order; each lane gets the pre-add value |
| `simdGroupIndex()` | `lane / 32` |
| the SIMD-group matrix | a dense 8×8 per SIMD group; the product is the accumulator plus the terms in ascending `k`, unfused; packed half and bf16 loads widen as they load |
| `eacpErf`, the packed reads, … | the public C++ twins in `CpuCompute/Helpers.h` (`errorFunction`, `readHalf`, `packHalf2`, …), which the tests' references use too |
| the math set | the `std::` function per lane, IEEE single precision, never contracted |

What the GPU leaves undefined, the CPU defines:

| Case | GPU | CPU |
| --- | --- | --- |
| buffer read out of bounds, or from an empty span | Metal undefined; D3D12/Vulkan read 0 | 0 |
| buffer store or atomic out of bounds | Metal undefined; D3D12/Vulkan drop it | dropped; the atomic yields 0 |
| shared or constant array out of bounds | undefined | reads 0, store dropped |
| integer `/` or `%` by zero | undefined, or all-ones on D3D | 0 |
| `INT_MIN / -1`, `INT_MIN % -1` | undefined | `INT_MIN`, 0 |
| shift by 32 or more | masked by the hardware | amount `& 31` |
| float to int out of range, NaN | saturates | saturates; NaN is 0 |
| `round` at an exact half | Metal away from zero; D3D12/Vulkan to even | away from zero |
| a barrier under divergent control flow | undefined | nothing |
| a reduction under divergence | undefined | inactive lanes give the fold's identity; only active ones receive it |
| a SIMD-matrix op under divergence | undefined | the whole fragment, with the first active lane's operands, if any lane is active |
| threadgroup memory and fragments at group start | uninitialised | zero |

### The realtime contract

Constructing an `Executor`, a `Plan` or a `Workspace` allocates, and so does
the kernel's own constructor, which records the graph; all of it belongs off
the audio thread. `Bindings::set`, `setUniform`, every `dispatch` form,
`prepareDispatch` and `dispatchGroups` allocate nothing, take no lock, log
nothing and make no system call —
`Executor/aDispatchAfterTheFirstAllocatesNothing` and its siblings replace the
global `operator new` to prove it. What breaks the contract is making a
workspace per call rather than per thread, sharing one executor's `dispatch`
between threads (only `dispatchGroups` is `const`), and a denormal-heavy signal
on x86: the executor leaves the thread's flush-to-zero state as the host set
it.

`plan().footprintBytes()` is exactly what one workspace costs. Scratch is
shared between nodes whose lifetimes do not overlap, and a kernel with no
group-scope feature — no barrier, shared array, reduction, SIMD group or
atomic — runs several consecutive groups as one batch of about
`PlanOptions::targetBatchLanes` lanes, 1024 by default, which is most of the
interpreter's speed on a stream and multiplies its footprint by up to sixteen:
the benchmark's tone generator is 68 KB at the default and 3.9 KB at one group
per batch. `PlanOptions::maxBatchBytes` (192 KB) halves the batch until it
fits. A plugin holding many kernels should pass
`CpuCompute::Executor {kernel, CpuCompute::PlanOptions {1}}`, one group per
batch. Measured on Apple silicon, Release, over a million elements, the
interpreter takes 0.97–1.1× the time of a hand-written C++ loop for a tone or
a mix and 3.7–5.1× for a crossfade or a smoothing filter, and 8–12× for path
binning over a large scene, whose lanes diverge.

### What it will not tell you

It is not bit-exact with the GPU and does not try to be: Metal compiles with
fast math, which may assume no NaN and flushes denormals, while the CPU is IEEE
with no contraction, so cross-checks compare within each suite's tolerance.
Binding the same memory to an input slot and an output slot is unsupported on
both — the GPU keeps a read hoisted across another slot's store
(`InPlace/aReadIsNotRefreshedByAnotherSlotsStore`), the CPU re-reads it per
statement. The CPU is not a race detector either: lockstep with every
statement evaluated before it commits turns a race inside a group into a
defined answer, the last lane winning. Across groups, serial groups look
sequential, a wide batch interleaves its groups by statement — so a different
`targetBatchLanes` can change a racy kernel's answer — and `dispatchGroups` on
several threads makes the order as real and as nondeterministic as a GPU's. A
race-free kernel gives the same bits every way. A barrier under divergent
control flow does nothing here; a kernel that relies on it doing more is
already broken on the GPU. And the uniforms are read on whichever thread
dispatches, unsynchronised, like any plain member: set them on one thread and
dispatch on another, and the synchronisation is yours.

`CpuComputeTests` (`Tests/CpuCompute/`) checks the executor's semantics, every
row above and the no-allocation cases; `CpuComputeBench` beside it times the
interpreter against hand-written loops. `GPUCodegenTests`' `…/runs` cases run
the graphs its string tests record, on every lane, and `GPUTests` carries a
CPU half in its compute suites through `Tests/GPU/CpuCrossCheck.h`, which runs
one kernel over plain arrays and, where a device exists, over buffers holding
the same data — the CPU half never self-skips, so the Linux lanes without a
driver check every kernel's numbers. `Apps/GPU/CpuCompute` dispatches one
kernel on both, compares and times them, and runs the CPU path alone where no
device came up.

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

**The chain `mipmapped` builds is built on the CPU, by eacp, for every
backend.** Metal has `generateMipmapsForTexture` and D3D12 has no equivalent at
all — a chain there means a compute shader, a UAV per level and a root signature
to bind them. So for pixels the CPU already holds the choice was a GPU chain on
one backend against a hand-written one on the other, which is two filters
producing two pictures for the same texture, or one filter producing the same
bytes for both. Only the second can be checked by a test, and this library has
been wrong about a cross-backend detail often enough to prefer the version that
can be.

`mipmapped` on a texture created with no pixels — a render target, a kernel
output — builds nothing, since there is nothing to build from. `update()`
rebuilds the CPU chain; `update(region, ...)` and `updateSubresource` do not,
because a partial upload cannot know what the rest of the texture holds. Ask a
texture what it got with `mipLevels()`.

### On the GPU

When level 0 is the GPU's — what a pass drew into a render target, what a
kernel wrote — or has just been replaced, `Frame::generateMipmaps` fills every
level below it from it, in order with the frame's passes:

```cpp
auto descriptor = TextureDescriptor {};
descriptor.width = 512;
descriptor.height = 512;
descriptor.renderTarget = true;
descriptor.mipLevels = mipLevelCount(512, 512);   // levels, no pixels

auto scene = Device::shared().makeTexture(descriptor);

{
    auto into = frame.beginPass(scene);
    into.draw(writer);                            // level 0
}

frame.generateMipmaps(scene);                     // levels 1 to 9 from it

auto pass = frame.beginPass();
pass.draw(reader);                                // samples the chain
```

Every slice gets its own chain — each face of a cube, each layer of an array or
a cube array — and a volume is filtered in z as well. `CommandBuffer` has the
same call off-screen, and either follows an `update()` or `updateSubresource()`
of level 0 to rebuild a stale chain where the CPU's would cost the upload of
every level: a streamed texture, a `.ktx` whose chain was not shipped
(`makeTextureFromSubresources` with level 0 alone and `mipLevels`).

No pass may be open, as for a copy. A texture of one level is nothing to do.
`Device::supportsMipGeneration(format)` says which formats a device filters
into a chain: never a block-compressed or an integer one, nor `RGB9E5Float`
(its chain is `mipmapped`'s, on the CPU), and a format it
refuses is refused with a trace, nothing recorded. Metal records
`generateMipmapsForTexture` on a blit encoder and takes every other format, the
32-bit floats where the device filters them. D3D12 dispatches eacp's own
kernel, a 2×2 (2×2×2 on a volume) box filter level by level through the
texture's per-level UAVs, which any texture of more than one level in a format
with typed UAV loads and stores is created with — it costs the resource
`ALLOW_UNORDERED_ACCESS` and a descriptor per level. Vulkan blits level to
level with `vkCmdBlitImage` and a linear filter, where the format has the blit
and linear-filter features. The weights are each API's own, so the three agree
to within a filter's rounding rather than to the byte; the CPU chain above is
the one that does.

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
chain — as are a count above `mipLevelCount` and null pixels on a texture only
the CPU fills. A render target or a kernel output may ask for levels with no
pixels at all, since the GPU fills them (a pass at `ColorTarget::level`, a
kernel at `setOutputTextureLevel(texture, slot, level)`), and a cube takes a chain
per face, slice-major: face 0's levels, then face 1's.

No new `TextureSampling` configuration is involved: mip filtering on a
single-level texture is what both APIs do anyway, so the four configurations
still cover everything. That is also where a long-standing divergence was found —
D3D12's static samplers had always declared `MIN_MAG_MIP_LINEAR`, while Metal
left `mipFilter` at its default of `NotMipmapped`. Nothing could see it while no
texture had a second level — and neither could `sample(t, uv, level)`, which
names a level instead of letting the hardware pick one from the derivatives, and
read level 0 whatever it asked for because level 0 was all there was.

## Arrays, cube arrays and volumes

```cpp
auto descriptor = TextureDescriptor {};
descriptor.width = 256;
descriptor.height = 256;
descriptor.kind = TextureType::TwoDArray;   // or CubeArray, ThreeD
descriptor.arrayLength = 8;                 // layers; cubes of a CubeArray
descriptor.mipLevels = mipLevelCount(256, 256);

auto layers = Device::shared().makeTextureFromSubresources(descriptor, subresources);
```

`TextureType` is the shape: `TwoD`, `Cube` (the same as `cube = true`),
`TwoDArray`, `CubeArray` and `ThreeD`, with `arrayLength` layers (cubes, for a
cube array) and `volumeDepth` images. The shader declares the matching type —
`texture2d_array`, `texturecube_array`, `texture3d` in MSL, `Texture2DArray`,
`TextureCubeArray`, `Texture3D` in HLSL, `sampler2DArray`, `samplerCubeArray`,
`sampler3D` in GLSL — and binds through the same `setFragmentTexture`,
`setVertexTexture` and `setInputTexture` as a 2D one. The shader graph has no
array or volume type yet; hand-written shaders do.

A **slice** is one 2D image of one level: a layer, a face (`layer * 6 + face`
on a cube array) — the index `TextureRegion`, `TextureCopyRegion`,
`ColorTarget` and `Texture::slices()` speak. A volume has one slice and
addresses its images by z, which is what `ColorTarget::slice` names on one.
A pass renders into any level and slice of a target, or a run of slices at once
(see "A level, a slice, or several layers at once"), and a kernel reads and
writes any level.

Three ways in:

- `makeTexture(descriptor, pixels)` with every slice tightly packed one after
  another, each slice's levels together when `mipLevels` says there are any
  (a volume is level after level instead, each level its depth's worth of
  images). `mipmapped` builds every slice's chain from its level 0.
- `makeTextureFromSubresources(descriptor, subresources)`, a
  `TextureSubresource {level, slice, pixels, bytesPerRow, bytesPerImage}` per
  level of each slice — what a `.dds` or `.ktx` container hands over. With
  `mipmapped` and level 0 alone, eacp builds the rest. Subresources not named
  are left unwritten, so `mipLevels` with an empty list is allowed here.
- `updateSubresource(region, pixels)` afterwards, a `TextureRegion {level,
  slice, origin, size, bytesPerRow, bytesPerImage}` naming any box of any level
  of any slice (z and depth on a volume); `readSubresource(region, dst)` is its
  mirror. A compressed region is block-aligned or runs to the level's edge.
  Neither rebuilds a chain eacp built; `Frame::generateMipmaps` after an update
  of level 0 does, on the GPU (see "Mipmaps").

Refused at creation: `arrayLength` on a kind without layers, `volumeDepth` on
anything but a volume, `cube` beside a kind other than `TwoD` or `Cube`, a
multisampled kind other than `TwoD`, a volume that is compressed or a render
target with depth, stencil or `sampleableDepth`, and a cube array where
`Device::supportsCubeArrays()` says no. A kernel writes a cube or a cube array
as the 2D array of its slices, through `ComputePass::setOutputTextureAsArray`.
`Device::maxTextureLayers()` and `maxTextureSize3D()` are the limits (2048 on
Metal and D3D12, the device's own on Vulkan).

On Metal a render target's pixels are in private storage, so an upload to one
goes through a staging buffer and a blit on the queue rather than
`replaceRegion`. D3D12 creates the resource with the kind's array size (a
`TEXTURE3D` for a volume) and the matching SRV and UAV dimension; Vulkan an
image of `slices` layers (`CUBE_COMPATIBLE` for the cube kinds, `imageCubeArray`
enabled where offered) with a view of the kind's type.

## Texture formats

| Format | Notes |
| --- | --- |
| `RGBA8Unorm`, `BGRA8Unorm` | The ordinary ones |
| `R8Unorm` | One byte per pixel, sampled as `(r, 0, 0, 1)` — masks, palette indices |
| `RG8Unorm` | Two, sampled as `(r, g, 0, 1)` — an NV12 frame's chroma plane |
| `RGBA16Float` | The float format to reach for |
| `RGBA32Float` | When the mantissa really is the point |
| `R32Float` | One full-precision channel — a depth copy, a distance field |
| `R16Float`, `RG16Float` | One or two half channels — an occlusion term, a motion vector |
| `RG32Float` | Two full floats |
| `R16Unorm` | One 16-bit normalised channel — a height map, a 16-bit depth copy |
| `R32Uint` | One 32-bit unsigned integer — an id, a count; read, never filtered |
| `RGBA8UnormSrgb`, `BGRA8UnormSrgb` | The ordinary ones holding colour: decoded to light when sampled, encoded when rendered into |
| `RG16Unorm`, `RGBA16Unorm` | Sixteen normalised bits a channel — a normal map, a 16-bit PNG |
| `RGB10A2Unorm` | 10:10:10:2 in one word, red lowest |
| `RG11B10Float` | Three unsigned small floats in one word — an HDR colour at half `RGBA16Float`'s cost |
| `RGB9E5Float` | Three nine-bit mantissas sharing a five-bit exponent — HDR colour, sampled only |
| `R8Uint` … `RGBA32Uint`, `R8Sint` … `RGBA32Sint` | Every 8-, 16- and 32-bit integer shape in one, two and four channels; read, never filtered |
| `BC1RGBA`, `BC2RGBA`, `BC3RGBA`, `BC7RGBA` | Block-compressed — DXT1/3/5 and BC7 |
| `BC1RGBASrgb` … `BC7RGBASrgb` | The same four, decoded from sRGB |
| `BC4RUnorm`, `BC4RSnorm`, `BC5RGUnorm`, `BC5RGSnorm` | One and two block-compressed channels — a mask, a normal map |
| `BC6HRGBUfloat` | Block-compressed unsigned half-float RGB — an environment map |

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

Every uncompressed format but `RGB9E5Float` is a render target, its
`PixelFormat` the one `pixelFormatFor` hands back (and `textureFormatFor` the
way back); `isRenderableFormat` answers which. The `Uint`
and `Sint` formats are integer attachments, written as a `uint` or an `int` by
the fragment function and never blended — a blend preset left on a pipeline
writing one is dropped rather than failing it — and read by a shader that
declares `texture2d<uint>` or `<int>`, `Texture2D<uint4>` or `<int4>`,
`usampler2D` or `isampler2D`, through `read`, `Load` or `texelFetch`. None of
them gets a mip chain from `mipmapped`, there being nothing to average in an id.
Which formats a kernel can write is the device's answer (see "Textures a kernel
writes"); no sRGB format is one of them anywhere. `textureFormatTraits` is the
whole of what eacp knows about a format in one table — bytes per texel or per
block, channels, `TextureFormatKind` (`Unorm`, `Snorm`, `Float`, `Uint`,
`Sint`) and whether it is sRGB — and `isIntegerFormat`, `isSrgbFormat`,
`formatChannels` and the rest answer from it.

**sRGB is how a format says its bytes are colour rather than numbers.** A shader
sampling `RGBA8UnormSrgb` gets linear light, decoded per texel *before* the
filter, so the texel between a black one and a white one is half the light (0.5)
rather than the decode of half the code value (0.21). A pass rendering into one
writes linear light and the hardware encodes it, blending in light first: two
additive draws of 0.25 store 188, the encoding of 0.5, not 137 + 137. That is
the gamma a lit scene needs, and the only way an 8-bit target holds one without
banding the darks. `read()` and `update()` move the encoded bytes untouched;
`srgbFormatFor` and `linearFormatFor` swap a format for its twin, the same
bytes decoded the other way. A chain eacp builds for one on the CPU averages in
light — decoded, averaged, encoded — as `generateMipmaps` does on Metal and
Vulkan; D3D12's chain kernel needs a typed UAV no sRGB format has, so there
`supportsMipGeneration` answers no and `mipmapped` is the way.

**`RGB9E5Float` is sampled, never rendered into.** Three nine-bit mantissas,
red lowest, scaled by one five-bit exponent at the top of the word
(`mantissa × 2^(exponent − 24)`, no implied leading one): HDR colour in four
bytes with more precision than `RG11B10Float`, for a light probe or an
irradiance map baked offline. All three APIs sample and filter it; none
promises to render into it (Metal does on Apple GPUs only) or to store to it
from a kernel, so a `renderTarget` or `computeWrite` texture in it is invalid,
like a block format's, and it has no `PixelFormat`. It uploads, updates, reads
back and copies as any 32-bit format does, and `mipmapped` builds its chain on
the CPU, decoding to float, averaging and encoding again.

A compressed texture cannot be a render target or a kernel output, `read()` and
`update(region, ...)` are no-ops on one, `update(pixels)` takes the same packed
block the constructor took with `bytesPerRow` at 0, and `mipmapped` gives it
exactly one level — the CPU filter averages texels and a block is not four
numbers to average. `mipLevels` is how it gets a chain. BC4 is 8 bytes a
block and one channel, BC5 16 bytes and two (each unsigned or signed), BC6H 16
bytes of unsigned half-float RGB; the BC sRGB twins decode like
`RGBA8UnormSrgb`.

Depth as a texture format the caller chooses — `D16`, `D24S8`, `D32FS8` beside
the `D32F` eacp always makes — is not here yet. Depth is a plane of a render
target (`TextureDescriptor::depth`, `stencil`), its format fixed at `D32F` or
`D32F` with stencil, and the pipeline's attachment format is derived from the
same two flags, so a chosen format is a field on the descriptor, on the
pipeline and on `GPUView` together, and a D24S8 refusal on every Apple-silicon
Mac.

## Sampling

How a texture is sampled belongs to the *shader*, not to the `Texture`, which is
a deliberate break from the obvious design and has a Windows driver bug behind
it. See [`SAMPLERS.md`](SAMPLERS.md).

A shader written by hand, or imported from another engine, can instead take its
sampler per draw. `Device::makeSampler(SamplerDescriptor)` describes one — wrap
per axis (clamp, repeat, mirror, border), min/mag/mip filter, anisotropy, a
comparison function for shadow lookups, border colour, LOD clamp — and returns a
`Sampler` the device keeps: an equal descriptor gives back the same native
object, so asking per draw costs a hash lookup. The overloads taking one bind it
beside the texture:

```cpp
auto tiled = device.makeSampler({.addressU = SamplerAddressMode::Repeat,
                                 .addressV = SamplerAddressMode::Repeat,
                                 .minFilter = SamplerFilter::Linear,
                                 .magFilter = SamplerFilter::Linear,
                                 .mipFilter = SamplerMipFilter::Linear});

pass.setFragmentTexture(floor, 0, tiled);
pass.setFragmentDepthTexture(shadowMap, 1, shadowCompare);
```

The shader declares the sampler where the slot puts it: an MSL
`sampler s [[sampler(slot)]]` argument, an HLSL
`SamplerState s : register(s<8 + slot>)` (`RenderPass::samplerRegisterBase`; a
`SamplerComparisonState` for a comparison sampler), or on Vulkan the combined
`sampler2D` / `sampler2DShadow` at the texture's own binding. Shaders built with
the codegen layer keep sampling the way they declared, and the `TextureSampling`
overloads are the same cache under four fixed descriptors
(`SamplerDescriptor::from`). `Device::supportsPerBindSamplers()` is false on the
one D3D12 driver whose sampler tables cannot be trusted, where the per-bind
registers hold a fixed linear/repeat sampler instead; `SAMPLERS.md` says which
and how to override it.

### Textures in the vertex stage, and how many slots

A vertex shader samples too - a height map raising a terrain grid, a shadow map
read per vertex. `setVertexTexture` and `setVertexDepthTexture` are the
fragment binds' siblings, with the same `TextureSampling` and `Sampler`
overloads:

```cpp
pass.setVertexTexture(heightMap, 0, nearest);
pass.setVertexDepthTexture(shadowMap, 1, shadowCompare);
```

The vertex function samples with an explicit level - `sample(s, uv, level(0))`
in MSL, `SampleLevel` or `SampleCmpLevelZero` in HLSL, `textureLod` in GLSL. The
codegen layer still binds textures in the fragment stage only.

| | texture slot `n` | its per-bind sampler | slots per stage | stages share a slot |
| --- | --- | --- | --- | --- |
| Metal | `texture(n)` in each stage | `sampler(n)` in each stage | 16 (16 sampler indices; 31 texture ones) | no |
| D3D12 | `t<n>` | `s<8 + n>` | 8 (buffers start at `t8`) | yes |
| Vulkan | binding `8 + n`, a combined image sampler | the same binding | 16 (bindings 8 to 23) | yes |

`Device::maxTextureSlots()` is the device's own count and a bind past it is
dropped; `RenderPass::maxStageTextureSlots` (16) is the most any backend has, and
the codegen layer and compute keep to eight everywhere. On D3D12 and Vulkan a
slot is one binding both stages see: the D3D12 texture and sampler tables are
visible to every stage, which costs no root-signature space where a table per
stage would need sixteen DWORDs it does not have, and Vulkan declares each
combined image sampler for both stages. A vertex bind and a fragment bind at
one slot are then the same bind, the last winning, and
`Device::stagesShareTextureSlots()` says so; Metal keeps an index space per
stage. Portable code binds one texture per slot across the two stages, which is
bgfx's model too.

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

### The drawable itself

A window's screenshot is the third case, and neither of the two above reaches
it: `renderToImage` renders the frame again into a target of its own, and the
drawable is not a `Texture`. `Frame::copyDrawable` copies what the frame's
passes have drawn into its own target so far — the drawable on screen, the
snapshot's texture off it — into a texture of `Frame::drawableFormat()`
(`BGRA8Unorm` on every view eacp makes) and `pixelSize()`, in order with the
passes, and that texture is then read like any other once the frame has gone:

```cpp
view.setReadableDrawable(true);   // once, before the frames that copy

void render(Frame& frame) override
{
    { auto pass = frame.beginPass(); pass.draw(scene); }

    const auto size = frame.pixelSize();

    if (!screenshot.isValid())
        screenshot = Device::shared().makeTexture({.width = (int) size.x,
                                                   .height = (int) size.y,
                                                   .format = frame.drawableFormat()});

    frame.copyDrawable(screenshot);   // after the last pass, none open
}
// ... after the frame: screenshot.read(pixels.data());
```

A multisampled view copies the resolved picture. A texture of another format,
size, kind or sample count is refused with a trace, as is a drawable the view
was not made readable for: on Metal `setReadableDrawable` is the layer's
`framebufferOnly` turned off, and on Vulkan the swapchain rebuilt with
`TRANSFER_SRC` where the surface offers it (where it does not, the copy is
refused); a D3D12 back buffer copies either way. An off-screen frame's target
always copies. Metal records a blit on the frame's command buffer; D3D12 a
`CopyTextureRegion` out of the back buffer, or a `ResolveSubresource` from the
multisample target, whose picture only reaches the back buffer as the frame
ends; Vulkan a `vkCmdCopyImage` out of the swapchain image, which a multisampled
pass has already resolved into.

## Several views in one render

One renderer drawing into several windows in one go — bgfx's frame into each of
its swapchains, an editor's viewports — calls `GPUView::renderFrame` on each
other view from inside the render that is running. It takes that view's next
drawable, opens a `Frame` on it, calls the function in place of `render()`, and
presents when the function returns:

```cpp
void render(Frame& frame) override        // the main window's view
{
    { auto pass = frame.beginPass(target); pass.draw(shadows); }

    for (auto* other: otherWindows)
        other->renderFrame([&](Frame& theirs)
        {
            auto pass = theirs.beginPass();
            pass.draw(sceneFrom(other->camera));   // may sample `target`
        });

    auto pass = frame.beginPass();
    pass.draw(sceneFrom(camera));
}
```

**The frames on a device are a stack.** A frame opened while another is open is
nested in it: the open one is flushed first (`Frame::flush`, so **no pass may be
open** on it), the nested frame records, submits and presents, and the outer one
carries on recording behind it. So the order of the work is the order of the
code — the nested frame sees everything the outer one drew before the call, and
the outer one's later passes see everything the nested one drew — and each
window is presented by its own frame, the nested ones first. A nested frame is
not timed (`Device::lastFrameTimings` is the outer frame's) and does not advance
`Device::frameIndex`, so what it streams goes into the outer frame's pools,
which are not recycled until that frame is off the GPU. `Device::isFrameOpen()`
says whether a frame is open.

`renderFrame` returns whether there was a drawable: false for a view with no
size yet, a view with no swapchain — on Linux and Windows a view in no window
has none — and a view whose own frame is already open, which cannot nest in
itself. On Metal a `CAMetalLayer` hands out drawables whether or not it is on
screen, which is what the `MultiView/` tests ride. A view presenting this way is
driven by its caller: leave it out of continuous mode, or its display link
renders it a second time. Each present waits on its own swapchain, so on D3D12,
where a present at sync interval 1 can wait for a vertical blank, `n` windows
can cost up to `n` refreshes a frame.

## Copies

`Frame::copy` records a copy onto the frame's command buffer, in order with its
passes, and `CommandBuffer::copy` does the same off-screen. What an earlier pass
wrote is what gets copied, and a later pass sees the copy:

```cpp
frame.beginPass(scene, {{1.f, 0.f, 0.f, 1.f}});
frame.copy(scene, history);                                // whole texture
frame.copy(mips, other, TextureCopyRegion::wholeLevel(1)); // one level

auto region = TextureCopyRegion {};
region.sourceSlice = 3;                                    // a cube face
region.sourceOrigin = {8, 8};
region.size = {16, 16};                                    // 0 runs to the edge
frame.copy(cube, faces, region);

auto box = TextureCopyRegion {};
box.sourceOrigin = {0, 0, 4};                              // z on a volume
box.size = {0, 0, 8};                                      // eight images
frame.copy(volume, otherVolume, box);

auto layers = TextureCopyRegion::wholeLevel(0, 2);
layers.sliceCount = 3;                                     // slices 2, 3, 4
frame.copy(array, otherArray, layers);

frame.copy(vertices, scratch, 0, 256, 1024);               // buffer to buffer
frame.copy(staging, texture, {.bytesPerRow = 64});         // buffer to texture
frame.copy(texture, readback, {.origin = {2, 2}, .size = {4, 4}});
```

The rules, each refused with a trace and nothing recorded when broken: no pass
may be open (a command buffer has one encoder at a time, as `flush()` and
`beginCompute` already say); texture to texture is between two textures of one
format, two resources rather than one, with a level and a slice on each side and
the region inside both levels; a compressed texture copies whole levels only; a
multisampled target copies its resolved picture and the depth plane is never
copied; buffer offsets and lengths are multiples of four; a buffer layout's
offset is on the texel grid and its rows at least a row of texels long, and the
rows it describes fit the buffer. Between two volumes the origins' `z` and the
size's `depth` pick a box of images (`wholeLevel` and the two-argument copy take
every image); a volume copies only to and from another volume; `sliceCount`
copies that many consecutive slices from each side's slice, the same box of
each. A buffer-texture copy is still one image: `z` and `depth` are refused
there.

Metal records a blit encoder per copy, one blit per slice of a run. D3D12
records `CopyTextureRegion` (one per slice, the box's `front` and `back` a
volume's images) and `CopyBufferRegion` through the resource state tracking, handing a plain texture
back to `PIXEL_SHADER_RESOURCE` after; a buffer layout off D3D12's 256-byte row
and 512-byte offset grid goes through a scratch buffer row by row, and a driver
with `noBoxedTextureCopy` takes whole subresources only. Vulkan records
`vkCmdCopyImage` (a run as one region's `layerCount`, a box's depth as its
extent), `vkCmdCopyBuffer` and the buffer-image pair through the layout
tracker, each image returned to its resting layout after.

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
`Graphics::Window` presents through a `VK_KHR_swapchain` over whichever window
system that window came up on, Wayland or X11 — as does one in an
`EmbeddedView`, whose surface is an X11 child of a window a host owns, and which
is the same code path from `createSurface()` down. The backend is built on every
Linux build, exactly as the Metal and D3D12 backends are on theirs;
`-DEACP_BUILD_GRAPHICS=OFF` is the only thing that leaves it out.

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
- **A 1.1 or 1.2 device reaches the same floor through extensions.** Phone
  drivers lag the hardware: a Galaxy S22's Adreno 730 reports 1.1 under a 1.4
  loader. Such a device is taken when it offers `VK_KHR_synchronization2`,
  `VK_KHR_timeline_semaphore` and `VK_EXT_descriptor_indexing` with the same
  features, plus `VK_KHR_create_renderpass2` and `VK_KHR_depth_stencil_resolve`
  in place of dynamic rendering. `createDevice` enables them, chains their
  feature structs in place of `VkPhysicalDeviceVulkan12/13Features`, and points
  volk's core entry points (`vkCmdPipelineBarrier2`, `vkQueueSubmit2`,
  `vkCmdWriteTimestamp2`, `vkWaitSemaphores`, `vkGetSemaphoreCounterValue`,
  `vkCreateRenderPass2`) at the `KHR` ones, so no call site branches: the
  structures and `_2_` flags are the same values. The instance asks for the
  loader's version capped at 1.3, VMA for 1.1, and glslang writes SPIR-V 1.3 for
  Vulkan 1.1 — 1.4 where `VK_KHR_spirv_1_4` is offered and enabled.
  The log line at device creation says which path was taken.
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
- **A pass is one `vkCmdBeginRendering` on a 1.3 device.** Below 1.3 (or with
  `EACP_VK_RENDER_PASSES=1`, which is how a 1.3 device tests it) the same
  `VkRenderingInfo` is turned into a `VkRenderPass` of one subpass with the same
  attachments, ops and resolves — depth through
  `VkSubpassDescriptionDepthStencilResolve` — and a framebuffer, both cached in
  `VulkanRenderPassCache` (`Vulkan/VulkanRenderPass-Linux.cpp`): render passes by
  formats, samples, ops and resolves, framebuffers by render pass, views and
  extent, dropped when a view they name is destroyed. Every attachment's initial,
  subpass and final layout is the one the barriers around the pass already put
  it in, so the render pass moves nothing and the barriers stay the only
  transitions on both paths. A pipeline names a render pass with its formats and
  sample count, load/store ops and resolves not deciding compatibility for one
  subpass.
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
  `CoordinateSpaceTests` pass unchanged. The two other fixes are wrong:
  negating `gl_Position.y` reverses the winding, so `CullModeTests` fail
  unless the front face is inverted with it, and negating y in the
  projection breaks the CPU/shader equivalence of `Mat4`, which `CubeMap`
  and `StencilShadows` build on the CPU while `Teapot` and `Maze` build
  in-shader.

### The swapchain

`GPUView` asks `Graphics::requestViewSurface` for a `ViewSurface`
(`Graphics/View/View-Linux.h`) and creates a `VkSurfaceKHR` over the
`NativeSurfaceHandle` the window backend reports on it — a kind tag, a
connection, a `wl_surface*` or an X11 window id. That record — the handle, a
pixel size, a scale and five hooks — is the whole of what `eacp-gpu` knows
about the window system: `createSurface()` switches on the kind between
`vkCreateWaylandSurfaceKHR` and `vkCreateXcbSurfaceKHR`, and everything below
it is shared. It neither links nor includes libwayland or xcb beyond what
`vulkan_wayland.h` and `vulkan_xcb.h` pull in, and `VK_USE_PLATFORM_WAYLAND_KHR`
with `VK_USE_PLATFORM_XCB_KHR` (`CMake/FindVulkanBackend.cmake`, `PUBLIC` so
`volk.c` sees them too) is what makes the two creators reachable.
`VK_KHR_surface` with `VK_KHR_wayland_surface` and `VK_KHR_xcb_surface` on the
instance and `VK_KHR_swapchain` on the device are enabled only where they are
offered — the two window systems independently, since a driver may carry either,
which is why each creator is null-checked before it is called: volk leaves the
pointer null for an extension that was not enabled.
`VulkanShared::supportsPresentation()` says whether any of it was — a headless
ICD, or a loader with no WSI, leaves a `GPUView` rendering off-screen exactly as
it did before there was a swapchain. lavapipe presents to an Xvfb over
`VK_KHR_xcb_surface` with no help, which is how the `Present` cases run on the
CI lane's second test step.

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
- **Frames are paced by the window system, not by a clock.** There is no
  `DisplayLink` here. Continuous mode renders, asks for a frame callback
  (before the present, which is the commit that carries the request on
  Wayland), and renders again when `ViewSurface::onFrameDone` says the last
  frame was taken — so a hidden or occluded window, which gets no callbacks,
  renders nothing, and the main thread never blocks inside
  `vkAcquireNextImageKHR`. On Wayland that callback is `wl_surface.frame`; X11
  has no equivalent, so the backend answers from a timer at the RandR mode's
  rate — rebuilt at the new rate when a RandR change moves it — and this loop
  does not know the difference. The
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
  `maxImageCount`; `preTransform` identity wherever the surface supports it,
  else the surface's own. Wayland reports
  `currentExtent` as `0xFFFFFFFF` — there is no server-side surface size — so the
  extent comes from the record's `pixelWidth`/`pixelHeight`; X11 reports the
  child window's real size and that is taken as it stands.
- **Rotation on Android** is the compositor's. The swapchain is built
  identity, the window's own size, and the frame is drawn upright in the
  window's coordinates; the compositor turns it. Pre-rotating instead would
  save that pass but would mean rotating every projection, clip space
  position, viewport and scissor an app or the UI tier writes, and there is no
  one place eacp applies them. The extent of a surface that offers quarter
  turns in `supportedTransforms` — Android's — is the record's
  `pixelWidth`/`pixelHeight` (the `ANativeWindow`'s size), sides swapped only
  in the fallback where identity is unsupported and the surface's quarter-turn
  `currentTransform` is taken, then clamped to `min`/`maxImageExtent`:
  `currentExtent` is not used there because it describes the orientation the
  surface last presented in, and at the resize that announces a turn it still
  holds the old one (measured on the emulator: a 2400x1080 window reporting
  1080x2400 and the old transform). Wayland and X11 offer identity alone and
  are unchanged. On a rotated display Android reports every acquire and present
  `SUBOPTIMAL` because an identity transform is not the display's, and that is
  all it means there — a size change is `OUT_OF_DATE` or a resize — so a
  swapchain built identity on such a surface ignores `SUBOPTIMAL` outright:
  no rebuild and no surface query in a steady landscape frame. Turning between
  portrait and landscape resizes the `ANativeWindow`, which the window reports
  as a resize, so the content is laid out again at the new size in points and
  the swapchain rebuilt once; a half turn (90 to 270) keeps the size and
  rebuilds nothing.
- **Rebuilds** are marked and done at the next frame, so a live resize that
  reports twenty sizes builds one swapchain: `onResized`, `OUT_OF_DATE` from
  either the acquire or the present, and `SUBOPTIMAL` from either except where
  the rotation rule above expects it. A `SUBOPTIMAL` acquire is
  drawn and presented first — it handed over an image and signalled the
  semaphore, and dropping it would leave that semaphore signalled. `onLost`
  destroys the swapchain, the semaphores, the companions and the `VkSurfaceKHR`
  synchronously, before the `wl_surface` or the child window goes: a swapchain
  outliving its surface is a use-after-free inside the driver, not an error
  code.
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
| `EACP_REQUIRE_DISPLAY=1` | The swapchain's sibling of `EACP_REQUIRE_GPU`. `Tests/GPU/PresentTests-Linux.cpp` needs a display server — a compositor, or an X server where `EACP_WINDOW_SYSTEM=x11` — and every case in it self-skips without one, which ctest scores as a pass. This makes those cases fail instead, so a lane whose Weston or Xvfb session did not come up says so. Set it wherever the suite is run under a display server; leave it unset everywhere else. |
| `EACP_HEADLESS=1` | Not Vulkan's, but it belongs here: it is what tells the window backend to build no surface, and therefore what the present tests read to decide there is nothing to present to. |

CI runs the suite on lavapipe with `EACP_VK_SOFTWARE`, `EACP_REQUIRE_GPU` and
`EACP_VK_VALIDATION` set and no display server. The lavapipe ICD manifest is
named per architecture (`lvp_icd.x86_64.json` on an x86-64 runner,
`lvp_icd.json` in an arm64 container), so nothing sets `VK_DRIVER_FILES` — the
loader finds it from the ICD directory.

The present tests need a display server, and they run against both: Weston's
headless backend for Wayland and an Xvfb for X11 — `with-weston <command>` and
`with-xvfb <command>` in the `Dockerfile` run a command inside one each, the
second exporting `EACP_WINDOW_SYSTEM=x11` so the same cases come up on the
other backend:

```bash
docker run --rm -e EACP_VK_SOFTWARE=1 -e EACP_REQUIRE_GPU=1 \
    -e EACP_REQUIRE_DISPLAY=1 -v "$PWD":/workspace eacp-ci-linux \
    with-weston ctest --test-dir build-ci-linux --output-on-failure \
    -E '^(X11|EmbeddedView)/'

docker run --rm -e EACP_VK_SOFTWARE=1 -e EACP_REQUIRE_GPU=1 \
    -e EACP_REQUIRE_DISPLAY=1 -v "$PWD":/workspace eacp-ci-linux \
    with-xvfb ctest --test-dir build-ci-linux --output-on-failure \
    -R '^(X11|EmbeddedView|Present)/'
```
