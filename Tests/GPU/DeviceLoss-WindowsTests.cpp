#include "Common.h"

#include <eacp/Graphics/DComp-Windows.h>

#include <d3d12.h>
#include <winrt/base.h>

// Its own binary because it removes the process-wide D3D12 device, which every
// case after it in a shared suite would then run on.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
int argCount = 0;
char** argValues = nullptr;
int exitCode = 0;

constexpr auto viewSidePixels = 16.f;
constexpr auto framesAfterRecovery = 4;

ID3D12Device* getSharedD3D12Device()
{
    return static_cast<ID3D12Device*>(Device::shared().nativeDevice());
}

bool isSharedDeviceRemoved()
{
    return FAILED(getSharedD3D12Device()->GetDeviceRemovedReason());
}

void removeSharedDeviceAndRecover()
{
    auto removableDevice = winrt::com_ptr<ID3D12Device5>();

    if (SUCCEEDED(getSharedD3D12Device()->QueryInterface(
            __uuidof(ID3D12Device5), removableDevice.put_void())))
    {
        removableDevice->RemoveDevice();
    }

    Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED);
}

struct TimedClearView final : GPUView
{
    TimedClearView()
    {
        setSampleCount(1);
        setBounds({0.f, 0.f, viewSidePixels, viewSidePixels});
    }

    void render(Frame& frame) override
    {
        frame.beginPass(
            {.clearColor = {0.f, 0.4f, 0.f, 1.f}, .clear = true, .label = "timed"});
    }
};

void runTests()
{
    exitCode = nano::run(argCount, argValues);
}
} // namespace

auto tTimedFramesAfterDeviceLoss =
    test("DeviceLoss/timedFramesKeepTheReplacementDeviceAlive") = []
{
    if (!Device::shared().isValid())
    {
        check(true);
        return;
    }

    auto view = TimedClearView {};
    check(view.renderToImage(1.f).isValid());

    if (!Device::shared().supportsPassTimings())
        return;

    const auto* removedDevice = getSharedD3D12Device();
    removeSharedDeviceAndRecover();

    if (getSharedD3D12Device() == removedDevice || isSharedDeviceRemoved())
        return;

    for (auto frame = 0; frame < framesAfterRecovery; ++frame)
        view.renderToImage(1.f);

    check(!isSharedDeviceRemoved());
    check(Device::shared().supportsPassTimings());
};

int main(int argc, char* argv[])
{
    argCount = argc;
    argValues = argv;

    Apps::run(runTests);
    return exitCode;
}
