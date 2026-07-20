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
`drawTexture`. It keeps an `Array<std::optional<SpriteProgram>,
samplingConfigurations>` built on first use, and `drawTexture` takes a
`TextureSampling` defaulting to `Nearest`. Switching sampling mid-frame costs a
pipeline change, so batching by sampling is worth it where it is easy.

## Note for whoever is next on Windows

`D3D12Context` still carries the sampler heap and
`allocateSamplerDescriptor` / `freeSamplerDescriptor`, and `Frame-Windows` still
binds the heap in `SetDescriptorHeaps`. Nothing allocates from it any more — it
is dead weight, harmless but exactly the kind of API that looks live. It was
left because this pass was written on macOS, where the D3D12 side cannot be
compiled, and deleting it unverified is the guess this file exists to avoid.

The same constraint is why the `isValid` fix matters: `Texture::isValid` still
required a sampler descriptor that had stopped being allocated, so **every
texture reported invalid on Windows**. `D3D12TextureData::sampler` is gone with
it.

## Verifying a change here

`Tests/GPU/TextureSamplingTests.cpp` covers this automatically on both backends.
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
