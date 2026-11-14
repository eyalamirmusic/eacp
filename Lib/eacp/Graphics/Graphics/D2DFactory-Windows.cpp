#include "../DComp-Windows.h"
#include "SharedDevices.h"

#include "../Common.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Singleton.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <chrono>
#include <optional>

namespace eacp::Graphics
{

// Defined in CompositionHostWindow-Windows.cpp. DirectComposition binds its
// rendering device at creation and cannot swap it, so a lost device invalidates
// every target, visual and surface — the hosts rebuild theirs, then redraw.
// (Windows.UI.Composition could hot-swap the device via SetRenderingDevice and
// only had to repaint; DComp has no equivalent.)
void rebuildAllCompositionHosts();

namespace
{
Vector<std::function<void()>>& renderingDeviceReplacedListeners()
{
    static auto listeners = Vector<std::function<void()>> {};
    return listeners;
}

Vector<std::function<void()>>& sharedDevicesReleaseListeners()
{
    static auto listeners = Vector<std::function<void()>> {};
    return listeners;
}

// Everything that has to happen once a replacement rendering device is live:
// rebuild and re-render the 2D composition content, then let other modules (the
// GPU device and its swapchains) re-acquire. Listeners must tolerate being
// called with an unchanged device.
void handleRenderingDeviceReplaced()
{
    rebuildAllCompositionHosts();

    for (auto& listener: renderingDeviceReplacedListeners())
        listener();
}

// EACP_DCOMP_UNAVAILABLE=1 (anything but "0") makes composition-device
// creation fail with E_ACCESSDENIED, which is what DCompositionCreateDevice2
// answers in a session with no desktop compositor: a service, a CI runner, a
// job started over ssh into session 0. It lets that state be tested, and
// reproduced, on a desktop that has a compositor. Read when the device is
// built, so it has to be set before the first graphics call.
bool compositionForcedUnavailable()
{
    const auto value = getEnvValue("EACP_DCOMP_UNAVAILABLE");
    return !value.empty() && value != "0";
}

struct InjectedFault
{
    HRESULT hr = S_OK;
    int remaining = 0;
};

using InjectedFaults =
    Array<InjectedFault, static_cast<int>(Detail::DeviceFault::Count)>;

InjectedFaults& injectedFaults()
{
    static auto faults = InjectedFaults {};
    return faults;
}
} // namespace

namespace Detail
{
void injectDeviceFault(DeviceFault point, HRESULT hr, int count)
{
    injectedFaults()[static_cast<int>(point)] = {hr, count};
}

HRESULT takeInjectedDeviceFault(DeviceFault point)
{
    auto& fault = injectedFaults()[static_cast<int>(point)];

    if (fault.remaining <= 0)
        return S_OK;

    --fault.remaining;
    return fault.hr;
}
} // namespace Detail

// Registers a callback for after the shared D3D/D2D device was replaced
// following device loss. Used by eacp-gpu to re-acquire the device and rebuild
// swapchains. Listeners are never unregistered, so only register from
// process-lifetime objects. Main-thread only.
void addRenderingDeviceReplacedListener(std::function<void()> listener)
{
    renderingDeviceReplacedListeners().add(std::move(listener));
}

// Registers what releaseSharedDevices runs before it releases the compositor:
// eacp-gpu's release of its D3D12 device. The same rules as above.
void addSharedDevicesReleaseListener(std::function<void()> listener)
{
    sharedDevicesReleaseListeners().add(std::move(listener));
}

class DCompCompositor
{
public:
    static DCompCompositor& instance()
    {
        auto& compositor = holder();

        if (!compositor)
            compositor.emplace();

        return *compositor;
    }

    // Built again on the next use. A retry still scheduled finds its token
    // superseded and does nothing.
    static void release()
    {
        ++retryToken;
        holder().reset();
    }

    void retryRecoveryIfDue()
    {
        if (retryDue && Clock::now() >= *retryDue)
            retryRecovery();
    }

    // The fresh token is hygiene, not what keeps it to one chain: whichever of
    // the two callbacks fires first either fails, superseding the other, or
    // succeeds, leaving the other nothing to do.
    void resumeRecovery()
    {
        if (!retryDue)
            return;

        const auto untilDue = *retryDue - Clock::now();

        if (untilDue <= Clock::duration::zero())
            retryRecovery();
        else
            scheduleRetry(std::chrono::ceil<std::chrono::milliseconds>(untilDue));
    }

    bool recoverFromDeviceLoss(HRESULT hr)
    {
        if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET
            && hr != D2DERR_RECREATE_TARGET)
            return false;

        return recreateRenderingDevice();
    }

    ID2D1Factory1* getD2DFactory() const { return d2dFactory.Get(); }
    IDWriteFactory* getDWriteFactory() const { return dwriteFactory.Get(); }
    ID3D11Device* getD3DDevice() const { return d3dDevice.Get(); }
    IDXGIDevice* getDXGIDevice() const { return dxgiDevice.Get(); }
    ID2D1Device* getD2DDevice() const { return d2dDevice.Get(); }
    IDCompositionDesktopDevice* getDevice() const { return device.Get(); }

    uint64_t getGeneration() const { return generation; }
    bool isInitialized() const { return initialized; }

    // A failed Commit is itself a device-loss signal, so route it through
    // recovery rather than dropping it: otherwise the tree silently stops
    // updating until something else happens to fail.
    void commit()
    {
        if (!device)
            return;

        if (auto hr = device->Commit(); FAILED(hr))
            recoverFromDeviceLoss(hr);
    }

    // Three tiers, each needing the one before it and each more likely to be
    // missing than the last:
    //
    //   * the DirectWrite and Direct2D factories, which need no device and are
    //     all text measuring, shaping and path geometry use;
    //   * the D3D11 + D2D device pair, which off-screen drawing (renderToImage)
    //     uses;
    //   * the DirectComposition device, which is the only way anything reaches
    //     the screen.
    //
    // A session with no accessible desktop compositor — a service, a CI runner,
    // a job started over ssh into session 0, some plugin hosts — fails the last
    // one, typically with E_ACCESSDENIED, while the first two come up fine. So
    // only the composition device is dropped then: text and snapshots keep
    // working, and on-screen views draw nothing. Every consumer of the
    // composition device guards it being null. No failure may escape into a
    // host (a DAW, a validator) that installed no handler, so each is logged
    // and leaves its tier, and the ones above it, empty.
    DCompCompositor()
    {
        if (auto hr = createFactories(); FAILED(hr))
        {
            LOGF("DCompCompositor: DirectWrite/Direct2D factory creation failed "
                 "(hr=0x{:08x}); no text, drawing or compositing is available.",
                 static_cast<unsigned long>(hr));

            releaseAll();
            return;
        }

        if (auto hr = createRenderingDevice(); FAILED(hr))
        {
            LOGF("DCompCompositor: D3D11/Direct2D device creation failed "
                 "(hr=0x{:08x}); views and renderToImage draw nothing. Text "
                 "still measures and shapes.",
                 static_cast<unsigned long>(hr));

            releaseDevices();
            return;
        }

        if (auto hr = createCompositionDevice(); FAILED(hr))
        {
            logCompositionUnavailable(hr);
            return;
        }

        initialized = true;
    }

    ~DCompCompositor() { releaseAll(); }

private:
    // Never destroyed. eacp links statically into plug-in modules, whose static
    // destructors run on the host's FreeLibrary, under the loader lock, and
    // releasing a device there makes the driver wait for its worker threads to
    // exit, which they need that lock to do: the host hangs. A module unloaded
    // without releaseSharedDevices leaks the devices instead.
    static std::optional<DCompCompositor>& holder()
    {
        return Singleton::getImmortal<std::optional<DCompCompositor>>();
    }

    static void logCompositionUnavailable(HRESULT hr)
    {
        if (compositionForcedUnavailable())
        {
            LOG("DCompCompositor: DirectComposition turned off by "
                "EACP_DCOMP_UNAVAILABLE, standing in for a session with no "
                "desktop compositor; nothing will reach the screen. Text, "
                "Direct2D drawing and renderToImage still work.");
            return;
        }

        LOGF("DCompCompositor: DirectComposition device creation failed "
             "(hr=0x{:08x}); nothing will reach the screen. Expected in a "
             "session with no desktop compositor (a service, a CI runner, "
             "session 0) and in hosts without one. Text, Direct2D drawing and "
             "renderToImage still work.",
             static_cast<unsigned long>(hr));
    }

    HRESULT createFactories()
    {
        auto hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));
        if (FAILED(hr))
            return hr;

        return D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 d2dFactory.GetAddressOf());
    }

    // Creates (or re-creates, after device loss) the D3D + D2D device pair the
    // factories stay independent of.
    HRESULT createRenderingDevice()
    {
        d2dDevice.Reset();
        dxgiDevice.Reset();
        d3dDevice.Reset();

        // A recovery can be asked for (by a GPU device loss, say) in a process
        // whose factories never came up.
        if (!d2dFactory)
            return E_POINTER;

        if (auto hr = Detail::takeInjectedDeviceFault(
                Detail::DeviceFault::CreateRenderingDevice);
            FAILED(hr))
            return hr;

        // BGRA support is required for D2D interop.
        Array featureLevels = {D3D_FEATURE_LEVEL_11_1,
                               D3D_FEATURE_LEVEL_11_0,
                               D3D_FEATURE_LEVEL_10_1,
                               D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL featureLevel;

        // No SINGLETHREADED: the device is shared with the composition engine,
        // which may touch it off-thread.
        auto hr = D3D11CreateDevice(nullptr,
                                    D3D_DRIVER_TYPE_HARDWARE,
                                    nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    featureLevels.data(),
                                    static_cast<UINT>(featureLevels.size()),
                                    D3D11_SDK_VERSION,
                                    d3dDevice.GetAddressOf(),
                                    &featureLevel,
                                    nullptr);

        if (FAILED(hr))
        {
            // Fallback to WARP software renderer
            hr = D3D11CreateDevice(nullptr,
                                   D3D_DRIVER_TYPE_WARP,
                                   nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   featureLevels.data(),
                                   static_cast<UINT>(featureLevels.size()),
                                   D3D11_SDK_VERSION,
                                   d3dDevice.GetAddressOf(),
                                   &featureLevel,
                                   nullptr);
        }

        if (FAILED(hr))
            return hr;

        hr = d3dDevice.As(&dxgiDevice);
        if (FAILED(hr))
            return hr;

        return d2dFactory->CreateDevice(dxgiDevice.Get(), d2dDevice.GetAddressOf());
    }

    // The DComp device takes the D2D device straight in — no separate
    // CompositionGraphicsDevice, and no ICompositorInterop dance to build one.
    HRESULT createCompositionDevice()
    {
        device.Reset();

        if (compositionForcedUnavailable())
            return E_ACCESSDENIED;

        if (auto hr = Detail::takeInjectedDeviceFault(
                Detail::DeviceFault::CreateCompositionDevice);
            FAILED(hr))
            return hr;

        return DCompositionCreateDevice2(d2dDevice.Get(),
                                         IID_PPV_ARGS(device.GetAddressOf()));
    }

    // Never touches the factories: they are device-independent, and the text
    // laid out against them stays good across a device loss.
    bool recreateRenderingDevice()
    {
        compositionWanted = compositionWanted || initialized;

        if (auto hr = createRenderingDevice(); FAILED(hr))
        {
            releaseDevices();
            initialized = false;
            recoveryFailed("D3D11/Direct2D device", hr);
            return false;
        }

        // Optional here as at startup, unless it was up before the loss.
        auto compositionResult = createCompositionDevice();
        initialized = SUCCEEDED(compositionResult);

        if (initialized || !compositionWanted)
        {
            recoveryComplete();
        }
        else if (compositionForcedUnavailable())
        {
            logCompositionUnavailable(compositionResult);
            recoveryComplete();
        }
        else
        {
            recoveryFailed("DirectComposition device", compositionResult);
        }

        // Everything built against the old device — targets, visuals, surfaces —
        // is dead. Moving the generation is what tells every holder to rebuild.
        ++generation;
        handleRenderingDeviceReplaced();

        return true;
    }

    // On the live rendering device, which is kept.
    void recreateCompositionDevice()
    {
        if (auto hr = createCompositionDevice(); FAILED(hr))
        {
            if (compositionForcedUnavailable())
            {
                logCompositionUnavailable(hr);
                recoveryComplete();
            }
            else
            {
                recoveryFailed("DirectComposition device", hr);
            }

            return;
        }

        initialized = true;
        recoveryComplete();

        ++generation;
        handleRenderingDeviceReplaced();
    }

    // With no device nothing draws, so nothing would fail into another
    // recovery: the next attempt is scheduled, backing off between attempts.
    void recoveryFailed(std::string_view what, HRESULT hr)
    {
        if (!recoveryFailing)
            LOGF("DCompCompositor: replacing the lost {} failed (hr=0x{:08x}); "
                 "retrying, from every {} ms up to every {} s.",
                 what,
                 static_cast<unsigned long>(hr),
                 firstRetryDelay.count(),
                 maxRetryDelay.count() / 1000);

        retryDelay = recoveryFailing ? std::min(retryDelay * 2, maxRetryDelay)
                                     : firstRetryDelay;
        recoveryFailing = true;

        if (!d2dFactory)
            return;

        retryDue = Clock::now() + retryDelay;
        scheduleRetry(retryDelay);
    }

    // Each schedule supersedes any callback still pending, late or dropped (a
    // hosted copy detaching stops the scheduler): only the latest one acts.
    void scheduleRetry(std::chrono::milliseconds delay)
    {
        const auto token = ++retryToken;
        Threads::callAfter(Time::MS {delay.count()},
                           [this, token]
                           {
                               if (token == retryToken)
                                   retryRecovery();
                           });
    }

    void retryRecovery()
    {
        if (!d2dDevice)
            recreateRenderingDevice();
        else if (compositionWanted && !initialized)
            recreateCompositionDevice();
    }

    void recoveryComplete()
    {
        if (recoveryFailing)
            LOG("DCompCompositor: the lost devices are replaced.");

        recoveryFailing = false;
        compositionWanted = false;
        retryDue.reset();
    }

    void releaseDevices()
    {
        device.Reset();
        d2dDevice.Reset();
        dxgiDevice.Reset();
        d3dDevice.Reset();
    }

    void releaseAll()
    {
        releaseDevices();
        d2dFactory.Reset();
        dwriteFactory.Reset();
    }

    using Clock = std::chrono::steady_clock;

    static constexpr auto firstRetryDelay = std::chrono::milliseconds {500};
    static constexpr auto maxRetryDelay = std::chrono::milliseconds {30000};

    bool initialized = false;
    uint64_t generation = 1;

    // Composition was up before a recovery that has not brought it back.
    bool compositionWanted = false;
    bool recoveryFailing = false;
    std::chrono::milliseconds retryDelay = firstRetryDelay;
    std::optional<Clock::time_point> retryDue;
    // Shared by every instance, so a release cannot hand a later instance a
    // token an earlier one's pending retry still holds.
    static inline uint64_t retryToken = 0;

    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<IDWriteFactory> dwriteFactory;
    ComPtr<IDCompositionDesktopDevice> device;
};

ID2D1Factory1* getD2DFactory()
{
    return DCompCompositor::instance().getD2DFactory();
}

IDWriteFactory* getDWriteFactory()
{
    return DCompCompositor::instance().getDWriteFactory();
}

ID3D11Device* getD3DDevice()
{
    return DCompCompositor::instance().getD3DDevice();
}

IDXGIDevice* getDXGIDevice()
{
    return DCompCompositor::instance().getDXGIDevice();
}

ID2D1Device* getD2DDevice()
{
    return DCompCompositor::instance().getD2DDevice();
}

IDCompositionDesktopDevice* getCompositionDevice()
{
    return DCompCompositor::instance().getDevice();
}

bool isCompositorInitialized()
{
    return DCompCompositor::instance().isInitialized();
}

void commitComposition()
{
    DCompCompositor::instance().commit();
}

HRESULT insertVisualAtTop(IDCompositionVisual2* parent, IDCompositionVisual2* child)
{
    return parent->AddVisual(child, FALSE, nullptr);
}

HRESULT insertVisualAtBottom(IDCompositionVisual2* parent,
                             IDCompositionVisual2* child)
{
    return parent->AddVisual(child, TRUE, nullptr);
}

uint64_t getCompositionGeneration()
{
    return DCompCompositor::instance().getGeneration();
}

bool handleDeviceLossIfNeeded(HRESULT hr)
{
    return DCompCompositor::instance().recoverFromDeviceLoss(hr);
}

void retryDeviceRecoveryIfDue()
{
    DCompCompositor::instance().retryRecoveryIfDue();
}

void resumeDeviceRecovery()
{
    DCompCompositor::instance().resumeRecovery();
}

void releaseSharedDevices()
{
    for (auto& release: sharedDevicesReleaseListeners())
        release();

    DCompCompositor::release();
}

} // namespace eacp::Graphics
