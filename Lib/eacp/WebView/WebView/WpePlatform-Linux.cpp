#include "WpePlatform-Linux.h"

#include <eacp/Core/Core.h>

#include <wpe/wpe-platform.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace eacp::Graphics::detail
{
namespace
{
// G_DEFINE_TYPE's fixed names would collide when two copies of eacp share a
// process (a host and a plugin, two plugins): the second registration fails
// and its type is 0. Each copy names its types after an address of its own.
GType registerType(GType parent,
                   const char* baseName,
                   guint classSize,
                   GClassInitFunc classInit,
                   guint instanceSize,
                   GInstanceInitFunc instanceInit)
{
    static const auto copyAnchor = 0;
    auto name = std::string {baseName} + "_"
                + std::to_string(reinterpret_cast<std::uintptr_t>(&copyAnchor));

    return g_type_register_static_simple(parent,
                                         g_intern_string(name.c_str()),
                                         classSize,
                                         classInit,
                                         instanceSize,
                                         instanceInit,
                                         GTypeFlags {});
}

struct EacpWpeViewState
{
    WpeFrameSink sink = [](const WpeFrameBytes&) {};
    WPEBuffer* pendingBuffer = nullptr;
    WPEBuffer* committedBuffer = nullptr;
    GSource* frameSource = nullptr;
    gint64 lastFrameTime = 0;
    bool presenting = false;
};

// How long a presented view's buffer may wait for its present before it is
// reported rendered anyway: a swapchain that never came up, or a surface the
// compositor stopped answering, must not stall WebKit for good.
constexpr auto presentFallback = gint64 {G_USEC_PER_SEC / 4};

struct EacpWpeView
{
    WPEView parent;
    EacpWpeViewState* state;
};

struct EacpWpeToplevel
{
    WPEToplevel parent;
};

struct EacpWpeDisplay
{
    WPEDisplay parent;
};

gpointer viewParentClass = nullptr;

GType eacpWpeViewType();

EacpWpeViewState* viewStateOf(WPEView* view)
{
    if (view == nullptr || !G_TYPE_CHECK_INSTANCE_TYPE(view, eacpWpeViewType()))
        return nullptr;

    return reinterpret_cast<EacpWpeView*>(view)->state;
}

void replaceBuffer(WPEBuffer*& slot, WPEBuffer* buffer)
{
    if (buffer != nullptr)
        g_object_ref(buffer);

    if (slot != nullptr)
        g_object_unref(slot);

    slot = buffer;
}

// The frame clock WPE's own headless view keeps: a buffer is reported
// rendered at most once a 60th of a second after the last, which is what
// paces WebKit's next frame.
gboolean presentPendingBuffer(gpointer userData)
{
    auto* view = WPE_VIEW(userData);
    auto* state = viewStateOf(view);

    if (state == nullptr)
        return G_SOURCE_REMOVE;

    if (state->pendingBuffer == nullptr)
        return G_SOURCE_CONTINUE;

    if (state->committedBuffer != nullptr)
        wpe_view_buffer_released(view, state->committedBuffer);

    if (state->committedBuffer != nullptr)
        g_object_unref(state->committedBuffer);

    state->committedBuffer = std::exchange(state->pendingBuffer, nullptr);

    if (state->committedBuffer != nullptr)
        wpe_view_buffer_rendered(view, state->committedBuffer);

    return G_SOURCE_CONTINUE;
}

GSourceFuncs frameSourceFuncs = {
    nullptr,
    nullptr,
    [](GSource* source, GSourceFunc callback, gpointer userData) -> gboolean
    {
        if (g_source_get_ready_time(source) == -1)
            return G_SOURCE_CONTINUE;

        g_source_set_ready_time(source, -1);
        return callback(userData);
    },
    nullptr,
    nullptr,
    nullptr,
};

void deliverFrame(EacpWpeViewState& state, WPEBuffer* buffer)
{
    auto width = wpe_buffer_get_width(buffer);
    auto height = wpe_buffer_get_height(buffer);

    if (WPE_IS_BUFFER_SHM(buffer))
    {
        auto* shm = WPE_BUFFER_SHM(buffer);
        auto size = gsize {};
        const auto* bytes = static_cast<const std::uint8_t*>(
            g_bytes_get_data(wpe_buffer_shm_get_data(shm), &size));

        state.sink({bytes, width, height, (int) wpe_buffer_shm_get_stride(shm)});
        return;
    }

    GError* error = nullptr;
    auto* pixels = wpe_buffer_import_to_pixels(buffer, &error);

    if (pixels == nullptr)
    {
        if (error != nullptr)
            g_error_free(error);

        return;
    }

    auto size = gsize {};
    const auto* bytes =
        static_cast<const std::uint8_t*>(g_bytes_get_data(pixels, &size));

    if (size >= (gsize) width * (gsize) height * 4)
        state.sink({bytes, width, height, width * 4});
}

gboolean viewRenderBuffer(
    WPEView* view, WPEBuffer* buffer, const WPERectangle*, guint, GError**)
{
    auto* state = viewStateOf(view);

    if (state == nullptr)
        return FALSE;

    // Pending before the sink sees it, so a sink that presents at once can
    // report it rendered from inside the call.
    replaceBuffer(state->pendingBuffer, buffer);
    deliverFrame(*state, buffer);

    auto now = g_get_monotonic_time();

    if (state->lastFrameTime == 0)
        state->lastFrameTime = now;

    auto next = state->lastFrameTime + G_USEC_PER_SEC / 60;
    state->lastFrameTime = now;

    if (state->pendingBuffer == nullptr)
        return TRUE;

    if (state->presenting)
        next = now + presentFallback;

    g_source_set_ready_time(state->frameSource, next <= now ? 0 : next);
    return TRUE;
}

void viewConstructed(GObject* object)
{
    G_OBJECT_CLASS(viewParentClass)->constructed(object);

    auto* state = reinterpret_cast<EacpWpeView*>(object)->state;

    state->frameSource = g_source_new(&frameSourceFuncs, sizeof(GSource));
    g_source_set_name(state->frameSource, "eacp WPE frame clock");
    g_source_set_callback(state->frameSource, presentPendingBuffer, object, nullptr);
    g_source_attach(state->frameSource, g_main_context_get_thread_default());
    g_source_set_ready_time(state->frameSource, -1);
}

void viewDispose(GObject* object)
{
    auto* state = reinterpret_cast<EacpWpeView*>(object)->state;

    if (state->frameSource != nullptr)
    {
        g_source_destroy(state->frameSource);
        g_source_unref(state->frameSource);
        state->frameSource = nullptr;
    }

    replaceBuffer(state->pendingBuffer, nullptr);
    replaceBuffer(state->committedBuffer, nullptr);

    G_OBJECT_CLASS(viewParentClass)->dispose(object);
}

void viewFinalize(GObject* object)
{
    delete std::exchange(reinterpret_cast<EacpWpeView*>(object)->state, nullptr);

    G_OBJECT_CLASS(viewParentClass)->finalize(object);
}

void viewClassInit(gpointer klass, gpointer)
{
    viewParentClass = g_type_class_peek_parent(klass);

    auto* objectClass = G_OBJECT_CLASS(klass);
    objectClass->constructed = viewConstructed;
    objectClass->dispose = viewDispose;
    objectClass->finalize = viewFinalize;

    WPE_VIEW_CLASS(klass)->render_buffer = viewRenderBuffer;
}

void viewInit(GTypeInstance* instance, gpointer)
{
    reinterpret_cast<EacpWpeView*>(instance)->state = new EacpWpeViewState {};
}

GType eacpWpeViewType()
{
    static const auto type = registerType(WPE_TYPE_VIEW,
                                          "EacpWpeView",
                                          sizeof(WPEViewClass),
                                          viewClassInit,
                                          sizeof(EacpWpeView),
                                          viewInit);
    return type;
}

gpointer toplevelParentClass = nullptr;

void resizeEachView(WPEToplevel* toplevel)
{
    wpe_toplevel_foreach_view(
        toplevel,
        [](WPEToplevel* owner, WPEView* view, gpointer) -> gboolean
        {
            auto width = 0;
            auto height = 0;
            wpe_toplevel_get_size(owner, &width, &height);
            wpe_view_resized(view, width, height);
            return FALSE;
        },
        nullptr);
}

gboolean toplevelResize(WPEToplevel* toplevel, int width, int height)
{
    wpe_toplevel_resized(toplevel, width, height);
    resizeEachView(toplevel);
    return TRUE;
}

void toplevelConstructed(GObject* object)
{
    G_OBJECT_CLASS(toplevelParentClass)->constructed(object);
    wpe_toplevel_state_changed(WPE_TOPLEVEL(object), WPE_TOPLEVEL_STATE_ACTIVE);
}

void toplevelClassInit(gpointer klass, gpointer)
{
    toplevelParentClass = g_type_class_peek_parent(klass);
    G_OBJECT_CLASS(klass)->constructed = toplevelConstructed;
    WPE_TOPLEVEL_CLASS(klass)->resize = toplevelResize;
}

GType eacpWpeToplevelType()
{
    static const auto type = registerType(WPE_TYPE_TOPLEVEL,
                                          "EacpWpeToplevel",
                                          sizeof(WPEToplevelClass),
                                          toplevelClassInit,
                                          sizeof(EacpWpeToplevel),
                                          nullptr);
    return type;
}

gboolean displayConnect(WPEDisplay*, GError**)
{
    return TRUE;
}

WPEView* displayCreateView(WPEDisplay* display)
{
    return WPE_VIEW(g_object_new(eacpWpeViewType(), "display", display, nullptr));
}

WPEToplevel* displayCreateToplevel(WPEDisplay* display, guint maxViews)
{
    return WPE_TOPLEVEL(g_object_new(
        eacpWpeToplevelType(), "display", display, "max-views", maxViews, nullptr));
}

WPEKeymap* displayGetKeymap(WPEDisplay* display)
{
    static auto* keymap = wpe_keymap_xkb_new();
    (void) display;
    return keymap;
}

void displayClassInit(gpointer klass, gpointer)
{
    auto* displayClass = WPE_DISPLAY_CLASS(klass);
    displayClass->connect = displayConnect;
    displayClass->create_view = displayCreateView;
    displayClass->create_toplevel = displayCreateToplevel;
    displayClass->get_keymap = displayGetKeymap;
}

GType eacpWpeDisplayType()
{
    static const auto type = registerType(WPE_TYPE_DISPLAY,
                                          "EacpWpeDisplay",
                                          sizeof(WPEDisplayClass),
                                          displayClassInit,
                                          sizeof(EacpWpeDisplay),
                                          nullptr);
    return type;
}

WPEDisplay* connectDisplay()
{
    auto* display = WPE_DISPLAY(g_object_new(eacpWpeDisplayType(), nullptr));
    GError* error = nullptr;

    if (wpe_display_connect(display, &error))
        return display;

    LOG("WebView: the WPE display would not connect: ",
        error != nullptr ? error->message : "no reason given");

    if (error != nullptr)
        g_error_free(error);

    g_object_unref(display);
    return nullptr;
}
} // namespace

WPEDisplay* wpeDisplay()
{
    static auto* display = connectDisplay();
    return display;
}

WpeViewHost::WpeViewHost(WPEView* viewToHost)
    : view(viewToHost)
{
    if (view == nullptr)
        return;

    toplevel = wpe_view_get_toplevel(view);

    if (toplevel != nullptr)
    {
        g_object_ref(toplevel);
    }
    else
    {
        toplevel = wpe_display_create_toplevel(wpe_view_get_display(view), 1);
        wpe_view_set_toplevel(view, toplevel);
    }

    wpe_toplevel_state_changed(toplevel, WPE_TOPLEVEL_STATE_ACTIVE);
    wpe_view_set_visible(view, TRUE);
    wpe_view_map(view);
}

WpeViewHost::~WpeViewHost()
{
    if (auto* state = viewStateOf(view))
        state->sink = [](const WpeFrameBytes&) {};

    if (toplevel != nullptr)
        g_object_unref(toplevel);
}

void WpeViewHost::setFrameSink(WpeFrameSink sink)
{
    if (auto* state = viewStateOf(view))
        state->sink = std::move(sink);
}

void WpeViewHost::setPresenting(bool presenting)
{
    auto* state = viewStateOf(view);

    if (state == nullptr || state->presenting == presenting)
        return;

    state->presenting = presenting;

    if (!presenting && state->pendingBuffer != nullptr)
        g_source_set_ready_time(state->frameSource, 0);
}

void WpeViewHost::bufferPresented()
{
    auto* state = viewStateOf(view);

    if (state == nullptr || state->pendingBuffer == nullptr)
        return;

    g_source_set_ready_time(state->frameSource, -1);
    presentPendingBuffer(view);
}

void WpeViewHost::resize(float widthPoints, float heightPoints, float scale)
{
    auto width = (int) std::lround(widthPoints);
    auto height = (int) std::lround(heightPoints);

    if (view == nullptr || toplevel == nullptr || width <= 0 || height <= 0)
        return;

    if (scale > 0.f && wpe_toplevel_get_scale(toplevel) != (double) scale)
        wpe_toplevel_scale_changed(toplevel, scale);

    wpe_toplevel_resized(toplevel, width, height);
    wpe_view_resized(view, width, height);
}

void WpeViewHost::setVisible(bool visible)
{
    if (view == nullptr)
        return;

    wpe_view_set_visible(view, visible ? TRUE : FALSE);

    if (visible)
        wpe_view_map(view);
    else
        wpe_view_unmap(view);
}

void WpeViewHost::setFocused(bool focused)
{
    if (view == nullptr)
        return;

    if (focused)
        wpe_view_focus_in(view);
    else
        wpe_view_focus_out(view);
}
} // namespace eacp::Graphics::detail
