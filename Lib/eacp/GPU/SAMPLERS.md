# Samplers, and why the shader declares them

`TextureSampling` (filter + address mode) belongs to the **shader**, declared on
its texture members, and not to the `Texture` it is bound to. That is the
opposite of the obvious design and of what this backend did until recently, so
this file records why, and what is left to finish.

## The driver bug that forced it

On Windows-on-Arm (Qualcomm/Adreno, Windows 11 26200), **a D3D12 sampler
descriptor table always resolves to descriptor 0 of the bound sampler heap. The
offset in the table is ignored.**

Every texture in the process therefore sampled through whichever sampler
happened to be first in the heap — which was `D3D12Context`'s null seed, point
filtered and clamped — and no per-texture sampler state had any effect at all.

It is reproducible in a standalone program with no eacp involved: create a
sampler heap with `CLAMP` at descriptor 0 and `WRAP` at descriptor 1, bind
descriptor 1 with `SetGraphicsRootDescriptorTable`, and sample a texture at
`u = 1.25`. The GPU uses `CLAMP`. Swap which sampler sits in which descriptor and
the result swaps with it — it tracks descriptor 0, not the one bound.

Ruled out, each with its own run:

| Hypothesis | Result |
|---|---|
| The handle arithmetic is wrong | No — swapping the two descriptors swaps the result |
| Rebinding a root parameter is dropped | No — fails with a single bind and no prior seeding |
| Non-canonical state-setting order | No — PSO/root-signature/heaps in canonical order is identical |
| The D3D12 debug layer | No — fails identically with the layer off |
| SRV tables are affected too | No — SRV offsets work fine; this is sampler-specific |
| **Static samplers** | **Work correctly** |

The debug layer is worth a note of its own, because it sends a red herring: with
it enabled this driver reports `GetDescriptorHandleIncrementSize` as **152 for
every heap type** (including RTV and DSV, which cannot all be equal) and returns
GPU handles as `cpu + 1`. That is just its descriptor virtualisation and is
harmless. Without the layer the same device reports a sane 16/16/32/16 and
`gpu == cpu`. Do not chase those numbers; they are not the bug.

## What eacp does instead

`TextureSampling` lives in `Texture.h` next to the filter and address-mode enums
it is built from. A shader declares it on each texture member; `Texture` carries
no sampler state at all, and `TextureDescriptor` has no `filter` or
`addressMode` field to disagree with the declaration.

On **D3D12**, samplers never go near a descriptor heap:

- The render root signature declares a **static sampler for every (texture slot,
  sampling configuration) pair** — `maxTextureSlots * samplingConfigurations` of
  them, currently 16. `D3D12Context::createRootSignatures` builds them.
- `ShaderEmitter` emits each texture's `SamplerState` at register
  `s(slot * samplingConfigurations + samplingIndex(sampling))`. Choosing the
  register chooses the sampler.
- `RenderPass::setFragmentTexture` binds only the SRV table and ignores the
  sampling it is passed — the shader already committed to it at compile time.
- `Frame::beginPass` seeds only the SRV tables. There are no sampler tables left
  to seed.
- `Texture::Native::createDescriptors` allocates **no** sampler descriptor.

On **Metal**, where nothing is wrong with binding samplers, the declaration is
still what picks one, so the two backends cannot drift apart:

- `Device` builds one `MTLSamplerState` per configuration at construction —
  there are only four — and `Device::nativeSampler(sampling)` returns it.
- `RenderPass::setFragmentTexture` binds the state for the sampling it is
  passed, not one the `Texture` carries.
- `ShaderProgram::bindTextures` is what passes it, from each member's
  declaration. A hand-rolled bind supplies it at the call.

The cost is that the sampling is fixed when the shader is compiled rather than
when a texture is bound. A shader sets it before calling `compile()`:

```cpp
WorldShader()
{
    texture.sampling = {TextureFilter::Nearest, TextureAddressMode::Repeat};
    compile();
}
```

Adding a configuration means widening `samplingConfigurations` and
`samplingIndex`; the root signature, the emitter and the Metal sampler cache all
derive from those, so they stay in step on their own.

## One shader samples one slot one way

The real cost of a compile-time decision is not the enumeration, it is that a
program cannot change its mind per draw. A renderer that must sample some
textures smoothly and others crisply needs **one compiled program per
configuration**, not one program it re-points.

`Sprites::SpriteRenderer` is the case in the tree: it draws camera frames
(scaled to fit, so `Linear`) and pixel art (so `Nearest`) through the same
`drawTexture`. It keeps a program per sampling configuration, built on first
use, behind a Pimpl in `SpriteRenderer.cpp` (the shader classes are in
`SpriteShaders.h`), and `drawTexture` takes a
`TextureSampling` defaulting to `Nearest`.

Sampling is therefore part of what a batch is: the renderer queues quads and
draws a run of them at once, and a change of sampling closes the open run for
the same reason a change of texture does. Issuing draws grouped by sampling is
worth it where it is easy, since alternating between two of them turns a single
instanced draw into one draw per quad.

## Samplers chosen per bind

Everything above is about shaders built with the codegen layer, and it still
holds for them. A shader that comes from elsewhere — hand-written, or emitted
by another engine's compiler, bgfx's first among them — expects to be handed
its sampler with each draw, and eacp now does that too:

- `SamplerDescriptor` (`Texture/Sampler.h`) is the whole sampler as a value:
  wrap per axis (`ClampToEdge`, `Repeat`, `MirrorRepeat`, `ClampToBorder`),
  min/mag filter, mip filter (`None` reads level 0), anisotropy, an optional
  `CompareFunction`, one of three border colours, and a LOD clamp. It compares
  and hashes by value.
- `Device::makeSampler(descriptor)` returns a `Sampler`, a handle the device
  owns. The device keeps a map from descriptor to sampler behind a mutex
  (`Device.cpp`) and only asks the backend (`createNativeSampler`) the first
  time it sees a descriptor, so a renderer can ask per draw per texture.
- The four `TextureSampling` configurations are four of those descriptors
  (`SamplerDescriptor::from`), and `makeSampler` hands back the very objects
  `Device::nativeSampler(sampling)` already held on Metal and Vulkan. The
  `TextureSampling` overloads are therefore sugar over the same cache, and
  behave exactly as before on every backend.
- `RenderPass::setFragmentTexture(texture, slot, sampler)`,
  `setFragmentDepthTexture(target, slot, sampler)` and
  `ComputePass::setInputTexture(texture, slot, sampler)` bind it, and
  `setVertexTexture`/`setVertexDepthTexture` do the same for a vertex shader.

Where the shader finds it, per backend:

| | Shader declares | Bind |
|---|---|---|
| Metal | `sampler s [[sampler(slot)]]` — what the MSL emitter already writes | `setFragmentSamplerState:atIndex:slot`, or `setVertexSamplerState:` for the vertex stage |
| D3D12 | `SamplerState s : register(s<8 + slot>)` (`RenderPass::samplerRegisterBase`) | a one-descriptor sampler table per slot, pointed at the `Sampler`'s descriptor in the device's sampler heap |
| Vulkan | the combined `sampler2D` / `sampler2DShadow` at the texture's binding (`8 + slot`) | the `VkSampler` goes in the combined image sampler's descriptor write |

On D3D12 the per-bind registers start at `s8`, above the four static samplers
at `s0..s3`, so codegen shaders and per-bind shaders share one root signature
without either moving. It is the same shape the storage buffers already have
(`t<8 + slot>`, `RenderPass::bufferRegisterBase`), and it keeps shader model 5.0:
register spaces would have been the tidier separation, but they need 5.1 and
bgfx's DXBC is 5.0. A codegen shader never reads `s8..`, so on D3D12 it samples
the way it declared whatever `Sampler` is bound; on Metal and Vulkan it shares
the slot with the per-bind sampler, so a `Sampler` bound after
`ShaderProgram::bindTextures` would override the declaration there. Don't mix
the two on one slot.

On Vulkan the render layout keeps its combined image samplers: the sampler
already travelled in the descriptor write, so per-bind costs nothing there. A
module declaring a *separate* `sampler` and `texture2D` — bgfx's SPIR-V does,
with the sampler on a binding of its own — needs layout bindings the
fixed render layout does not have; that belongs with the SPIR-V intake (G1),
whose reflection (`spirvTextureBindings`) can build per-pipeline layouts the way
compute already does.

### Vertex stages and the slot ceiling

Metal has sixteen sampler indices per stage against thirty-one texture ones,
so sixteen is its texture slot count too (`Device::maxTextureSlots()`), with
the vertex and fragment stages each holding their own sixteen. Vulkan matches
it: the combined image samplers run from binding 8 to 23, under the storage
buffers at 24, and are declared for both stages. D3D12 stays at eight: the
texture registers stop at `t8`, where the storage buffers start, and the render
root signature spends 56 of its 64 DWORDs - four root CBVs, sixteen root SRVs,
eight SRV tables and eight sampler tables. A vertex table and a vertex sampler
table per slot would be sixteen more, so instead the existing tables, the four
static samplers and the fallback statics are all visible to every stage: a
vertex shader reads `t<n>` and `s<8 + n>` exactly as a pixel shader does, the
two stages share each slot, and `Device::stagesShareTextureSlots()` is true
there as it is on Vulkan. The fallback route's per-bind statics serve the
vertex stage the same fixed linear/repeat sampler.

Anisotropy is clamped to 16 on Metal and D3D12 and to the device limit on
Vulkan, where `samplerAnisotropy` is now enabled whenever the device offers it;
without the feature the sampler is built without anisotropy.

### The D3D12 fallback, and who decides it

The bug above has not gone anywhere, and a per-bind sampler is a descriptor
table by definition, so the D3D12 backend decides at device creation which of
two root signatures to build:

- **Sampler tables** (the default): eight sampler tables at `s8..s15`, seeded
  with the null sampler (Tier 1 requires every table set) and pointed at a
  `Sampler`'s descriptor on bind. `Device::supportsPerBindSamplers()` is true.
- **Static fallback**: no tables. `s8..s15` are static samplers, all linear with
  repeat — bgfx's default sampler state, so the commonest request draws right —
  and a per-bind bind is dropped, logged once. `supportsPerBindSamplers()` is
  false, and the per-bind tests in `SamplerTests.cpp` skip.

`DriverQuirks::samplerTablesIgnoreOffset` picks between them, and it is set by
**adapter**, unlike the other two quirks, which are found by trial. Those two
are refused at `Close()`, so recording them on a throwaway device answers the
question for free; this one records and runs without complaint and simply
samples through the wrong descriptor, so a trial would mean drawing and reading
back on a throwaway device at every start. The guess is every Qualcomm adapter
(vendor `0x5143` or `0x4D4F4351`), which is where the bug was seen.

Two switches overrule it:

- `EACP_D3D12_SAMPLER_TABLES=1` forces the tables on — the way to find out
  whether a driver has been fixed: run `ctest -R '^Sampler/'` on it and
  watch whether `repeatTilesTheTexture` and
  `oneProgramSamplesTwoWaysInOnePass` stay green.
  If they do, the Qualcomm guess can be narrowed or dropped.
- `EACP_D3D12_SAMPLER_TABLES=0` forces the fallback anywhere, and
  `EACP_D3D12_QUIRKS=1` sets it with the other quirks, so CI's WARP lane can
  run the fallback route.

The default is tables everywhere else because the alternative — static samplers
for everybody — is the design that made bgfx's sampler state inexpressible in
the first place, and the bug is one driver's.

## Note for whoever is next on Windows

`D3D12Context`'s sampler heap and `allocateSamplerDescriptor` are live again:
`Device::makeSampler` writes each distinct descriptor into the heap once, and
nothing frees one before the device goes (the cache is never pruned; a
renderer uses a few dozen at most against 255 slots). After device loss the
cached `Sampler`s point into the old heap, as textures do; nothing rebuilds
them yet.

The `isValid` fix still matters: `Texture::isValid` once required a sampler
descriptor that had stopped being allocated, so **every texture reported
invalid on Windows**. `D3D12TextureData::sampler` is gone with it, and a
texture still carries no sampler.

## Verifying a change here

`Tests/GPU/SamplerTests.cpp` covers the per-bind path: the cache's identity
rules, then every pixel of a 2x2 texture drawn through repeat, clamp, mirror,
border, nearest and linear samplers, two samplers through one pipeline in one
pass, and a comparison sampler (`Less`, then `Greater`) over a depth buffer.
Making the Metal per-bind overload bind the default sampler instead turns seven
of them red, which is how they were checked to be measuring something.

`Tests/GPU/TextureSamplingTests.cpp` covers the declared path on both backends.
It draws a two-texel red|green texture through a shader declaring one
configuration and reads the pixels back:

- Address mode is checked with UVs running `1..2`, entirely outside the texture —
  the only place `Clamp` and `Repeat` differ. `Repeat` brings the red texel back;
  `Clamp` holds green across the whole width.
- Filter is checked by looking for blended pixels, which only `Linear` produces.

`repeatWrapsBackToTheFirstTexel` and `linearBlendsBetweenTexels` are the two that
carry the weight — both fail if a backend reads sampling from anywhere but the
declaration. That was confirmed by making the Metal bind ignore its argument and
watching exactly those two go red.

For a change these cannot reach, the sharpest manual check is a surface whose
UVs leave `[0, 1]` by a lot — a DOOM floor is ideal, its UVs being world
coordinates over 64, so they run to hundreds. Under a broken sampler path such a
surface comes out as a single flat colour, because every pixel samples the same
clamped texel; under a working one it tiles.

A second, independent check: something that *relies* on clamping. eacp's own
COLORMAP-style lookup textures do — a row index past the end is meant to land on
the last row. If those wrap instead, lighting inverts at the extremes and the
result is unmistakable.
