#include "Common.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Graphics/DComp-Windows.h>

#include <d3d12.h>
#include <winrt/base.h>

// Its own binary because it removes the process-wide D3D12 device. The replacement
// comes up on WARP, as it did on the machine that crashed after its GPU hung.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
int argCount = 0;
char** argValues = nullptr;
int exitCode = 0;

constexpr auto viewSidePixels = 32.f;
constexpr auto framesToCycleEveryTimerSlotPastTheLostFences = 64;

struct TimedPassView final : GPUView
{
    TimedPassView()
    {
        setSampleCount(1);
        setBounds({0.f, 0.f, viewSidePixels, viewSidePixels});
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass(
            {.clearColor = {0.f, 0.4f, 0.f, 1.f}, .clear = true, .label = "timed"});

        if (!isTimerOnTheCurrentDevice())
            framesTimedOnALostDevice++;
    }

    static bool isTimerOnTheCurrentDevice()
    {
        auto* heap = static_cast<ID3D12QueryHeap*>(
            Device::shared().frameTimer().nativeSamples());

        if (heap == nullptr)
            return true;

        auto heapDevice = winrt::com_ptr<ID3D12Device>();
        heap->GetDevice(__uuidof(ID3D12Device), heapDevice.put_void());

        return heapDevice.get() == Device::shared().nativeDevice();
    }

    int framesTimedOnALostDevice = 0;
};

bool renderTimedFrames(TimedPassView& view)
{
    auto everyFrameDrew = true;

    for (auto frame = 0; frame < framesToCycleEveryTimerSlotPastTheLostFences;
         ++frame)
        everyFrameDrew = everyFrameDrew && view.renderToImage(1.f).isValid();

    return everyFrameDrew;
}

void removeSharedDevice()
{
    auto* device = static_cast<ID3D12Device*>(Device::shared().nativeDevice());
    auto removableDevice = winrt::com_ptr<ID3D12Device5>();

    if (device != nullptr
        && SUCCEEDED(device->QueryInterface(__uuidof(ID3D12Device5),
                                            removableDevice.put_void())))
        removableDevice->RemoveDevice();
}

struct ReplacementDeviceOnWarp
{
    ReplacementDeviceOnWarp() { setEnv("EACP_D3D12_WARP", "1"); }
    ~ReplacementDeviceOnWarp() { unsetEnv("EACP_D3D12_WARP"); }
};

void runTests()
{
    exitCode = nano::run(argCount, argValues);
}
} // namespace

auto tFramesAfterDeviceLossDraw =
    test("DeviceLoss/framesDrawOnTheReplacementDeviceAfterTheOldOneIsRemoved") = []
{
    if (!Device::shared().isValid())
    {
        check(true);
        return;
    }

    auto view = TimedPassView {};
    check(renderTimedFrames(view));
    check(Device::shared().lastFrameTimings().passes.size() == 1);

    const auto* lostDevice = Device::shared().nativeDevice();
    {
        auto onWarp = ReplacementDeviceOnWarp {};
        removeSharedDevice();
        check(Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));
    }

    check(Device::shared().nativeDevice() != lostDevice);
    check(renderTimedFrames(view));
    check(Device::shared().supportsPassTimings());
    check(Device::shared().lastFrameTimings().passes.size() == 1);
    check(view.framesTimedOnALostDevice == 0);
};

int main(int argc, char* argv[])
{
    argCount = argc;
    argValues = argv;
    Apps::run(runTests);
    return exitCode;
}
