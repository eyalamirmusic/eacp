#include "Common.h"

#include <eacp/Core/App/Clipboard.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Graphics/Window/LinuxWindowSystem-Linux.h>

#include <xcb/xcb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

// The CLIPBOARD selection of the X11 backend, against a real server and
// against a real second client. Every case self-skips without a display, as
// the rest of the X11 suite does, and each one is a process of its own with
// both the display and the clipboard locks: a display has exactly one
// selection, and a case's copy must not land between another's copy and its
// read.
//
// The second client is an xcb connection of the test's own. Reading what eacp
// owns is done from the message thread with eacp's loop pumped between polls,
// because the answer comes from eacp; owning the selection for eacp to read is
// done on a thread, because Clipboard::getText blocks the message thread until
// the owner has answered and an owner on that same thread would never get to.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
constexpr auto x11ClipboardTestTimeout = Time::MS {5000};

// 64 KB a chunk, so a payload of a few hundred KB is a handful of them.
constexpr size_t x11ClipboardIncrChunk = 64u * 1024u;

// The clipboard is up as soon as the connection is, and the connection needs
// no window of ours: unlike Wayland, taking a selection here wants neither a
// toplevel nor keyboard focus. Asking the seat a question is what opens it.
bool x11ClipboardReachable()
{
    static const auto reachable = []
    {
        if (Apps::getAppEnvironment().headless)
            return false;

        if (getEnvValue("EACP_WINDOW_SYSTEM") != "x11")
            return false;

        if (getEnvValue("DISPLAY").empty())
            return false;

        return linuxSeat() != nullptr;
    }();

    return reachable;
}

// The machine's clipboard is shared with everything else running on it, so
// whatever was there goes back afterwards.
struct X11ClipboardGuard
{
    X11ClipboardGuard()
        : previous(Clipboard::getText())
    {
    }

    ~X11ClipboardGuard()
    {
        if (!previous.empty())
            Clipboard::copyText(previous);
    }

    std::string previous;
};

xcb_atom_t x11TestAtom(xcb_connection_t* connection, const char* name)
{
    auto* reply = xcb_intern_atom_reply(
        connection,
        xcb_intern_atom(connection, 0, (uint16_t) std::strlen(name), name),
        nullptr);

    if (reply == nullptr)
        return XCB_ATOM_NONE;

    const auto atom = reply->atom;
    std::free(reply);

    return atom;
}

// One never-mapped window and the atoms both halves of a selection need.
struct X11TestClient
{
    X11TestClient()
        : connection(xcb_connect(nullptr, nullptr))
    {
        if (xcb_connection_has_error(connection) != 0)
        {
            xcb_disconnect(connection);
            connection = nullptr;
            return;
        }

        auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
        const uint32_t values[] = {XCB_EVENT_MASK_PROPERTY_CHANGE};

        window = xcb_generate_id(connection);

        xcb_create_window(connection,
                          XCB_COPY_FROM_PARENT,
                          window,
                          screen->root,
                          -1,
                          -1,
                          1,
                          1,
                          0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          XCB_COPY_FROM_PARENT,
                          XCB_CW_EVENT_MASK,
                          values);

        clipboard = x11TestAtom(connection, "CLIPBOARD");
        targets = x11TestAtom(connection, "TARGETS");
        utf8 = x11TestAtom(connection, "UTF8_STRING");
        plainUtf8 = x11TestAtom(connection, "text/plain;charset=utf-8");
        uriList = x11TestAtom(connection, "text/uri-list");
        incr = x11TestAtom(connection, "INCR");
        transfer = x11TestAtom(connection, "EACP_TEST_SELECTION");
        nonsense = x11TestAtom(connection, "EACP_TEST_NOT_A_TARGET");

        xcb_flush(connection);
    }

    ~X11TestClient()
    {
        if (connection != nullptr)
            xcb_disconnect(connection);
    }

    X11TestClient(const X11TestClient&) = delete;
    X11TestClient& operator=(const X11TestClient&) = delete;

    bool isValid() const { return connection != nullptr; }

    xcb_window_t selectionOwner() const
    {
        auto* reply = xcb_get_selection_owner_reply(
            connection, xcb_get_selection_owner(connection, clipboard), nullptr);

        if (reply == nullptr)
            return XCB_NONE;

        const auto owner = reply->owner;
        std::free(reply);

        return owner;
    }

    xcb_connection_t* connection = nullptr;
    xcb_window_t window = XCB_NONE;

    xcb_atom_t clipboard = XCB_ATOM_NONE;
    xcb_atom_t targets = XCB_ATOM_NONE;
    xcb_atom_t utf8 = XCB_ATOM_NONE;
    xcb_atom_t plainUtf8 = XCB_ATOM_NONE;
    xcb_atom_t uriList = XCB_ATOM_NONE;
    xcb_atom_t incr = XCB_ATOM_NONE;
    xcb_atom_t transfer = XCB_ATOM_NONE;
    xcb_atom_t nonsense = XCB_ATOM_NONE;
};

// What a conversion came back with: `refused` is a SelectionNotify naming no
// property, which is how an owner says no.
struct X11TestConversion
{
    bool answered = false;
    bool refused = false;
    std::string data;
    xcb_atom_t type = XCB_ATOM_NONE;
};

// A second client asking eacp for the selection, on the message thread with
// eacp's loop pumped between polls: the answer it waits for is written by the
// very copy under test.
struct X11ForeignReader : X11TestClient
{
    X11TestConversion read(xcb_atom_t target)
    {
        auto conversion = X11TestConversion {};

        if (!isValid())
            return conversion;

        xcb_delete_property(connection, window, transfer);
        xcb_convert_selection(
            connection, window, clipboard, target, transfer, XCB_CURRENT_TIME);
        xcb_flush(connection);

        auto property = xcb_atom_t {XCB_ATOM_NONE};

        const auto answered = Threads::runEventLoopUntil(
            [&] { return pollForNotify(conversion, property); },
            x11ClipboardTestTimeout);

        if (!answered || conversion.refused)
            return conversion;

        auto* reply =
            xcb_get_property_reply(connection,
                                   xcb_get_property(connection,
                                                    1,
                                                    window,
                                                    property,
                                                    XCB_GET_PROPERTY_TYPE_ANY,
                                                    0,
                                                    1u << 20),
                                   nullptr);

        if (reply == nullptr)
            return conversion;

        conversion.type = reply->type;
        conversion.data.append((const char*) xcb_get_property_value(reply),
                               (size_t) xcb_get_property_value_length(reply));

        std::free(reply);

        return conversion;
    }

    Vector<xcb_atom_t> readTargets()
    {
        const auto conversion = read(targets);
        const auto count = conversion.data.size() / sizeof(xcb_atom_t);

        auto list = Vector<xcb_atom_t> {};
        auto atom = xcb_atom_t {};

        for (auto i = size_t {0}; i < count; ++i)
        {
            std::memcpy(&atom,
                        conversion.data.data() + i * sizeof(xcb_atom_t),
                        sizeof(atom));
            list.add(atom);
        }

        return list;
    }

private:
    bool pollForNotify(X11TestConversion& conversion, xcb_atom_t& property)
    {
        while (auto* event = xcb_poll_for_event(connection))
        {
            if ((event->response_type & ~0x80) == XCB_SELECTION_NOTIFY)
            {
                const auto& notify =
                    *reinterpret_cast<xcb_selection_notify_event_t*>(event);

                conversion.answered = true;
                conversion.refused = notify.property == XCB_ATOM_NONE;
                property = notify.property;
            }

            std::free(event);
        }

        return conversion.answered;
    }
};

// A client that owns the selection for eacp to read, on a thread of its own:
// a read blocks eacp's message thread until this answers.
class X11SelectionServer
{
public:
    enum class Mode
    {
        Normal,
        Refuse,
        Incremental
    };

    X11SelectionServer(std::string payloadToServe, Mode modeToUse)
        : payload(std::move(payloadToServe))
        , mode(modeToUse)
    {
        worker = std::thread([this] { serveUntilStopped(); });

        auto deadline = Time::Deadline {x11ClipboardTestTimeout};

        while (!owning && !failed && !deadline.expired())
            std::this_thread::sleep_for(std::chrono::milliseconds {2});
    }

    ~X11SelectionServer() { stop(); }

    X11SelectionServer(const X11SelectionServer&) = delete;
    X11SelectionServer& operator=(const X11SelectionServer&) = delete;

    bool isOwner() const { return owning; }

    // Joining disconnects, which is how a selection is left with no owner at
    // all: the server hands every id of a departed client back.
    void stop()
    {
        if (!worker.joinable())
            return;

        stopping = true;
        worker.join();
    }

private:
    void serveUntilStopped()
    {
        auto client = X11TestClient {};

        if (!client.isValid())
        {
            failed = true;
            return;
        }

        xcb_set_selection_owner(
            client.connection, client.window, client.clipboard, XCB_CURRENT_TIME);
        xcb_flush(client.connection);

        if (client.selectionOwner() != client.window)
        {
            failed = true;
            return;
        }

        owning = true;

        while (!stopping)
        {
            auto watched =
                pollfd {xcb_get_file_descriptor(client.connection), POLLIN, 0};

            ::poll(&watched, 1, 10);

            while (auto* event = xcb_poll_for_event(client.connection))
            {
                handleEvent(client, *event);
                std::free(event);
            }
        }

        owning = false;
    }

    void handleEvent(X11TestClient& client, const xcb_generic_event_t& event)
    {
        switch (event.response_type & ~0x80)
        {
            case XCB_SELECTION_REQUEST:
                serveRequest(
                    client,
                    *reinterpret_cast<const xcb_selection_request_event_t*>(&event));
                break;

            case XCB_PROPERTY_NOTIFY:
                continueTransfer(
                    client,
                    *reinterpret_cast<const xcb_property_notify_event_t*>(&event));
                break;

            default:
                break;
        }
    }

    void serveRequest(X11TestClient& client,
                      const xcb_selection_request_event_t& request)
    {
        const auto property =
            request.property != XCB_ATOM_NONE ? request.property : request.target;

        if (mode == Mode::Refuse)
        {
            answer(client, request, XCB_ATOM_NONE);
            return;
        }

        if (request.target == client.targets)
        {
            const xcb_atom_t list[] = {
                client.targets, client.utf8, client.plainUtf8};

            xcb_change_property(client.connection,
                                XCB_PROP_MODE_REPLACE,
                                request.requestor,
                                property,
                                XCB_ATOM_ATOM,
                                32,
                                3,
                                list);

            answer(client, request, property);
            return;
        }

        if (request.target != client.utf8 && request.target != client.plainUtf8)
        {
            answer(client, request, XCB_ATOM_NONE);
            return;
        }

        if (mode == Mode::Incremental)
        {
            startTransfer(client, request, property);
            return;
        }

        xcb_change_property(client.connection,
                            XCB_PROP_MODE_REPLACE,
                            request.requestor,
                            property,
                            request.target,
                            8,
                            (uint32_t) payload.size(),
                            payload.data());

        answer(client, request, property);
    }

    // ICCCM's INCR: the size first, then a property at a time, each written
    // when the requestor deletes the last one, and a zero-length one to end it.
    // One transfer per requestor: on a desktop the compositor's clipboard
    // bridge converts a fresh selection at the same time eacp does, and a
    // sender that kept one requestor's state would stall the other's read.
    void startTransfer(X11TestClient& client,
                       const xcb_selection_request_event_t& request,
                       xcb_atom_t property)
    {
        const uint32_t mask[] = {XCB_EVENT_MASK_PROPERTY_CHANGE};

        xcb_change_window_attributes(
            client.connection, request.requestor, XCB_CW_EVENT_MASK, mask);

        const auto total = (uint32_t) payload.size();

        xcb_change_property(client.connection,
                            XCB_PROP_MODE_REPLACE,
                            request.requestor,
                            property,
                            client.incr,
                            32,
                            1,
                            &total);

        std::erase_if(transfers,
                      [&](const Transfer& transfer)
                      { return transfer.window == request.requestor; });

        transfers.push_back({request.requestor, property, request.target});

        answer(client, request, property);
    }

    void continueTransfer(X11TestClient& client,
                          const xcb_property_notify_event_t& event)
    {
        if (event.state != XCB_PROPERTY_DELETE)
            return;

        auto transfer = std::find_if(transfers.begin(),
                                     transfers.end(),
                                     [&](const Transfer& candidate)
                                     {
                                         return candidate.window == event.window
                                                && candidate.property == event.atom;
                                     });

        if (transfer == transfers.end())
            return;

        if (transfer->finished)
        {
            transfers.erase(transfer);
            return;
        }

        const auto chunk =
            std::min(payload.size() - transfer->sent, x11ClipboardIncrChunk);

        xcb_change_property(client.connection,
                            XCB_PROP_MODE_REPLACE,
                            transfer->window,
                            transfer->property,
                            transfer->type,
                            8,
                            (uint32_t) chunk,
                            payload.data() + transfer->sent);

        transfer->sent += chunk;
        transfer->finished = chunk == 0;

        xcb_flush(client.connection);
    }

    void answer(X11TestClient& client,
                const xcb_selection_request_event_t& request,
                xcb_atom_t property)
    {
        alignas(xcb_selection_notify_event_t) char buffer[32] = {};

        auto& notify = *reinterpret_cast<xcb_selection_notify_event_t*>(buffer);

        notify.response_type = XCB_SELECTION_NOTIFY;
        notify.time = request.time;
        notify.requestor = request.requestor;
        notify.selection = request.selection;
        notify.target = request.target;
        notify.property = property;

        xcb_send_event(client.connection,
                       0,
                       request.requestor,
                       XCB_EVENT_MASK_NO_EVENT,
                       buffer);

        xcb_flush(client.connection);
    }

    std::string payload;
    Mode mode = Mode::Normal;

    std::thread worker;
    std::atomic<bool> stopping {false};
    std::atomic<bool> owning {false};
    std::atomic<bool> failed {false};

    struct Transfer
    {
        xcb_window_t window = XCB_NONE;
        xcb_atom_t property = XCB_ATOM_NONE;
        xcb_atom_t type = XCB_ATOM_NONE;
        size_t sent = 0;
        bool finished = false;
    };

    std::vector<Transfer> transfers;
};

std::string x11LongClipboardText()
{
    auto text = std::string {};

    while (text.size() < 256u * 1024u)
        text += "a line of clipboard text that is not especially short\n";

    return text;
}
} // namespace

// No window is opened anywhere in this case: owning a selection on X11 needs
// neither a toplevel nor keyboard focus, which is the whole reason a plugin
// copy prefers this backend.
auto tX11ClipboardRoundTrip = test("X11/clipboardTextRoundTripsWithinTheCopy") = []
{
    if (!x11ClipboardReachable())
        return;

    const auto guard = X11ClipboardGuard {};

    check(Clipboard::copyText("eacp x11 clipboard"));
    check(Clipboard::hasText());

    // Answered from the store rather than converted: a conversion of our own
    // selection would be served by the thread that is waiting for it.
    check(Clipboard::getText() == "eacp x11 clipboard");

    const auto unicode = std::string {"héllo → 世界"};

    check(Clipboard::copyText(unicode));
    check(Clipboard::getText() == unicode);

    // Reading must not consume the selection.
    check(Clipboard::getText() == unicode);
};

auto tX11ClipboardServesOthers = test("X11/clipboardServesAnotherClient") = []
{
    if (!x11ClipboardReachable())
        return;

    const auto guard = X11ClipboardGuard {};

    auto reader = X11ForeignReader {};

    if (!reader.isValid())
        return;

    check(Clipboard::copyText("served over the wire"));
    check(reader.selectionOwner() != XCB_NONE);

    const auto offered = reader.readTargets();

    check(offered.contains(reader.targets), "TARGETS did not list itself");
    check(offered.contains(reader.utf8), "UTF8_STRING was not offered");
    check(offered.contains(reader.plainUtf8));
    check(!offered.contains(reader.uriList), "text was offered as files");

    const auto text = reader.read(reader.utf8);

    check(text.answered && !text.refused);
    check(text.type == reader.utf8);
    check(text.data == "served over the wire");

    // Anything we do not hold is a SelectionNotify naming no property, which
    // is how a requestor is told to look elsewhere.
    const auto refused = reader.read(reader.nonsense);

    check(refused.answered, "an unknown target was never answered at all");
    check(refused.refused, "an unknown target was answered with data");
};

auto tX11ClipboardReadsOthers =
    test("X11/clipboardReadsAnotherClientsSelection") = []
{
    if (!x11ClipboardReachable())
        return;

    const auto guard = X11ClipboardGuard {};

    auto server =
        X11SelectionServer {"from another client", X11SelectionServer::Mode::Normal};

    if (!server.isOwner())
        return;

    check(Clipboard::hasText());
    check(Clipboard::getText() == "from another client");
    check(Clipboard::getText() == "from another client");
};

auto tX11ClipboardOwnershipMoves = test("X11/clipboardOwnershipMovesAway") = []
{
    if (!x11ClipboardReachable())
        return;

    const auto guard = X11ClipboardGuard {};

    auto watcher = X11TestClient {};

    if (!watcher.isValid())
        return;

    check(Clipboard::copyText("ours for now"));

    const auto ourOwner = watcher.selectionOwner();

    check(ourOwner != XCB_NONE);

    auto server =
        X11SelectionServer {"theirs now", X11SelectionServer::Mode::Normal};

    if (!server.isOwner())
        return;

    // The SelectionClear that took our store away arrives on the loop.
    Threads::runEventLoopUntil([&] { return watcher.selectionOwner() != ourOwner; },
                               x11ClipboardTestTimeout);

    check(watcher.selectionOwner() != ourOwner, "the selection never moved");
    check(Clipboard::getText() == "theirs now");
};

auto tX11ClipboardRefused = test("X11/clipboardIsEmptyWhenTheOwnerRefuses") = []
{
    if (!x11ClipboardReachable())
        return;

    const auto guard = X11ClipboardGuard {};

    auto server =
        X11SelectionServer {"never handed over", X11SelectionServer::Mode::Refuse};

    if (!server.isOwner())
        return;

    check(!Clipboard::hasText(), "an owner that refuses TARGETS reported text");
    check(Clipboard::getText().empty());
};

auto tX11ClipboardNoOwner = test("X11/clipboardIsEmptyWithNoOwner") = []
{
    if (!x11ClipboardReachable())
        return;

    const auto guard = X11ClipboardGuard {};

    auto watcher = X11TestClient {};

    if (!watcher.isValid())
        return;

    // Taking the selection and then disconnecting is the only way to leave it
    // with no owner at all; a desktop session usually has one, and there the
    // case has nothing to assert.
    auto server = X11SelectionServer {"briefly", X11SelectionServer::Mode::Normal};

    if (!server.isOwner())
        return;

    server.stop();

    if (watcher.selectionOwner() != XCB_NONE)
        return;

    const auto reportedText = Clipboard::hasText();
    const auto text = Clipboard::getText();

    // A desktop session's clipboard manager takes a dropped selection straight
    // back, and under XWayland the compositor proxies it within a millisecond
    // or two. Where one did, there was an owner after all and this case has
    // nothing left to say; on a bare Xvfb nothing is listening and it asserts.
    if (watcher.selectionOwner() != XCB_NONE)
        return;

    check(!reportedText, "an ownerless selection reported text");
    check(text.empty());
};

// text/uri-list is not text: a Paste menu driven by hasText must stay disabled
// when the clipboard holds files.
auto tX11ClipboardFiles = test("X11/clipboardFilesOfferNoText") = []
{
    if (!x11ClipboardReachable())
        return;

    const auto guard = X11ClipboardGuard {};

    auto reader = X11ForeignReader {};

    if (!reader.isValid())
        return;

    check(Clipboard::copyFiles({"/tmp/eacp clipboard.txt"}));
    check(!Clipboard::hasText());
    check(Clipboard::getText().empty());

    const auto offered = reader.readTargets();

    check(offered.contains(reader.uriList), "text/uri-list was not offered");
    check(!offered.contains(reader.utf8), "files were offered as text");

    const auto list = reader.read(reader.uriList);

    check(list.answered && !list.refused);
    check(list.data == "file:///tmp/eacp%20clipboard.txt\r\n");
};

// The way every toolkit sends anything large: the property holds INCR and a
// size, and the payload follows a chunk at a time as we delete each one.
auto tX11ClipboardIncremental = test("X11/clipboardReadsAnIncrementalTransfer") = []
{
    if (!x11ClipboardReachable())
        return;

    const auto guard = X11ClipboardGuard {};

    const auto payload = x11LongClipboardText();

    auto server =
        X11SelectionServer {payload, X11SelectionServer::Mode::Incremental};

    if (!server.isOwner())
        return;

    const auto pasted = Clipboard::getText();

    check(pasted.size() == payload.size(), "an INCR transfer came back short");
    check(pasted == payload);
};

// The inverse skip of every case above, and the whole of what a copy with no
// server to reach must do: nothing installed a backend, so Core's own empty
// answers come back, at once and without a crash.
auto tX11ClipboardWithoutAServer =
    test("X11/clipboardWithoutAServerAnswersEmpty") = []
{
    if (x11ClipboardReachable())
        return;

    check(!Clipboard::copyText("nowhere to put this"));
    check(!Clipboard::copyFiles({"/tmp/eacp clipboard.txt"}));
    check(!Clipboard::hasText());
    check(Clipboard::getText().empty());
};
