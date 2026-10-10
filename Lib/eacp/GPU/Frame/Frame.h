#pragma once

#include "../CommandBuffer/Copy.h"
#include "ComputePass.h"
#include "RenderPass.h"

#include <span>
#include <string_view>

namespace eacp::GPU
{
class Device;
class Frame;

// What a pass does with the depth and stencil planes it is handed - the load at
// its start and the store at its end, which are one decision because the second
// is only worth paying for when a later pass makes the first one Load.
//
// The two planes move together throughout, both APIs putting them in one
// attachment of one combined format, so this says nothing about depth that it
// does not also say about stencil.
enum class DepthAction
{
    // Clear on entry, discard on exit. What a pass drawing a scene wants: the
    // buffer is scratch space for the pass and nothing after it looks.
    //
    // The discard is not a detail. A tile-based GPU keeps the buffer in tile
    // memory and would otherwise write the whole thing out to memory at the end
    // of every pass, which is the target's size again in bandwidth for pixels
    // nothing reads.
    Clear,

    // Clear on entry, keep on exit, for a pass a later Resume will pick up
    // from.
    Keep,

    // Load what a Keep or Resume pass left, and keep it again.
    //
    // This is what a *suspended* pass needs. A texture cannot be sampled by the
    // pass rendering into it, so an app that wants to copy the frame it has
    // drawn so far - a refraction sampling what is behind it, Doom 3's
    // _currentRender - has to end its pass, copy, and open another. Without
    // this the second one comes back with an empty depth buffer and everything
    // drawn after the copy has nothing to be occluded by.
    //
    // `clear` is the colour's half of the same question and is separate,
    // because the two are not always asked together: the copy above wants the
    // colour kept and so does the depth, while a second *view* on one frame
    // wants the colour kept and the depth cleared.
    Resume
};

// The image of another render target whose depth plane a pass attaches in
// place of its first colour target's - RenderPassDescriptor::depthTarget. A
// depth plane belongs to a colour target and has its shape, so this is how a
// pass into one layer of an array, one face of a cube or one image of a
// volume (which has no depth plane at all) tests against a plain 2D depth
// buffer, and how a pass takes its depth at another level or slice than its
// colour's.
//
// `texture` is a render target created with depth; the pass attaches its
// plane at `level` and from `slice` (the run's first slice on a layered
// pass, which covers ColorTarget::layerCount slices of it too). That level is
// the pass's size, and the texture is single-sampled, as the pass's colour
// targets then are. Anything else is refused like a mismatched colour
// target. What the pass does with the plane is still depthAction, and a
// later pass samples it through `texture` - setFragmentDepthTexture, or
// ...Slice for one slice of an array's.
struct DepthTarget
{
    const Texture* texture = nullptr;
    int level = 0;
    int slice = 0;
};

struct RenderPassDescriptor
{
    Graphics::Color clearColor = Graphics::Color::black();
    bool clear = true;

    // Names this pass for the GPU timer, which is also what turns timing on for
    // it: a labelled pass has its start and end timestamped by the hardware and
    // turns up in Device::lastFrameTimings(), while an unlabelled one is not
    // measured and costs nothing.
    //
    // A label rather than a flag because a frame has several passes and the
    // question worth asking is which of them costs what. Copied as the pass
    // begins, so a temporary is safe.
    //
    // Initialised here rather than left to aggregate initialisation, or every
    // existing `beginPass({colour})` in the tree warns under
    // -Wmissing-field-initializers for the field it does not know about.
    std::string_view label = {};

    // The value the stencil plane is cleared to, where the pass has one. Zero
    // is what a mask wants to start from and what a shadow volume counts up
    // from; a half-way value is what an algorithm that decrements below its
    // start needs, since the plane is unsigned.
    //
    // Read on a pass that clears - DepthAction::Clear or Keep - and ignored by
    // one that resumes, which by definition wants the count the pass before it
    // left. A pass with no stencil plane ignores it either way.
    unsigned char clearStencil = 0;

    // The depth and stencil planes' load and store. Clear by default, which is
    // what every pass did before this existed and what a pass drawing a scene
    // wants.
    DepthAction depthAction = DepthAction::Clear;

    // Where the depth and stencil planes come from on a texture-target pass:
    // the first colour target's, at its level and slice, when this names no
    // texture (the default), and the named target's image otherwise. See
    // DepthTarget. The drawable pass ignores it.
    DepthTarget depthTarget = {};

    // The set RenderPass::beginOcclusionQuery counts into, for every query of
    // the pass. Named here rather than per query because Metal binds the
    // results to the pass as it begins; a set that is not valid is ignored,
    // and the queries of such a pass are refused. The set must outlive the
    // frame. See OcclusionQueries.
    OcclusionQueries* occlusionQueries = nullptr;
};

// One colour attachment of a pass over several targets: the texture, and what
// the pass does with what is already in it. Each attachment clears to its own
// colour, which is what a G-buffer wants - normals cleared to zero beside an
// albedo cleared to the sky.
//
// level and slice pick the 2D image of the texture the pass renders into: any
// mip of any layer, cube face (layer * 6 + face on a cube array) or, on a
// volume, z of that level. The pass is that level's size, and the target's
// depth buffer is attached at the same level and slice, so a cube's caller-
// built chain or a shadow map array is rendered one image at a time.
//
// layerCount above 1 is layered rendering: the pass covers layerCount slices
// from `slice` at once and the vertex shader picks each primitive's -
// [[render_target_array_index]] in MSL, SV_RenderTargetArrayIndex in HLSL,
// gl_Layer in GLSL, counted from `slice` - where Device::supportsLayeredRendering()
// says so. A primitive that writes none lands in the first. Every target of
// one pass takes the same layerCount, and a volume's run starts at z 0.
struct ColorTarget
{
    const Texture* texture = nullptr;
    Graphics::Color clearColor = Graphics::Color::black();
    bool clear = true;

    int level = 0;
    int slice = 0;
    int layerCount = 1;
};

// Off-screen render target for snapshots: a colour texture the app owns instead
// of a swapchain drawable, resolved from msaaTexture when multisampling. A Frame
// built from one renders into it and, on destruction, commits and waits so the
// texture is ready to read back -- it never presents.
struct OffscreenTarget
{
    void* colorTexture = nullptr;
    void* msaaTexture = nullptr;

    // The combined depth-stencil buffer, when the view asked for either. What
    // the pass does with the stencil plane follows from the buffer's format, so
    // there is nothing further to pass here.
    void* depthTexture = nullptr;
};

// Internal: a frame's place on its Device's stack of open frames - the frame
// it is nested in, null for one opened on its own, and the timer slot that
// frame was timing into. See Device::enterFrame.
struct FrameNesting
{
    Frame* outer = nullptr;
    int timerSlot = -1;
};

// One renderable frame: owns the drawable being rendered to plus its command
// buffer. Presents the drawable and commits the command buffer on destruction.
// Created by GPUView each tick and handed to GPUView::render. When msaaTexture
// is non-null the pass renders into it and resolves into the drawable.
class Frame
{
public:
    Frame(Device& device,
          void* drawable,
          void* msaaTexture,
          void* depthTexture,
          float backingScaleToUse = 1.f);

    Frame(Device& device,
          const OffscreenTarget& target,
          float backingScaleToUse = 1.f);

    ~Frame();

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    // The size of the target this frame draws into: the drawable's texture on
    // screen, the app's texture off it.
    //
    // **This is what a projection set inside render() should be derived from.**
    // A size cached in resized() is a different thing that is usually equal to
    // it - what the view will be once the layout pass is over - and the one
    // frame where the two disagree is a live resize, whose drawable is already
    // the new size while the cached divisor is still the old one. Read off the
    // frame, a projection cannot disagree with the viewport it draws through.
    Graphics::Point pixelSize() const;

    // Device pixels per logical point for this frame's target: the view's
    // backing scale on screen, and the scale a snapshot was asked for off it.
    float backingScale() const;

    // pixelSize() in logical points - the units views, mouse events and the
    // sprite and glyph renderers all work in.
    Graphics::Point logicalSize() const;

    RenderPass beginPass(const RenderPassDescriptor& descriptor = {});

    // Renders into an app-owned texture instead of the frame's own target: the
    // app-facing render-to-texture, of which OffscreenTarget above is the
    // snapshot-only ancestor.
    //
    // It is a pass on the *same* frame rather than a frame of its own, which is
    // the whole point. A shader that runs several passes over one image - a
    // Shadertoy's Buffer A through D, a blur's two axes, anything that reads
    // back what a previous pass wrote - wants them on one command buffer, in
    // order, with nothing waiting in between. Passes on a frame are already
    // ordered by the queue, so a texture written by an earlier one is legal to
    // sample in a later one and neither backend needs a fence to say so.
    //
    // The texture must have been created with TextureDescriptor::renderTarget;
    // anything else yields a pass that records nothing.
    //
    // Depth is the target's, from TextureDescriptor::depth: a target created
    // with one gets it attached, cleared to the far plane and discarded, and a
    // target created without one runs the pass with no depth test at all. Ask
    // for it whenever the pass draws a 3D scene rather than a full-screen quad,
    // and match it with RenderPipelineDescriptor::depth on the pipeline.
    //
    // Multisampling is the target's, from TextureDescriptor::sampleCount. Above
    // 1 the pass renders into the multisampled texture beside the target and
    // resolves into the target at its end, which is what the drawable path does
    // with a drawable at the far end of the resolve - so the texture handed in
    // here always holds the resolved picture, whoever samples or reads it, and
    // only the pipeline has to know: match RenderPipelineDescriptor::sampleCount
    // to Texture::sampleCount or the draw is rejected.
    //
    // A texture cannot be sampled by the same pass that renders into it. That
    // is what two of them and a swap is for - see the ping-pong a feedback
    // buffer needs.
    RenderPass beginPass(const Texture& target,
                         const RenderPassDescriptor& descriptor = {});

    // The same over several textures at once, attachment n of the pass being
    // targets[n] and the fragment shader's [[color(n)]], SV_Target<n> or
    // `layout(location = n) out`. Up to maxColorAttachments, each created with
    // TextureDescriptor::renderTarget, and all of one size and one sample
    // count; anything else is refused with a trace and yields a pass that
    // records nothing. The pipeline drawing here names a format per attachment
    // (RenderPipelineDescriptor::extraColorTargets).
    //
    // Depth, stencil and multisampling are the first target's, exactly as the
    // single-target pass takes them: its depth buffer is attached and the
    // others' are ignored, unless descriptor.depthTarget names another target
    // whose plane to attach. Every multisampled target resolves into itself at
    // the end of the pass.
    //
    // One texture may be two targets of the pass when they name different
    // images of it - two levels, two layers of an array, two faces of a cube,
    // two images of a volume, runs of slices that do not meet. The same image
    // twice is refused.
    //
    // Each target names its own level and slice (ColorTarget), and the sizes
    // compared are those levels' sizes, so level 1 of a 64-wide target and
    // level 0 of a 32-wide one are one pass. A level, slice or run of layers
    // the texture lacks is refused like a mismatched size.
    //
    // Each target clears or loads by its own ColorTarget; the descriptor's
    // clearColor and clear are not read, and the rest of it is.
    RenderPass beginMultiTargetPass(std::span<const ColorTarget> targets,
                                    const RenderPassDescriptor& descriptor = {});

    // The same with every target cleared or loaded as the descriptor says, one
    // colour for all of them, which is bgfx's shape for a framebuffer's clear.
    RenderPass beginMultiTargetPass(std::span<const Texture* const> targets,
                                    const RenderPassDescriptor& descriptor = {});

    // Records a compute pass onto this frame's command buffer, in order with
    // its render passes and with nothing waiting in between.
    //
    // This is what lets a kernel's output feed the very draw that consumes it,
    // in the same frame, without the result ever reaching the CPU: a particle
    // integrator writing the buffer the next pass draws as per-instance data, a
    // histogram a fragment shader then reads. The alternative —
    // Device::makeCommandBuffer — is a separate submission whose commit() has to
    // finish before the frame can use a byte of what it wrote.
    //
    // Ordering is the queue's, exactly as it is for two render passes on one
    // frame (see beginPass above), so a buffer written here is legal to bind in
    // a later pass and neither backend needs a fence to say so.
    //
    // A command buffer has one open encoder at a time: let the returned pass end
    // (drop it out of scope, or call end()) before beginning the pass that reads
    // what it wrote.
    //
    // A label times the pass, exactly as it does on a render pass — see
    // RenderPassDescriptor::label.
    //
    // DispatchOrder::Concurrent lets the pass's dispatches overlap, and leaves
    // the ordering between dependent ones to ComputePass::barrier().
    ComputePass beginCompute(std::string_view label = {},
                             DispatchOrder order = DispatchOrder::Serial);

    // Sends everything recorded so far to the GPU and carries on recording, so
    // that the rest of the frame is encoded onto a second submission rather
    // than the same one.
    //
    // There is one reason to want that, and it is the reason this exists:
    // **reading back, inside the frame, something the frame itself drew.**
    // Texture::read and Buffer::read are only valid once the work that wrote
    // them has been committed, and a frame commits when it ends — so without
    // this a screenshot taken mid-frame reads the frame before it. Nothing else
    // needs it: two passes on one frame already see each other's results, which
    // is what beginPass above is at pains to say.
    //
    // **No pass may be open.** A command buffer takes one encoder at a time and
    // cannot be committed while one is live, so end the pass first — the same
    // rule beginCompute states for the pass that follows it.
    //
    // What it costs, beyond the submission: the frame's own GPU time on Metal
    // stops meaning the frame. That number is read off the command buffer, and
    // after a flush there are two, so it measures the part after the last one.
    // The per-pass timings are unaffected on both backends, and so is D3D12's
    // total, which is a pair of queries rather than a property of a list.
    void flush();

    // Copies recorded onto this frame's command buffer, in order with its
    // passes: what an earlier pass wrote is what is copied, and a later pass
    // sees the copy. Each is refused with a trace, recording nothing, when it
    // does not fit both resources - see CommandBuffer/Copy.h for the region
    // types.
    //
    // **No pass may be open**, as for beginCompute and flush: end the pass first, and begin
    // another after the copy if more drawing follows.
    //
    // Texture to texture is between two textures of one format, a level and a
    // slice (a cube's face) on each side; a multisampled target copies its
    // resolved picture, and the depth plane is never copied. A compressed
    // texture copies whole levels only. Source and destination are two
    // resources, never one, for buffers too. The two-argument form copies every
    // level and slice of two textures of one size, level count and shape.
    void copy(const Texture& source, const Texture& destination);
    void copy(const Texture& source,
              const Texture& destination,
              const TextureCopyRegion& region);

    // Buffer to buffer: offsets and length multiples of four, inside both.
    // The two-argument form
    // copies the whole source to the start of the destination.
    void copy(const Buffer& source, const Buffer& destination);
    void copy(const Buffer& source,
              const Buffer& destination,
              std::int64_t sourceOffset,
              std::int64_t destinationOffset,
              std::int64_t bytes);

    // Between a buffer and one level of one slice of an uncompressed texture,
    // the buffer laid out as `layout` says.
    void copy(const Buffer& source,
              const Texture& destination,
              const BufferTextureCopy& layout = {});
    void copy(const Texture& source,
              const Buffer& destination,
              const BufferTextureCopy& layout = {});

    // Fills every level below 0 of every slice of `texture` from level 0, on
    // the GPU and in order with the passes: what an earlier pass or kernel
    // drew into level 0, or what update() or updateSubresource() last put
    // there, is what the chain is built from, and a later pass samples the
    // result. Each level is the one above it filtered down by two (a box
    // filter; the exact weights are each API's own), on a 2D texture, a cube
    // face by face, an array or a cube array slice by slice, and a volume in
    // all three directions.
    //
    // **No pass may be open**, as for copy. A texture of one level is nothing
    // to do. A format Device::supportsMipGeneration refuses - every block-
    // compressed and integer format, and some others per device - is refused
    // with a trace and nothing recorded. A multisampled target has one level.
    //
    // Metal records generateMipmapsForTexture on a blit encoder; D3D12 a
    // compute dispatch per level through the texture's per-level UAVs, which
    // any texture of more than one level in such a format is created with;
    // Vulkan a vkCmdBlitImage per level with a linear filter.
    void generateMipmaps(const Texture& texture);

    // The format of the target beginPass() renders into: the drawable's on
    // screen, the snapshot's texture off it. BGRA8Unorm wherever eacp makes
    // the target itself, which is every view; a Vulkan surface that offers
    // only RGBA8 answers RGBA8Unorm, and an sRGB swapchain its sRGB twin.
    TextureFormat drawableFormat() const;

    // Copies what beginPass() has drawn into this frame's own target so far -
    // the drawable on screen, the snapshot texture off it - into level 0 of
    // `destination`, in order with the passes. It is the window's screenshot:
    // the drawable lives only as long as the frame, and this is the one way its
    // pixels reach a texture that outlives it, for Texture::read to hand back
    // once the frame has ended (or after flush()).
    //
    // Call it after the last pass that draws into the drawable, **with no pass
    // open**, as for copy. A multisampled view copies the resolved picture.
    // `destination` is a TwoD, single-sampled texture of drawableFormat() and
    // exactly pixelSize(); anything else is refused with a trace, and so is a
    // drawable the view did not make readable (GPUView::setReadableDrawable),
    // which on Metal and Vulkan is created able to be presented and nothing
    // else. Returns whether the copy was recorded.
    bool copyDrawable(const Texture& destination);

    bool isValid() const;

private:
    static bool canRenderInto(std::span<const ColorTarget> targets,
                              const RenderPassDescriptor& descriptor);

    // The render target whose depth plane the pass attaches and the image of
    // it: the first colour target's own, or descriptor.depthTarget's, with the
    // first target's layerCount.
    static ColorTarget depthSource(std::span<const ColorTarget> targets,
                                   const RenderPassDescriptor& descriptor);

    // The set a pass counts its occlusion queries into: the descriptor's, when
    // it is valid, and null otherwise.
    static OcclusionQueries*
        occlusionQueriesFor(const RenderPassDescriptor& descriptor);

    // Whether `destination` is a texture copyDrawable may write: TwoD,
    // single-sampled, drawableFormat() and pixelSize(). Traces the refusal.
    bool canCopyDrawableInto(const Texture& destination) const;

    void* nativeRecording() const;

    // Publishes this frame's recording again as the one CPU uploads go onto,
    // after a frame nested in it has ended and withdrawn its own (D3D12 and
    // Vulkan; nothing on Metal). Device::leaveFrame calls it.
    void resumeRecording();
    friend class Device;

    // Entered before impl is built and left after it is gone, so a nested
    // frame's whole life - its acquire, its recording, its submit and present -
    // falls between the outer frame's flush and its resumption.
    struct Nesting
    {
        Nesting(Device& device, Frame& frame);
        ~Nesting();

        Nesting(const Nesting&) = delete;
        Nesting& operator=(const Nesting&) = delete;

        Device* device;
        FrameNesting place;
    };

    Nesting nesting;

    struct Native;
    Pimpl<Native> impl;

    float scale = 1.f;
};
} // namespace eacp::GPU
