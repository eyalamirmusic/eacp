#include "GPUView.h"

#include "../Device/Device.h"
#include "../OpenGL/GLBackend-Linux.h"

#include <eacp/Graphics/View/View-Linux.h>

// The presenting half of a GPUView is stage 3's: an EGLSurface over the view's
// wl_egl_window or its X11 child window. Until there is one this backend has no
// drawable at all, which is exactly the headless state the pacing half above it
// already knows how to be in - so a view reports nothing to present and its
// content is drawn by the off-screen renderNativeContent path of
// GPUView-Linux.cpp, over an OffscreenTarget frame like every snapshot.
namespace eacp::GPU
{
namespace
{
struct GLGPUViewBackend final : GPUViewBackend
{
    bool surfaceAvailable() override { return false; }

    void surfaceLost() override {}

    void surfaceResized() override {}

    bool isPresenting() const override { return false; }

    bool readyToRender() override { return false; }

    void renderOneFrame(float) override {}

    // Recorded rather than dropped: the swapchain stage 3 builds is built from
    // these, and a view sets them before it is ever shown.
    void setSampleCount(int count) override { sampleCount = count; }

    void setDepth(bool depth, bool stencil) override
    {
        depthEnabled = depth;
        stencilEnabled = stencil;
    }

    void setFramesInFlight(int count) override { framesInFlight = count; }

    int sampleCount = 1;
    int framesInFlight = 2;
    bool depthEnabled = false;
    bool stencilEnabled = false;
};
} // namespace

std::unique_ptr<GPUViewBackend> makeGLGPUView(GPUView&, Graphics::ViewSurface&)
{
    return std::make_unique<GLGPUViewBackend>();
}
} // namespace eacp::GPU
