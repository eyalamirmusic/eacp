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

Samplers never go near a descriptor heap:

- The render root signature declares a **static sampler for every (texture slot,
  sampling configuration) pair** — `maxTextureSlots * samplingConfigurations` of
  them, currently 16. `D3D12Context::createRootSignatures` builds them.
- `ShaderEmitter` emits each texture's `SamplerState` at register
  `s(slot * samplingConfigurations + samplingIndex(sampling))`. Choosing the
  register chooses the sampler.
- `RenderPass::setFragmentTexture` binds only the SRV table.
- `Frame::beginPass` seeds only the SRV tables. There are no sampler tables left
  to seed.
- `Texture::Native::createDescriptors` allocates **no** sampler descriptor, and
  `TextureDescriptor::filter` / `::addressMode` are unused on this backend.

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
`samplingIndex`; the root signature and the emitter both derive from those, so
they stay in step on their own.

## Unfinished: the Metal backend still reads the Texture

**`RenderPass-Apple` binds its `MTLSamplerState` from the `Texture`, so on Metal
the shader's declared `TextureSampling` is currently ignored.**

Metal has no equivalent of this driver bug — sampler states are bound directly
and work — so nothing is broken there today. The problem is that the two
backends now disagree about where sampling comes from, and the disagreement is
silent: a shader that declares `Repeat` while its texture was created `Clamp`
gets `Repeat` on Windows and `Clamp` on macOS, with nothing to warn you. It will
be found as "this looks different on the other machine", which is the worst way
to find it.

The fix is to make Metal use the shader's declaration too:

1. `ShaderGraph::textureSampling(slot)` already carries it, and `ShaderProgram`
   already reflects it to the bind walk.
2. Build an `MTLSamplerState` per distinct `TextureSampling` (there are at most
   `samplingConfigurations` of them; cache them on the device).
3. In the Apple `setFragmentTexture`, bind the state for the slot's declared
   sampling rather than the one the `Texture` carries.
4. Then `TextureDescriptor::filter` / `::addressMode` are dead on both backends
   and can be removed outright — which is the real end state, since leaving them
   in the API while nothing reads them is its own trap.

Until then, any eacp app must keep a texture's creation-time sampler state in
agreement with the declaration in the shader that samples it. PureDOOM does this
by hand: `WorldShader` and `HudShader` declare `Repeat` because
`makeWorldTexture` creates those textures `Repeat`, and every other shader keeps
the default `Clamp` because its textures are created `Clamp`.

This was written on Windows, where the Metal side could not be built or tested,
which is why step 3 was left rather than guessed at.

## Verifying a change here

There is no automated coverage: this needs a GPU and a look at the screen. The
sharpest manual check is a surface whose UVs leave `[0, 1]` by a lot — a DOOM
floor is ideal, its UVs being world coordinates over 64, so they run to hundreds.
Under a broken sampler path such a surface comes out as a single flat colour,
because every pixel samples the same clamped texel; under a working one it tiles.

A second, independent check: something that *relies* on clamping. eacp's own
COLORMAP-style lookup textures do — a row index past the end is meant to land on
the last row. If those wrap instead, lighting inverts at the extremes and the
result is unmistakable.
