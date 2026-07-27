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
- `UInt` for the compute thread id
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

Render targets are single-sampled and have no depth attachment. What this is for
is a full-screen pass over a whole texture, and neither has a meaning there.

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

A command buffer has one open encoder at a time, so let a pass end before
beginning the one that reads what it wrote. `Apps/GPU/ComputeParticles` is the
worked example, and `Apps/GPU/AsyncCompute` times the two commits against each
other.

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
