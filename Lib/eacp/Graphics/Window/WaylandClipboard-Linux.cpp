#include "WaylandClipboard-Linux.h"

#include "WaylandInput-Linux.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace eacp::Graphics
{
namespace
{
// The mime the whole desktop agrees on, then the two older spellings still
// offered by GTK and Qt; the first match wins in both directions.
const auto waylandTextMimes =
    Vector<std::string> {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING"};

const auto waylandUriListMimes = Vector<std::string> {"text/uri-list"};

// A paste blocks the message thread, so it cannot block for long: a source
// that never writes must not hang the application.
constexpr auto waylandClipboardTimeout = Time::MS {2000};

// A source that goes away mid-write closes the pipe, and the default SIGPIPE
// would take the process with it. Process-Posix.cpp does the same.
void waylandIgnoreBrokenPipes()
{
    static auto once = std::once_flag {};

    std::call_once(once, [] { std::signal(SIGPIPE, SIG_IGN); });
}

int waylandMillisecondsLeft(Time::Deadline deadline)
{
    const auto left = deadline.remaining().count;

    return (int) std::clamp<int64_t>(left, 0, 1000);
}

// Non-blocking throughout: the receiver may read slowly, and a full pipe must
// not stop the message thread this runs on.
void waylandWriteAll(int fd, const std::string& data)
{
    waylandIgnoreBrokenPipes();

    auto flags = ::fcntl(fd, F_GETFL, 0);

    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    auto deadline = Time::Deadline {waylandClipboardTimeout};
    auto sent = size_t {0};

    while (sent < data.size() && !deadline.expired())
    {
        const auto written = ::write(fd, data.data() + sent, data.size() - sent);

        if (written > 0)
        {
            sent += (size_t) written;
            continue;
        }

        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            auto waiting = pollfd {fd, POLLOUT, 0};

            if (::poll(&waiting, 1, waylandMillisecondsLeft(deadline)) <= 0)
                continue;

            if ((waiting.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                return;

            continue;
        }

        if (written < 0 && errno == EINTR)
            continue;

        // EPIPE: the reader gave up, which is its right.
        return;
    }
}

// RFC 8089 with RFC 3986 escaping: a path with a space in it is not a URI.
std::string waylandFileUri(const std::string& path)
{
    auto uri = std::string {"file://"};

    for (auto character: path)
    {
        const auto byte = (unsigned char) character;
        const auto unreserved =
            (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
            || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_'
            || byte == '.' || byte == '~' || byte == '/';

        if (unreserved)
        {
            uri += (char) byte;
            continue;
        }

        char escaped[4] = {};
        std::snprintf(escaped, sizeof(escaped), "%%%02X", byte);
        uri += escaped;
    }

    return uri;
}
} // namespace

struct WaylandDataOffer
{
    wl_data_offer* offer = nullptr;
    Vector<std::string> mimes;
};

struct WaylandClipboardDispatch
{
    static WaylandClipboard& self(void* data)
    {
        return *static_cast<WaylandClipboard*>(data);
    }

    static void offerMime(void* data, wl_data_offer*, const char* mime)
    {
        if (mime != nullptr)
            static_cast<WaylandDataOffer*>(data)->mimes.add(mime);
    }

    static void offerSourceActions(void*, wl_data_offer*, uint32_t) {}
    static void offerAction(void*, wl_data_offer*, uint32_t) {}

    static void dataOffer(void* data, wl_data_device*, wl_data_offer* offer)
    {
        self(data).offerCreated(offer);
    }

    // Drag and drop is not implemented: the offer is refused and dropped, so
    // nothing here accumulates while the user drags across the window.
    static void dragEntered(void* data,
                            wl_data_device*,
                            uint32_t serial,
                            wl_surface*,
                            wl_fixed_t,
                            wl_fixed_t,
                            wl_data_offer* offer)
    {
        if (offer != nullptr)
            wl_data_offer_accept(offer, serial, nullptr);

        self(data).offerRefused(offer);
    }

    static void dragLeft(void*, wl_data_device*) {}

    static void dragMoved(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t)
    {
    }

    static void dragDropped(void*, wl_data_device*) {}

    static void selection(void* data, wl_data_device*, wl_data_offer* offer)
    {
        self(data).selectionChanged(offer);
    }

    static void sourceTarget(void*, wl_data_source*, const char*) {}

    static void sourceSend(void* data, wl_data_source*, const char*, int32_t fd)
    {
        self(data).sendSelection(fd);
    }

    static void sourceCancelled(void* data, wl_data_source* cancelled)
    {
        self(data).sourceCancelled(cancelled);
    }

    static void sourceDragFinished(void*, wl_data_source*) {}
    static void sourceDragPerformed(void*, wl_data_source*) {}
    static void sourceDragAction(void*, wl_data_source*, uint32_t) {}

    static const wl_data_offer_listener offerListener;
    static const wl_data_device_listener deviceListener;
    static const wl_data_source_listener sourceListener;
};

const wl_data_offer_listener WaylandClipboardDispatch::offerListener {
    .offer = WaylandClipboardDispatch::offerMime,
    .source_actions = WaylandClipboardDispatch::offerSourceActions,
    .action = WaylandClipboardDispatch::offerAction,
};

const wl_data_device_listener WaylandClipboardDispatch::deviceListener {
    .data_offer = WaylandClipboardDispatch::dataOffer,
    .enter = WaylandClipboardDispatch::dragEntered,
    .leave = WaylandClipboardDispatch::dragLeft,
    .motion = WaylandClipboardDispatch::dragMoved,
    .drop = WaylandClipboardDispatch::dragDropped,
    .selection = WaylandClipboardDispatch::selection,
};

const wl_data_source_listener WaylandClipboardDispatch::sourceListener {
    .target = WaylandClipboardDispatch::sourceTarget,
    .send = WaylandClipboardDispatch::sourceSend,
    .cancelled = WaylandClipboardDispatch::sourceCancelled,
    .dnd_drop_performed = WaylandClipboardDispatch::sourceDragPerformed,
    .dnd_finished = WaylandClipboardDispatch::sourceDragFinished,
    .action = WaylandClipboardDispatch::sourceDragAction,
};

WaylandClipboard::WaylandClipboard(WaylandDisplay& displayToUse)
    : display(displayToUse)
{
    waylandIgnoreBrokenPipes();

    auto backend = Clipboard::Backend {};

    backend.copyText = [this](std::string_view text) { return copyText(text); };
    backend.copyFiles = [this](const Vector<std::string>& paths)
    { return copyFiles(paths); };
    backend.getText = [this] { return getText(); };
    backend.hasText = [this] { return hasText(); };

    Clipboard::setBackend(std::move(backend));

    // Eagerly, before anything else on this connection asks for one.
    ensureDevice();
}

WaylandClipboard::~WaylandClipboard()
{
    Clipboard::clearBackend();

    destroySource();
    destroyOffers();

    if (device != nullptr)
    {
        if (wl_data_device_get_version(device)
            >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
            wl_data_device_release(device);
        else
            wl_data_device_destroy(device);

        device = nullptr;
    }
}

bool WaylandClipboard::ensureDevice()
{
    if (device != nullptr)
        return true;

    auto* manager = display.getDataDeviceManager();
    auto* seat = display.getSeat();

    if (manager == nullptr || seat == nullptr)
        return false;

    device = wl_data_device_manager_get_data_device(manager, seat);

    if (device == nullptr)
        return false;

    wl_data_device_add_listener(
        device, &WaylandClipboardDispatch::deviceListener, this);

    display.roundtrip();

    return true;
}

void WaylandClipboard::offerCreated(wl_data_offer* offer)
{
    if (offer == nullptr)
        return;

    auto record = std::make_unique<WaylandDataOffer>();
    record->offer = offer;

    wl_data_offer_add_listener(
        offer, &WaylandClipboardDispatch::offerListener, record.get());

    pendingOffers.add(std::move(record));
}

void WaylandClipboard::offerRefused(wl_data_offer* offer)
{
    if (offer == nullptr)
        return;

    pendingOffers.removeIndexesMatching(
        [offer](const std::unique_ptr<WaylandDataOffer>& record)
        { return record == nullptr || record->offer == offer; });

    wl_data_offer_destroy(offer);
}

// The compositor sends this to whoever has keyboard focus every time the
// selection changes - including when this process is the one that changed it,
// which is what makes a self-paste work.
void WaylandClipboard::selectionChanged(wl_data_offer* offer)
{
    auto claimed = std::unique_ptr<WaylandDataOffer> {};

    for (auto& record: pendingOffers)
        if (record != nullptr && record->offer == offer)
            claimed = std::move(record);

    // Anything left was announced and never claimed: a drag, or an offer the
    // compositor replaced before this one arrived.
    destroyOffers();

    selection = std::move(claimed);
}

void WaylandClipboard::destroyOffers()
{
    for (auto& record: pendingOffers)
        if (record != nullptr && record->offer != nullptr)
            wl_data_offer_destroy(record->offer);

    pendingOffers.clear();

    if (selection != nullptr && selection->offer != nullptr)
        wl_data_offer_destroy(selection->offer);

    selection.reset();
}

void WaylandClipboard::destroySource()
{
    if (source != nullptr)
    {
        wl_data_source_destroy(source);
        source = nullptr;
    }

    sourceData.reset();
}

// On a thread of its own, because the receiver may be this very process,
// blocked inside getText on the message thread: a pipe whose two ends are the
// same thread fills up and stops, and neither side ever moves again.
void WaylandClipboard::sendSelection(int fd)
{
    auto payload = sourceData;

    std::thread(
        [payload, fd]
        {
            if (payload != nullptr)
                waylandWriteAll(fd, *payload);

            ::close(fd);
        })
        .detach();
}

// Something else took the selection; this source will never be asked again.
void WaylandClipboard::sourceCancelled(wl_data_source* cancelled)
{
    if (cancelled != source)
    {
        wl_data_source_destroy(cancelled);
        return;
    }

    destroySource();
}

bool WaylandClipboard::offerSelection(std::string data,
                                      const Vector<std::string>& mimes)
{
    if (!ensureDevice())
        return false;

    auto* input = display.getInput();

    // set_selection needs the serial of an input event on a surface of ours,
    // so a window with no keyboard focus cannot take the clipboard.
    const auto serial = input != nullptr ? input->getSelectionSerial() : 0;

    if (serial == 0)
        return false;

    destroySource();

    source =
        wl_data_device_manager_create_data_source(display.getDataDeviceManager());

    if (source == nullptr)
        return false;

    sourceData = std::make_shared<const std::string>(std::move(data));

    wl_data_source_add_listener(
        source, &WaylandClipboardDispatch::sourceListener, this);

    for (const auto& mime: mimes)
        wl_data_source_offer(source, mime.c_str());

    wl_data_device_set_selection(device, source, serial);
    display.flush();

    return true;
}

bool WaylandClipboard::copyText(std::string_view text)
{
    return offerSelection(std::string {text}, waylandTextMimes);
}

bool WaylandClipboard::copyFiles(const Vector<std::string>& paths)
{
    if (paths.empty())
        return false;

    auto list = std::string {};

    // CRLF, as text/uri-list is specified.
    for (const auto& path: paths)
        list += waylandFileUri(path) + "\r\n";

    return offerSelection(std::move(list), waylandUriListMimes);
}

std::string WaylandClipboard::getText()
{
    return receiveSelection(waylandTextMimes);
}

const std::string* WaylandClipboard::pickMime(const Vector<std::string>& mimes) const
{
    if (selection == nullptr)
        return nullptr;

    for (const auto& mime: mimes)
        if (selection->mimes.contains(mime))
            return &mime;

    return nullptr;
}

bool WaylandClipboard::hasText()
{
    if (!ensureDevice())
        return false;

    // The selection may have changed under us since the last dispatch.
    display.roundtrip();

    return pickMime(waylandTextMimes) != nullptr;
}

// Blocks the message thread while the connection is pumped by hand: the source
// may be this very process, and its send event has to be dispatched from here
// or the pipe would never be written and the read would time out.
std::string WaylandClipboard::receiveSelection(const Vector<std::string>& mimes)
{
    if (!ensureDevice())
        return {};

    display.roundtrip();

    if (selection == nullptr || selection->offer == nullptr)
        return {};

    const auto* wanted = pickMime(mimes);

    if (wanted == nullptr)
        return {};

    int ends[2] = {-1, -1};

    if (::pipe2(ends, O_CLOEXEC) != 0)
        return {};

    // Only this end: the writer is another client, and a non-blocking fd is
    // not what it expects.
    if (auto flags = ::fcntl(ends[0], F_GETFL, 0); flags >= 0)
        ::fcntl(ends[0], F_SETFL, flags | O_NONBLOCK);

    wl_data_offer_receive(selection->offer, wanted->c_str(), ends[1]);
    ::close(ends[1]);

    display.flush();

    auto* connection = display.getDisplay();
    auto deadline = Time::Deadline {waylandClipboardTimeout};
    auto text = std::string {};
    auto finished = false;
    char buffer[4096];

    while (!finished && !deadline.expired())
    {
        while (wl_display_prepare_read(connection) != 0)
            if (wl_display_dispatch_pending(connection) < 0)
            {
                finished = true;
                break;
            }

        if (finished)
            break;

        wl_display_flush(connection);

        pollfd watched[2] = {
            {ends[0], POLLIN, 0},
            {wl_display_get_fd(connection), POLLIN, 0},
        };

        const auto ready = ::poll(watched, 2, waylandMillisecondsLeft(deadline));

        if ((watched[1].revents & POLLIN) != 0)
            wl_display_read_events(connection);
        else
            wl_display_cancel_read(connection);

        wl_display_dispatch_pending(connection);

        if (ready < 0 && errno != EINTR)
            break;

        if ((watched[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
        {
            if (ready == 0)
                break;

            continue;
        }

        auto draining = true;

        while (draining)
        {
            const auto received = ::read(ends[0], buffer, sizeof(buffer));

            if (received > 0)
            {
                text.append(buffer, (size_t) received);
                continue;
            }

            // Zero is the source closing its end, which is the whole payload;
            // anything else is EAGAIN, so the rest comes on a later turn.
            finished = received == 0;
            draining = received < 0 && errno == EINTR;
        }
    }

    ::close(ends[0]);

    return text;
}
} // namespace eacp::Graphics
