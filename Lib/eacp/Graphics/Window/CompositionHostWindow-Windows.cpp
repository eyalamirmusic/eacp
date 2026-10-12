#include "CompositionHostWindow-Windows.h"
#include <eacp/Core/Utils/Strings.h>
#include "../Helpers/ImageConversion-Windows.h"
#include "../Helpers/SystemAppearance.h"
#include "../Image/ImageOps.h"
#include "../Layers/NativeLayer-Windows.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Singleton.h>

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ranges>
#include <unordered_map>

namespace eacp::Graphics
{

// paintDirtyViewsForHost() is defined in View-Windows.cpp; the composition
// device accessors come from DComp-Windows.h. Both are linked earlier in the
// unity build.
void paintDirtyViewsForHost(HWND host);
void redrawAllCompositionHosts();

// Defined in Keyboard-Windows.cpp: Windows virtual key -> framework KeyCode
// (KeyCode::Unknown when unmapped), so KeyEvent::keyCode means the same thing
// on every platform.
uint16_t keyCodeFromVirtualKey(int vk);
uint16_t keyCodeFromKeyMessage(int vk, LPARAM lParam);

namespace
{
// Immortal because ~CompositionHostWindow unregisters here, and a Window at
// namespace scope outlives a registry that first use constructed after it.
std::unordered_map<View*, HWND>& contentViewToHwnd()
{
    return Singleton::getImmortal<std::unordered_map<View*, HWND>>();
}

// Every live host, so a device loss can rebuild each one's target and root.
// Main-thread only and immortal, like the view/HWND registry above.
Vector<CompositionHostWindow*>& compositionHosts()
{
    return Singleton::getImmortal<Vector<CompositionHostWindow*>>();
}

// Virtual key codes - defined manually to reduce Windows.h dependency.
namespace VK
{
constexpr uint16_t Shift = 0x10;
constexpr uint16_t Control = 0x11;
constexpr uint16_t Menu = 0x12; // Alt key
constexpr uint16_t LWin = 0x5B;
constexpr uint16_t RWin = 0x5C;
} // namespace VK

int getXFromLParam(LPARAM lp)
{
    return static_cast<int>(static_cast<short>(LOWORD(lp)));
}

int getYFromLParam(LPARAM lp)
{
    return static_cast<int>(static_cast<short>(HIWORD(lp)));
}

MouseButton buttonForDown(UINT msg)
{
    return msg == WM_LBUTTONDOWN   ? MouseButton::Left
           : msg == WM_RBUTTONDOWN ? MouseButton::Right
                                   : MouseButton::Middle;
}

MouseButton buttonForUp(UINT msg)
{
    return msg == WM_LBUTTONUP   ? MouseButton::Left
           : msg == WM_RBUTTONUP ? MouseButton::Right
                                 : MouseButton::Middle;
}

MouseEvent makeMouseEvent(LPARAM lParam,
                          float scale,
                          MouseEventType type,
                          const ModifierKeys& modifiers)
{
    MouseEvent event;
    event.pos = {static_cast<float>(getXFromLParam(lParam)) / scale,
                 static_cast<float>(getYFromLParam(lParam)) / scale};
    event.type = type;
    event.modifiers = modifiers;
    return event;
}

HCURSOR systemCursorFor(MouseCursor::Shape shape)
{
    switch (shape)
    {
        case MouseCursor::IBeam:
            return LoadCursor(nullptr, IDC_IBEAM);
        case MouseCursor::PointingHand:
            return LoadCursor(nullptr, IDC_HAND);
        case MouseCursor::ResizeLeftRight:
            return LoadCursor(nullptr, IDC_SIZEWE);
        case MouseCursor::ResizeUpDown:
            return LoadCursor(nullptr, IDC_SIZENS);
        case MouseCursor::Crosshair:
            return LoadCursor(nullptr, IDC_CROSS);
        case MouseCursor::Default:
            break;
    }

    return LoadCursor(nullptr, IDC_ARROW);
}

struct ImageCursorHandle
{
    ImageCursorHandle(HCURSOR handleToUse, int widthToUse, int heightToUse)
        : handle(handleToUse)
        , width(widthToUse)
        , height(heightToUse)
    {
    }

    ~ImageCursorHandle()
    {
        if (handle != nullptr)
            DestroyIcon(handle);
    }

    ImageCursorHandle(const ImageCursorHandle&) = delete;
    ImageCursorHandle& operator=(const ImageCursorHandle&) = delete;

    HCURSOR handle = nullptr;
    int width = 0;
    int height = 0;
};

int scaledCursorExtent(int pixels, float factor)
{
    return std::max(
        1, static_cast<int>(std::lround(static_cast<float>(pixels) * factor)));
}

int scaledHotspot(float pixels, float factor, int extent)
{
    return std::clamp(static_cast<int>(std::lround(pixels * factor)), 0, extent - 1);
}

HCURSOR imageCursorFor(const MouseCursor& cursor, float dpiScale)
{
    auto image = cursor.getImage();
    auto factor = dpiScale / cursor.getScale();
    auto width = scaledCursorExtent(image.width(), factor);
    auto height = scaledCursorExtent(image.height(), factor);

    auto& cache = *cursor.getNativeCache();
    auto* cached = static_cast<ImageCursorHandle*>(cache.get());

    if (cached != nullptr && cached->width == width && cached->height == height)
        return cached->handle;

    auto sized = width == image.width() && height == image.height()
                     ? image
                     : resizeBilinear(image, width, height);

    auto hotspot = cursor.getHotspot();
    auto handle = toHCursor(sized,
                            scaledHotspot(hotspot.x, factor, width),
                            scaledHotspot(hotspot.y, factor, height));

    if (handle == nullptr)
        return systemCursorFor(MouseCursor::Default);

    cache = std::make_shared<ImageCursorHandle>(handle, width, height);
    return handle;
}

HCURSOR cursorHandleFor(const MouseCursor& cursor, float dpiScale)
{
    if (cursor.isImage())
        return imageCursorFor(cursor, dpiScale);

    return systemCursorFor(cursor.getShape());
}

Vector<std::string> draggedFilePaths(IDataObject* data)
{
    auto paths = Vector<std::string> {};

    if (data == nullptr)
        return paths;

    auto format = FORMATETC {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    auto medium = STGMEDIUM {};

    if (FAILED(data->GetData(&format, &medium)))
        return paths;

    if (auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal)))
    {
        auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);

        for (auto index = UINT {0}; index < count; ++index)
        {
            auto length = DragQueryFileW(drop, index, nullptr, 0);
            auto path = std::wstring(length, L'\0');
            DragQueryFileW(drop, index, path.data(), length + 1);
            paths.add(Strings::narrow(path));
        }

        GlobalUnlock(medium.hGlobal);
    }

    ReleaseStgMedium(&medium);
    return paths;
}

DWORD dropEffectFor(bool accepted)
{
    return accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

POINT toPoint(POINTL point)
{
    return {point.x, point.y};
}

class HostFileDropTarget final : public IDropTarget
{
public:
    explicit HostFileDropTarget(CompositionHostWindow& hostToUse)
        : host(&hostToUse)
    {
    }

    void detach() { host = nullptr; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropTarget)
        {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        auto remaining = --refCount;

        if (remaining == 0)
            delete this;

        return remaining;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data,
                                        DWORD,
                                        POINTL point,
                                        DWORD* effect) override
    {
        files = draggedFilePaths(data);
        *effect = dropEffectFor(host != nullptr && !files.empty()
                                && host->fileDragOver(files, toPoint(point)));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override
    {
        *effect = dropEffectFor(host != nullptr && !files.empty()
                                && host->fileDragOver(files, toPoint(point)));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        if (host != nullptr)
            host->fileDragLeave();

        files.clear();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data,
                                   DWORD,
                                   POINTL point,
                                   DWORD* effect) override
    {
        auto dropped = draggedFilePaths(data);
        files.clear();

        *effect = dropEffectFor(host != nullptr && !dropped.empty()
                                && host->fileDrop(dropped, toPoint(point)));
        return S_OK;
    }

private:
    CompositionHostWindow* host = nullptr;
    Vector<std::string> files;
    std::atomic<ULONG> refCount {1};
};

bool isInViewTree(const View* root, const View* view)
{
    if (root == nullptr)
        return false;

    if (root == view)
        return true;

    for (auto* subview: root->getSubviews())
        if (isInViewTree(subview, view))
            return true;

    return false;
}

View* deepestFileDropView(View& view, Point position)
{
    if (!view.isVisible() || !view.getLocalBounds().contains(position))
        return nullptr;

    for (auto* child: std::ranges::reverse_view(view.getSubviews()))
    {
        auto childBounds = child->getBounds();

        if (auto* found = deepestFileDropView(
                *child, {position.x - childBounds.x, position.y - childBounds.y}))
            return found;
    }

    return view.getProperties().acceptsFileDrops ? &view : nullptr;
}

Point positionInView(const View& view, Point positionInRoot)
{
    auto local = positionInRoot;

    for (auto* current = &view; current->getParent() != nullptr;
         current = current->getParent())
    {
        auto bounds = current->getBounds();
        local.x -= bounds.x;
        local.y -= bounds.y;
    }

    return local;
}

void registerContentViewHwnd(View* root, HWND hwnd)
{
    contentViewToHwnd()[root] = hwnd;
}

void unregisterContentViewHwnd(View* root)
{
    contentViewToHwnd().erase(root);
}
} // namespace

HWND findHostHwndForView(View* view)
{
    auto* root = view;
    while (root && root->getParent())
        root = root->getParent();

    if (!root)
        return nullptr;

    auto& map = contentViewToHwnd();
    auto it = map.find(root);
    return it == map.end() ? nullptr : it->second;
}

void refreshHostCursor(HWND hwnd)
{
    if (hwnd == nullptr)
        return;

    for (auto* host: compositionHosts())
        if (host->hwnd == hwnd)
            host->refreshCursor();
}

void repaintViewTree(View* view)
{
    if (!view)
        return;

    view->repaint();

    for (auto* subview: view->getSubviews())
        repaintViewTree(subview);
}

namespace
{
// A native surface a view owns outside the composition tree (a WebView's
// browser window) does not follow the host window on its own, so the whole
// tree is told when it moves or its visibility changes.
void notifyViewTreeOfHostMove(View* view)
{
    if (!view)
        return;

    view->hostWindowMoved();

    for (auto* subview: view->getSubviews())
        notifyViewTreeOfHostMove(subview);
}

void notifyViewTreeOfHostVisibility(View* view, bool visible)
{
    if (!view)
        return;

    view->hostWindowVisibilityChanged(visible);

    for (auto* subview: view->getSubviews())
        notifyViewTreeOfHostVisibility(subview, visible);
}

void markViewTreeLayersDirty(const View* view)
{
    if (!view)
        return;

    for (auto* layer: view->getLayers())
        if (auto* native = static_cast<NativeLayerBase*>(layer->getNativeLayer()))
            native->markContentDirty();

    for (auto* subview: view->getSubviews())
        markViewTreeLayersDirty(subview);
}

} // namespace

bool isHostWindowTransparent(HWND hwnd)
{
    if (hwnd == nullptr)
        return false;

    for (auto* host: compositionHosts())
        if (host->hwnd == hwnd)
            return host->transparentBackground;

    return false;
}

// Called by the rendering-device recovery in D2DFactory-Windows.cpp. Unlike
// Windows.UI.Composition — which could hot-swap the rendering device and keep
// its surfaces (they merely lost their pixels) — DirectComposition binds the
// device at creation, so the target, the root visual and every layer/view visual
// below them are dead objects. Rebuild the hosts' targets first, then let the
// generation stamp on each view/layer pull the rest through on the redraw.
void rebuildAllCompositionHosts()
{
    for (auto* host: compositionHosts())
    {
        host->initializeComposition(host->topMostTarget);

        if (host->contentView)
            host->attachContentView(host->contentView);
    }

    redrawAllCompositionHosts();
}

// Re-renders every layer and repaints every painting view of all
// composition-hosted windows.
void redrawAllCompositionHosts()
{
    for (auto& [root, hostHwnd]: contentViewToHwnd())
    {
        markViewTreeLayersDirty(root);
        repaintViewTree(root);
        InvalidateRect(hostHwnd, nullptr, FALSE);
    }
}

void CompositionHostWindow::initializeComposition(bool topMost)
{
    if (!hwnd)
        return;

    topMostTarget = topMost;

    auto* device = getCompositionDevice();
    if (!device)
        return;

    EA::Vectors::addIfNotThere(compositionHosts(), this);

    // Drop anything built against a previous device before rebuilding.
    rootVisual.Reset();
    target.Reset();
    generation = getCompositionGeneration();

    if (FAILED(device->CreateTargetForHwnd(hwnd, topMost, target.GetAddressOf())))
    {
        target.Reset();
        return;
    }

    if (FAILED(device->CreateVisual(rootVisual.GetAddressOf())))
    {
        rootVisual.Reset();
        target.Reset();
        return;
    }

    rescaleRootVisualToDpi();
    target->SetRoot(rootVisual.Get());
    commitComposition();
}

float CompositionHostWindow::getDpiScale() const
{
    if (dpiScaleOverride > 0.f)
        return dpiScaleOverride;

    auto dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
    return static_cast<float>(dpi) / 96.f;
}

void CompositionHostWindow::setDpiScaleOverride(float scale)
{
    if (dpiScaleOverride == scale)
        return;

    dpiScaleOverride = scale;

    if (!hwnd)
        return;

    // Everything already laid out was measured against the old answer: the root
    // transform maps points to pixels with it, and the content view's size in
    // points came from dividing the client area by it.
    rescaleRootVisualToDpi();
    resizeContentViewToClient();
}

// The root maps logical points to physical pixels for the whole tree. Content
// visuals below it counter-scale by 1/dpiScale so their physical-pixel surfaces
// land 1:1 — see the note in NativeLayer-Windows.h.
void CompositionHostWindow::rescaleRootVisualToDpi()
{
    if (!rootVisual)
        return;

    auto scale = getDpiScale();
    rootVisual->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    commitComposition();
}

void CompositionHostWindow::attachContentView(View* view)
{
    if (contentView && contentView != view)
        unregisterContentViewHwnd(contentView);

    contentView = view;

    if (!hwnd || !view)
        return;

    registerContentViewHwnd(view, hwnd);
    registerFileDrops();

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    auto scale = getDpiScale();
    view->setBounds({0.f,
                     0.f,
                     static_cast<float>(clientRect.right) / scale,
                     static_cast<float>(clientRect.bottom) / scale});
    Threads::callAsync(
        [weak = std::weak_ptr<CompositionHostWindow*>(lifetime)]
        {
            if (auto host = weak.lock())
                (*host)->layOutContentView();
        });

    auto* viewVisual = static_cast<IDCompositionVisual2*>(view->getHandle());

    if (rootVisual && viewVisual)
        insertVisualAtTop(rootVisual.Get(), viewVisual);

    ensureAllLayersRendered(view);
}

void CompositionHostWindow::ensureAllLayersRendered(const View* view) const
{
    ensureAllLayersRendered(view, getDpiScale());

    // DComp batches every visual and surface change made above; nothing reaches
    // the screen until the device is flushed. WinRT committed implicitly, so
    // this is the one call the old backend never needed.
    commitComposition();
}

void CompositionHostWindow::ensureAllLayersRendered(const View* view,
                                                    float dpiScale) const
{
    if (!view)
        return;

    for (auto* layer: view->getLayers())
    {
        auto* native = static_cast<NativeLayerBase*>(layer->getNativeLayer());
        if (native)
        {
            native->setDpiScale(dpiScale);
            native->ensureContent();
        }
    }

    for (auto* subview: view->getSubviews())
        ensureAllLayersRendered(subview, dpiScale);
}

bool CompositionHostWindow::isKeyPressed(uint16_t vk) const
{
    return vk < 256 && keyState.test(vk);
}

bool CompositionHostWindow::isShiftPressed() const
{
    return isKeyPressed(VK::Shift);
}

bool CompositionHostWindow::isControlPressed() const
{
    return isKeyPressed(VK::Control);
}

bool CompositionHostWindow::isAltPressed() const
{
    return isKeyPressed(VK::Menu);
}

bool CompositionHostWindow::isCommandPressed() const
{
    return isKeyPressed(VK::LWin) || isKeyPressed(VK::RWin);
}

ModifierKeys CompositionHostWindow::getModifiers() const
{
    return {
        isShiftPressed(), isControlPressed(), isAltPressed(), isCommandPressed()};
}

void CompositionHostWindow::setMouseLocked(bool locked)
{
    if (mouseLockIntent == locked)
        return;

    mouseLockIntent = locked;

    if (locked && GetFocus() == hwnd)
        engageMouseLock();
    else if (!locked)
        disengageMouseLock();
}

bool CompositionHostWindow::applyCursorUnderPointer()
{
    if (contentView == nullptr || mouseLockEngaged)
        return false;

    auto screen = POINT {};
    GetCursorPos(&screen);

    auto* hit = contentView->hitTest(fromScreenPixels(screen));
    const auto& cursor =
        hit != nullptr ? hit->getMouseCursor() : contentView->getMouseCursor();

    SetCursor(cursorHandleFor(cursor, getDpiScale()));

    auto* cache = cursor.getNativeCache();
    appliedImageCursor = cache != nullptr ? *cache : nullptr;

    return true;
}

void CompositionHostWindow::refreshCursor()
{
    auto screen = POINT {};
    GetCursorPos(&screen);

    auto capture = GetCapture();

    if (capture == hwnd || (capture == nullptr && WindowFromPoint(screen) == hwnd))
        applyCursorUnderPointer();
}

void CompositionHostWindow::engageMouseLock()
{
    if (mouseLockEngaged)
        return;

    mouseLockEngaged = true;
    ShowCursor(FALSE);
    clipCursorToClient();

    auto center = clientCenter();
    ClientToScreen(hwnd, &center);
    SetCursorPos(center.x, center.y);
}

void CompositionHostWindow::disengageMouseLock()
{
    if (!mouseLockEngaged)
        return;

    mouseLockEngaged = false;
    ClipCursor(nullptr);
    ShowCursor(TRUE);
}

void CompositionHostWindow::clipCursorToClient() const
{
    RECT client {};
    GetClientRect(hwnd, &client);

    POINT topLeft {client.left, client.top};
    POINT bottomRight {client.right, client.bottom};
    ClientToScreen(hwnd, &topLeft);
    ClientToScreen(hwnd, &bottomRight);

    RECT screenRect {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    ClipCursor(&screenRect);
}

POINT CompositionHostWindow::clientCenter() const
{
    RECT client {};
    GetClientRect(hwnd, &client);
    return {client.right / 2, client.bottom / 2};
}

void CompositionHostWindow::handleLockedMouseMove(LPARAM lParam)
{
    auto center = clientCenter();
    auto x = getXFromLParam(lParam);
    auto y = getYFromLParam(lParam);

    // The echo of our own recentering SetCursorPos carries no motion.
    if (x == center.x && y == center.y)
        return;

    auto screenCenter = center;
    ClientToScreen(hwnd, &screenCenter);
    SetCursorPos(screenCenter.x, screenCenter.y);

    if (!contentView)
        return;

    auto scale = getDpiScale();
    MouseEvent event;
    event.type = MouseEventType::Moved;
    event.modifiers = getModifiers();
    event.pos = {static_cast<float>(center.x) / scale,
                 static_cast<float>(center.y) / scale};
    event.delta = {static_cast<float>(x - center.x) / scale,
                   static_cast<float>(y - center.y) / scale};
    dispatchMouseToContentView(event);
}

void CompositionHostWindow::registerFileDrops()
{
    if (fileDropTarget != nullptr || hwnd == nullptr)
        return;

    static thread_local auto oleReady = SUCCEEDED(OleInitialize(nullptr));

    if (!oleReady)
        return;

    auto* target = new HostFileDropTarget(*this);

    if (FAILED(RegisterDragDrop(hwnd, target)))
    {
        target->Release();
        return;
    }

    fileDropTarget = target;
}

void CompositionHostWindow::revokeFileDrops()
{
    if (fileDropTarget == nullptr)
        return;

    RevokeDragDrop(hwnd);
    static_cast<HostFileDropTarget*>(fileDropTarget)->detach();
    fileDropTarget->Release();
    fileDropTarget = nullptr;
    fileDragView = nullptr;
}

View* CompositionHostWindow::fileDropViewAt(Point position) const
{
    return contentView != nullptr ? deepestFileDropView(*contentView, position)
                                  : nullptr;
}

bool CompositionHostWindow::fileDragOver(const Vector<std::string>& files,
                                         POINT screenPixels)
{
    auto position = fromScreenPixels(screenPixels);
    auto* view = fileDropViewAt(position);

    if (view != fileDragView)
    {
        fileDragLeave();
        fileDragView = view;

        return view != nullptr
               && view->fileDragEnter(files, positionInView(*view, position));
    }

    return view != nullptr
           && view->fileDragMove(files, positionInView(*view, position));
}

void CompositionHostWindow::fileDragLeave()
{
    if (fileDragView != nullptr && isInViewTree(contentView, fileDragView))
        fileDragView->fileDragExit();

    fileDragView = nullptr;
}

bool CompositionHostWindow::fileDrop(const Vector<std::string>& files,
                                     POINT screenPixels)
{
    auto position = fromScreenPixels(screenPixels);
    auto* view = fileDropViewAt(position);

    if (view != fileDragView)
        fileDragLeave();

    fileDragView = nullptr;

    if (view == nullptr)
        return false;

    view->filesDropped(files, positionInView(*view, position));
    return true;
}

void CompositionHostWindow::teardown()
{
    disengageMouseLock();
    revokeFileDrops();

    if (rootVisual)
        rootVisual->RemoveAllVisuals();

    rootVisual.Reset();
    target.Reset();
    commitComposition();

    EA::Vectors::eraseIf(compositionHosts(),
                         [this](auto* host) { return host == this; });

    if (contentView)
        unregisterContentViewHwnd(contentView);

    if (hwnd)
        DestroyWindow(hwnd);
}

// The DComp visual tree composites above the window's GDI redirection bitmap,
// so wherever the app's content is transparent the bitmap shows through. Fill
// it with the theme's window background — the counterpart of NSWindow's
// windowBackgroundColor on macOS. Left unpainted it appears as a white
// rectangle frozen at the window's creation size (resizes reallocate the
// bitmap but nothing ever painted it).
//
// A transparent window has no redirection bitmap at all, and painting one is
// exactly the opaque background it was created to avoid.
void CompositionHostWindow::fillWindowBackground(HDC dc) const
{
    if (!dc || transparentBackground)
        return;

    RECT client {};
    GetClientRect(hwnd, &client);

    auto color = isSystemDarkMode() ? RGB(32, 32, 32) : RGB(243, 243, 243);
    auto brush = CreateSolidBrush(color);
    FillRect(dc, &client, brush);
    DeleteObject(brush);
}

void CompositionHostWindow::resizeContentViewToClient()
{
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    auto scale = getDpiScale();
    auto widthInPoints = static_cast<float>(clientRect.right) / scale;
    auto heightInPoints = static_cast<float>(clientRect.bottom) / scale;

    if (contentView)
    {
        contentView->setBounds(Rect(0, 0, widthInPoints, heightInPoints));
        ensureAllLayersRendered(contentView);
    }

    if (onContentResized)
        onContentResized(static_cast<int>(widthInPoints),
                         static_cast<int>(heightInPoints));
}

// Attaching sizes the content view at once, usually inside the constructor of
// the app that owns the window, before it has added the subviews its resized()
// places. The window is shown and painted there too, so a paint is too early;
// this pass is deferred a turn, as AppKit lays a window out before first
// display. Without it those subviews keep zero bounds until a WM_SIZE, which a
// window shown at its created size never gets.
void CompositionHostWindow::layOutContentView()
{
    if (contentView != nullptr)
        contentView->setBounds(contentView->getBounds());
}

void CompositionHostWindow::ensureMouseLeaveTracking()
{
    if (trackingMouseLeave)
        return;

    TRACKMOUSEEVENT tme {};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd;
    TrackMouseEvent(&tme);
    trackingMouseLeave = true;
}

void CompositionHostWindow::ensureRawMouseRegistered()
{
    if (rawMouseRegistered || hwnd == nullptr)
        return;

    RAWINPUTDEVICE mouse = {};
    mouse.usUsagePage = 0x01; // generic desktop
    mouse.usUsage = 0x02; // mouse

    // No RIDEV_NOLEGACY: the ordinary pointer messages must keep coming, since
    // a MouseEvent reports the pointer's movement as well as the device's.
    mouse.hwndTarget = hwnd;

    rawMouseRegistered = RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
}

void CompositionHostWindow::accumulateRawMouseMovement(LPARAM lParam)
{
    RAWINPUT input = {};
    UINT size = sizeof(input);

    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam),
                        RID_INPUT,
                        &input,
                        &size,
                        sizeof(RAWINPUTHEADER))
        == static_cast<UINT>(-1))
        return;

    if (input.header.dwType != RIM_TYPEMOUSE)
        return;

    // A tablet or a remote desktop reports where its pointer *is*, not how far
    // the hand moved it; there is no device movement in that to report.
    if ((input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0)
        return;

    rawMouseMovement.x += static_cast<float>(input.data.mouse.lLastX);
    rawMouseMovement.y += static_cast<float>(input.data.mouse.lLastY);
}

POINT CompositionHostWindow::toScreenPixels(Point position) const
{
    const auto scale = getDpiScale();

    POINT pixels {static_cast<LONG>(position.x * scale),
                  static_cast<LONG>(position.y * scale)};

    ClientToScreen(hwnd, &pixels);
    return pixels;
}

Point CompositionHostWindow::fromScreenPixels(POINT screenPixels) const
{
    auto pixels = screenPixels;
    ScreenToClient(hwnd, &pixels);

    const auto scale = getDpiScale();

    return {static_cast<float>(pixels.x) / scale,
            static_cast<float>(pixels.y) / scale};
}

void CompositionHostWindow::dispatchMouseToContentView(MouseEvent event)
{
    // Where the drag began, kept in SCREEN pixels and converted back on every
    // event — the client area can move under a stationary pointer (the window
    // dragged, an embedded editor a plugin host is resizing), and a start point
    // kept in client space would report that movement as pointer travel. A
    // wheel event has no drag behind it, so it reports itself.
    if (event.type == MouseEventType::Wheel || event.type == MouseEventType::Magnify)
    {
        event.downPos = event.pos;
    }
    else
    {
        if (event.type == MouseEventType::Down)
            mouseDownScreenPosition = toScreenPixels(event.pos);

        event.downPos = fromScreenPixels(mouseDownScreenPosition);
    }

    if (event.type == MouseEventType::Moved || event.type == MouseEventType::Dragged)
    {
        // A locked move already carries its delta: the pointer is put back in
        // the middle after every report, so its position says nothing.
        if (!mouseLockEngaged)
            event.delta =
                lastPointerPosition ? event.pos - *lastPointerPosition : Point {};

        lastPointerPosition = event.pos;

        // Whatever Raw Input has gathered since the last movement was reported.
        // A device that cannot report its own movement (a tablet, a remote
        // desktop) leaves this empty, and the pointer's movement stands in.
        auto moved = rawMouseMovement.x != 0.0f || rawMouseMovement.y != 0.0f;

        event.rawDelta = moved ? rawMouseMovement : event.delta;
        rawMouseMovement = {};
    }
    else if (event.type == MouseEventType::Down || event.type == MouseEventType::Up)
    {
        lastPointerPosition = event.pos;
    }
    else if (event.type == MouseEventType::Exited)
    {
        lastPointerPosition.reset();
    }

    contentView->dispatchMouseEvent(event);
    ensureAllLayersRendered(contentView);
}

bool CompositionHostWindow::dispatchZoomGesture(LPARAM lParam)
{
    auto handle = reinterpret_cast<HGESTUREINFO>(lParam);
    auto info = GESTUREINFO {};
    info.cbSize = sizeof(info);

    if (!GetGestureInfo(handle, &info) || info.dwID != GID_ZOOM)
        return false;

    const auto distance =
        static_cast<double>(static_cast<DWORD>(info.ullArguments));
    const auto began = (info.dwFlags & GF_BEGIN) != 0;

    MouseEvent event;
    event.type = MouseEventType::Magnify;
    event.button = MouseButton::Other;
    event.modifiers = getModifiers();
    event.pos = fromScreenPixels(POINT {info.ptsLocation.x, info.ptsLocation.y});

    if (began)
    {
        event.scrollPhase = ScrollPhase::Began;
    }
    else
    {
        event.scrollPhase = (info.dwFlags & GF_END) != 0 ? ScrollPhase::Ended
                                                         : ScrollPhase::Changed;
        event.magnifyScale =
            magnifyScaleBetweenDistances(zoomGestureDistance, distance);
    }

    zoomGestureDistance = distance;

    dispatchMouseToContentView(event);
    CloseGestureInfoHandle(handle);
    return true;
}

std::optional<LRESULT> CompositionHostWindow::handleCommonMessage(UINT msg,
                                                                  WPARAM wParam,
                                                                  LPARAM lParam)
{
    ensureRawMouseRegistered();

    switch (msg)
    {
        // Raw Input must reach DefWindowProc to be cleaned up, so this reports
        // the movement and then stands aside.
        case WM_INPUT:
            accumulateRawMouseMovement(lParam);
            return std::nullopt;

        // A minimized window reports a 0x0 client area, which would collapse
        // the view tree — and any native surface sized from it — only to
        // restore it a moment later. Treat it as a visibility change instead.
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED)
            {
                hostMinimized = true;
                notifyViewTreeOfHostVisibility(contentView, false);
                return 0;
            }

            if (hostMinimized)
            {
                hostMinimized = false;
                notifyViewTreeOfHostVisibility(contentView,
                                               IsWindowVisible(hwnd) != FALSE);
            }

            resizeContentViewToClient();
            InvalidateRect(hwnd, nullptr, FALSE);

            if (mouseLockEngaged)
                clipCursorToClient();
            return 0;

        // Both things that track the window in screen coordinates: a view's
        // native surface, and the clip rectangle confining a pinned cursor.
        case WM_MOVE:
            notifyViewTreeOfHostMove(contentView);

            if (mouseLockEngaged)
                clipCursorToClient();
            return std::nullopt;

        case WM_SHOWWINDOW:
            notifyViewTreeOfHostVisibility(contentView, wParam != FALSE);
            return std::nullopt;

        case WM_SETFOCUS:
            if (mouseLockIntent)
                engageMouseLock();
            return std::nullopt;

        case WM_KILLFOCUS:
            disengageMouseLock();
            // The matching key-ups go to whoever took focus (e.g. Alt+Tab), so
            // tracked state would otherwise report keys stuck down forever.
            keyState.reset();
            return std::nullopt;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && reinterpret_cast<HWND>(wParam) == hwnd
                && applyCursorUnderPointer())
                return TRUE;
            return std::nullopt;

        case WM_ERASEBKGND:
            fillWindowBackground(reinterpret_cast<HDC>(wParam));
            return 1;

        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            paintDirtyViewsForHost(hwnd);
            if (contentView)
                ensureAllLayersRendered(contentView);
            return 0;

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            if (contentView)
            {
                // Capture the mouse so a drag keeps delivering moves even when
                // the cursor leaves the client area (matching NSView tracking).
                SetCapture(hwnd);

                auto event = makeMouseEvent(
                    lParam, getDpiScale(), MouseEventType::Down, getModifiers());
                event.button = buttonForDown(msg);
                mouseButtonHeld = true;
                heldMouseButton = event.button;
                dispatchMouseToContentView(event);
            }
            return 0;

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            // Clear before ReleaseCapture: it sends WM_CAPTURECHANGED, which
            // must not synthesize a second Up for this gesture.
            mouseButtonHeld = false;

            if (contentView)
            {
                auto event = makeMouseEvent(
                    lParam, getDpiScale(), MouseEventType::Up, getModifiers());
                event.button = buttonForUp(msg);
                dispatchMouseToContentView(event);
            }

            // Release the capture once no buttons remain held.
            if ((wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) == 0)
                ReleaseCapture();
            return 0;

        case WM_CAPTURECHANGED:
            // Capture stolen mid-drag (OS move loop, drag-drop, a popup): the
            // matching WM_*BUTTONUP will never arrive here, so synthesize the
            // Up to unwind the captured view's drag state.
            synthesizeMouseUpOnCaptureLoss();
            return 0;

        case WM_MOUSEMOVE:
        {
            if (mouseLockEngaged)
            {
                handleLockedMouseMove(lParam);
                return 0;
            }

            // Arm a WM_MOUSELEAVE so a cursor leaving the surface produces an
            // Exited event, clearing any hover (e.g. a WebView's :hover state).
            ensureMouseLeaveTracking();

            if (contentView)
            {
                auto buttons = wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON);

                // A move with a button held is a drag. dispatchMouseEvent only
                // forwards Dragged/Up to the captured mouseDownTarget; a plain
                // Moved is re-hit-tested, so without this the title-bar grab is
                // lost the instant the cursor moves and panels never drag.
                auto event = makeMouseEvent(lParam,
                                            getDpiScale(),
                                            buttons != 0 ? MouseEventType::Dragged
                                                         : MouseEventType::Moved,
                                            getModifiers());
                if (buttons != 0)
                    event.button = (wParam & MK_LBUTTON)   ? MouseButton::Left
                                   : (wParam & MK_RBUTTON) ? MouseButton::Right
                                                           : MouseButton::Middle;
                dispatchMouseToContentView(event);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            trackingMouseLeave = false;
            if (contentView)
            {
                MouseEvent event;
                event.type = MouseEventType::Exited;
                event.modifiers = getModifiers();
                dispatchMouseToContentView(event);
            }
            return 0;

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (contentView)
            {
                // Wheel messages carry screen coordinates (unlike the button and
                // move messages), so map them into the client area before
                // hit-testing the view under the cursor.
                POINT pt = {getXFromLParam(lParam), getYFromLParam(lParam)};
                ScreenToClient(hwnd, &pt);

                auto scale = getDpiScale();
                MouseEvent event;
                event.pos = {static_cast<float>(pt.x) / scale,
                             static_cast<float>(pt.y) / scale};
                event.type = MouseEventType::Wheel;
                event.modifiers = getModifiers();

                // In lines, as the other platforms report a notched wheel: a
                // detent is WHEEL_DELTA, and a precision touchpad sends
                // fractions of one.
                auto lines = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam))
                             / static_cast<float>(WHEEL_DELTA);
                event.delta =
                    (msg == WM_MOUSEWHEEL) ? Point {0.f, lines} : Point {lines, 0.f};
                dispatchMouseToContentView(event);
            }
            return 0;

        case WM_GESTURE:
            if (contentView && dispatchZoomGesture(lParam))
                return 0;
            return std::nullopt;

        case WM_KEYDOWN:
        case WM_KEYUP:
            dispatchKeyEvent(msg, wParam, lParam);
            return 0;

        // Alt and Alt+key arrive as sys-key messages. Track and dispatch them
        // like plain keys, but fall through to DefWindowProc so the system
        // chords (Alt+F4, Alt+Space, ...) keep working.
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            dispatchKeyEvent(msg, wParam, lParam);
            return std::nullopt;
    }

    return std::nullopt;
}

void CompositionHostWindow::dispatchKeyEvent(UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    auto vk = static_cast<uint16_t>(wParam);

    if (vk < 256)
        keyState.set(vk, down);

    if (!contentView)
        return;

    KeyEvent event;
    event.keyCode = keyCodeFromKeyMessage(vk, lParam);
    event.type = down ? KeyEventType::Down : KeyEventType::Up;
    event.modifiers = getModifiers();

    if (down)
    {
        event.characters = takePendingCharacters();
        event.isRepeat = (lParam & 0x40000000) != 0;
    }

    contentView->dispatchKeyEvent(event);

    ensureAllLayersRendered(contentView);
}

// The pump's TranslateMessage has already posted any WM_CHAR / WM_SYSCHAR for
// the key being dispatched; pull them off the queue so the KeyEvent carries the
// layout-aware text — the equivalent of NSEvent.characters on macOS, and what
// makes text entry work on non-US layouts (including dead keys).
std::string CompositionHostWindow::takePendingCharacters() const
{
    auto wide = std::wstring {};
    auto charMsg = MSG {};

    while (PeekMessageW(&charMsg, hwnd, WM_CHAR, WM_CHAR, PM_REMOVE)
           || PeekMessageW(&charMsg, hwnd, WM_SYSCHAR, WM_SYSCHAR, PM_REMOVE))
        wide.push_back(static_cast<wchar_t>(charMsg.wParam));

    return Strings::narrow(wide);
}

void CompositionHostWindow::synthesizeMouseUpOnCaptureLoss()
{
    if (!mouseButtonHeld)
        return;

    mouseButtonHeld = false;

    if (!contentView)
        return;

    auto pt = POINT {};
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);

    auto scale = getDpiScale();
    MouseEvent event;
    event.pos = {static_cast<float>(pt.x) / scale, static_cast<float>(pt.y) / scale};
    event.type = MouseEventType::Up;
    event.button = heldMouseButton;
    event.modifiers = getModifiers();
    dispatchMouseToContentView(event);
}

} // namespace eacp::Graphics
