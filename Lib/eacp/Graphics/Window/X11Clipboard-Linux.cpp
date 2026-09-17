#include "X11Clipboard-Linux.h"

#include <cstdlib>
#include <cstring>

namespace eacp::Graphics
{
namespace
{
// A read blocks the message thread, so it cannot block for long: an owner that
// stops answering must not hang the application.
constexpr auto x11ClipboardTimeout = Time::MS {2000};

// 256 KB a round trip while a property is being read out in pieces.
constexpr uint32_t x11ClipboardChunkWords = 1u << 16;

template <typename T>
const T& x11SelectionAs(const xcb_generic_event_t& event)
{
    return *reinterpret_cast<const T*>(&event);
}

// What one ChangeProperty can carry: the server's limit less its header, with
// a word or two of slack.
size_t x11ClipboardPropertyLimit(xcb_connection_t* connection)
{
    const auto words = (size_t) xcb_get_maximum_request_length(connection);

    return words > 8 ? (words - 8) * 4 : 0;
}
} // namespace

struct X11Clipboard::Call
{
    explicit Call(X11Clipboard& owner)
        : clipboard(owner)
    {
        ++clipboard.callsInFlight;
    }

    ~Call()
    {
        if (--clipboard.callsInFlight > 0 || !clipboard.clearWhenIdle)
            return;

        clipboard.clearWhenIdle = false;
        linuxClearClipboard(LinuxWindowSystem::X11);
    }

    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;

    X11Clipboard& clipboard;
};

X11Clipboard::X11Clipboard(X11Connection& connectionToUse)
    : connection(connectionToUse)
{
    auto backend = Clipboard::Backend {};

    backend.copyText = [this](std::string_view text) { return copyText(text); };
    backend.copyFiles = [this](const Vector<std::string>& paths)
    { return copyFiles(paths); };
    backend.getText = [this] { return getText(); };
    backend.hasText = [this] { return hasText(); };

    linuxInstallClipboard(LinuxWindowSystem::X11, std::move(backend));
}

X11Clipboard::~X11Clipboard()
{
    linuxClearClipboard(LinuxWindowSystem::X11);

    if (window != XCB_NONE && connection.isConnected())
    {
        xcb_destroy_window(xcb(), window);
        connection.flush();
    }
}

void X11Clipboard::connectionLost()
{
    window = XCB_NONE;
    ownedData.clear();
    ownedContent = Content::None;

    if (callsInFlight > 0)
    {
        clearWhenIdle = true;
        return;
    }

    linuxClearClipboard(LinuxWindowSystem::X11);
}

// Made on the first copy or paste rather than with the connection: a copy that
// never touches the clipboard should cost the server nothing. Never mapped, so
// it takes no space on any screen and needs no window manager to agree to it.
bool X11Clipboard::ensureWindow()
{
    if (window != XCB_NONE)
        return true;

    if (!connection.isConnected() || connection.getScreen() == nullptr)
        return false;

    auto* screen = connection.getScreen();
    const auto created = xcb_generate_id(xcb());

    const uint32_t values[] = {XCB_EVENT_MASK_PROPERTY_CHANGE};

    xcb_create_window(xcb(),
                      XCB_COPY_FROM_PARENT,
                      created,
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

    connection.flush();

    if (!connection.isConnected())
        return false;

    window = created;

    return true;
}

xcb_window_t X11Clipboard::selectionOwner()
{
    auto* reply = xcb_get_selection_owner_reply(
        xcb(), xcb_get_selection_owner(xcb(), atoms().clipboard), nullptr);

    if (reply == nullptr)
        return XCB_NONE;

    const auto owner = reply->owner;
    std::free(reply);

    return owner;
}

// CurrentTime is the right timestamp here rather than a lazy one: the server
// replaces it with its own, and nothing of ours serves TIMESTAMP for a
// clipboard manager to compare.
bool X11Clipboard::takeSelection(std::string data, Content kind)
{
    if (!ensureWindow())
        return false;

    ownedData = std::move(data);
    ownedContent = kind;

    xcb_set_selection_owner(xcb(), window, atoms().clipboard, XCB_CURRENT_TIME);
    connection.flush();

    if (connection.isConnected() && selectionOwner() == window)
        return true;

    ownedData.clear();
    ownedContent = Content::None;

    return false;
}

bool X11Clipboard::copyText(std::string_view text)
{
    const auto call = Call {*this};

    return takeSelection(std::string {text}, Content::Text);
}

bool X11Clipboard::copyFiles(const Vector<std::string>& paths)
{
    const auto call = Call {*this};

    if (paths.empty())
        return false;

    return takeSelection(linuxUriList(paths), Content::Files);
}

std::string X11Clipboard::getText()
{
    const auto call = Call {*this};

    if (!ensureWindow())
        return {};

    const auto owner = selectionOwner();

    if (owner == XCB_NONE)
        return {};

    // Our own selection, answered without a round trip: a conversion we would
    // have to serve from the very thread that is waiting for it.
    if (owner == window)
        return ownedContent == Content::Text ? ownedData : std::string {};

    const auto target = pickTextTarget();

    if (target == XCB_ATOM_NONE)
        return {};

    return convertSelection(target);
}

bool X11Clipboard::hasText()
{
    const auto call = Call {*this};

    if (!ensureWindow())
        return false;

    const auto owner = selectionOwner();

    if (owner == XCB_NONE)
        return false;

    if (owner == window)
        return ownedContent == Content::Text;

    return pickTextTarget() != XCB_ATOM_NONE;
}

bool X11Clipboard::handleEvent(const xcb_generic_event_t& event)
{
    if (window == XCB_NONE)
        return false;

    switch (event.response_type & ~0x80)
    {
        case XCB_SELECTION_REQUEST:
        {
            const auto& request =
                x11SelectionAs<xcb_selection_request_event_t>(event);

            if (request.owner != window)
                return false;

            serveRequest(request);
            return true;
        }

        case XCB_SELECTION_CLEAR:
        {
            const auto& cleared = x11SelectionAs<xcb_selection_clear_event_t>(event);

            if (cleared.owner != window)
                return false;

            ownedData.clear();
            ownedContent = Content::None;
            return true;
        }

        case XCB_SELECTION_NOTIFY:
        {
            const auto& notify = x11SelectionAs<xcb_selection_notify_event_t>(event);

            if (notify.requestor != window)
                return false;

            notifyProperty = notify.property;
            notifyArrived = true;
            return true;
        }

        case XCB_PROPERTY_NOTIFY:
        {
            const auto& changed = x11SelectionAs<xcb_property_notify_event_t>(event);

            if (changed.window != window)
                return false;

            if (changed.atom == atoms().eacpSelection
                && changed.state == XCB_PROPERTY_NEW_VALUE)
                chunkArrived = true;

            return true;
        }

        default:
            break;
    }

    return false;
}

// MULTIPLE and every target we do not hold fall through to a refusal, which is
// a SelectionNotify naming no property.
void X11Clipboard::serveRequest(const xcb_selection_request_event_t& request)
{
    // A requestor old enough to leave the property out means the target.
    const auto property =
        request.property != XCB_ATOM_NONE ? request.property : request.target;

    if (request.selection != atoms().clipboard || !writeRequested(request, property))
    {
        answerRequest(request, XCB_ATOM_NONE);
        return;
    }

    answerRequest(request, property);
}

bool X11Clipboard::writeRequested(const xcb_selection_request_event_t& request,
                                  xcb_atom_t property)
{
    if (request.target == atoms().targets)
        return writeTargets(request.requestor, property);

    if (ownedContent == Content::Text && isTextTarget(request.target))
    {
        // TEXT leaves the encoding to the owner, which makes it UTF-8 here.
        const auto type =
            request.target == atoms().text ? atoms().utf8String : request.target;

        return writeData(request.requestor, property, type);
    }

    if (ownedContent == Content::Files && request.target == atoms().textUriList)
        return writeData(request.requestor, property, request.target);

    return false;
}

bool X11Clipboard::writeTargets(xcb_window_t requestor, xcb_atom_t property)
{
    const auto targets = offeredTargets();

    xcb_change_property(xcb(),
                        XCB_PROP_MODE_REPLACE,
                        requestor,
                        property,
                        XCB_ATOM_ATOM,
                        32,
                        (uint32_t) targets.getSize(),
                        targets.data());

    return true;
}

// No INCR on this side of a transfer: nothing this framework puts on a
// clipboard comes near a server's maximum request, and a refused paste is a
// far better answer than a silently truncated one.
bool X11Clipboard::writeData(xcb_window_t requestor,
                             xcb_atom_t property,
                             xcb_atom_t type)
{
    if (ownedData.size() > x11ClipboardPropertyLimit(xcb()))
    {
        LOG("X11: a clipboard selection of ",
            ownedData.size(),
            " bytes is larger than one request to this server, and this "
            "backend does not send INCR transfers. The paste was refused.");

        return false;
    }

    xcb_change_property(xcb(),
                        XCB_PROP_MODE_REPLACE,
                        requestor,
                        property,
                        type,
                        8,
                        (uint32_t) ownedData.size(),
                        ownedData.data());

    return true;
}

void X11Clipboard::answerRequest(const xcb_selection_request_event_t& request,
                                 xcb_atom_t property)
{
    // xcb_send_event reads 32 bytes whatever the event really is.
    alignas(xcb_selection_notify_event_t) char buffer[32] = {};

    auto& notify = *reinterpret_cast<xcb_selection_notify_event_t*>(buffer);

    notify.response_type = XCB_SELECTION_NOTIFY;
    notify.time = request.time;
    notify.requestor = request.requestor;
    notify.selection = request.selection;
    notify.target = request.target;
    notify.property = property;

    xcb_send_event(xcb(), 0, request.requestor, XCB_EVENT_MASK_NO_EVENT, buffer);

    connection.flush();
}

// STRING and TEXT are what Motif-era code still asks for and the mime
// spellings what a toolkit does; each costs one word to offer.
Vector<xcb_atom_t> X11Clipboard::offeredTargets() const
{
    auto targets = Vector<xcb_atom_t> {};
    targets.add(atoms().targets);

    if (ownedContent == Content::Text)
    {
        targets.add(atoms().utf8String);
        targets.add(atoms().textPlainUtf8);
        targets.add(atoms().textPlain);
        targets.add(XCB_ATOM_STRING);
        targets.add(atoms().text);
    }

    if (ownedContent == Content::Files)
        targets.add(atoms().textUriList);

    return targets;
}

bool X11Clipboard::isTextTarget(xcb_atom_t target) const
{
    return target == atoms().utf8String || target == atoms().textPlainUtf8
           || target == atoms().textPlain || target == XCB_ATOM_STRING
           || target == atoms().text;
}

// UTF-8 first and STRING last, which is Latin-1 by the letter of the spec and
// UTF-8 from every toolkit written since.
xcb_atom_t X11Clipboard::pickTextTarget()
{
    const auto offered = availableTargets();

    const xcb_atom_t wanted[] = {atoms().utf8String,
                                 atoms().textPlainUtf8,
                                 XCB_ATOM_STRING,
                                 atoms().textPlain,
                                 atoms().text};

    for (auto target: wanted)
        if (offered.contains(target))
            return target;

    return XCB_ATOM_NONE;
}

// An owner that will not answer TARGETS is treated as holding nothing: ICCCM
// has required it of every selection owner since 1988.
Vector<xcb_atom_t> X11Clipboard::availableTargets()
{
    const auto answer = convertSelection(atoms().targets);
    const auto count = answer.size() / sizeof(xcb_atom_t);

    auto targets = Vector<xcb_atom_t> {};
    auto atom = xcb_atom_t {};

    for (auto i = size_t {0}; i < count; ++i)
    {
        std::memcpy(&atom, answer.data() + i * sizeof(xcb_atom_t), sizeof(atom));
        targets.add(atom);
    }

    return targets;
}

std::string X11Clipboard::convertSelection(xcb_atom_t target)
{
    if (!ensureWindow())
        return {};

    xcb_delete_property(xcb(), window, atoms().eacpSelection);

    notifyArrived = false;
    notifyProperty = XCB_ATOM_NONE;

    xcb_convert_selection(xcb(),
                          window,
                          atoms().clipboard,
                          target,
                          atoms().eacpSelection,
                          XCB_CURRENT_TIME);

    connection.flush();

    const auto answered = connection.dispatchUntil(
        [this] { return notifyArrived; }, Time::Deadline {x11ClipboardTimeout});

    // No answer at all, or the owner refusing the target outright.
    if (!answered || notifyProperty == XCB_ATOM_NONE || !connection.isConnected())
        return {};

    auto type = xcb_atom_t {XCB_ATOM_NONE};
    auto data = readProperty(type);

    // The property held the size, not the data; deleting it was the go-ahead.
    if (type == atoms().incr)
        return readIncrementally();

    return data;
}

// Deleted once read: a selection is not consumed by a paste, but the property
// it was delivered into is ours to clear, and clearing it is what asks an INCR
// owner for the next chunk.
std::string X11Clipboard::readProperty(xcb_atom_t& type)
{
    type = XCB_ATOM_NONE;

    auto data = std::string {};
    auto offset = uint32_t {0};

    while (connection.isConnected())
    {
        auto* reply =
            xcb_get_property_reply(xcb(),
                                   xcb_get_property(xcb(),
                                                    0,
                                                    window,
                                                    atoms().eacpSelection,
                                                    XCB_GET_PROPERTY_TYPE_ANY,
                                                    offset,
                                                    x11ClipboardChunkWords),
                                   nullptr);

        if (reply == nullptr)
            break;

        type = reply->type;

        const auto* bytes = (const char*) xcb_get_property_value(reply);
        const auto length = (size_t) xcb_get_property_value_length(reply);
        const auto more = reply->bytes_after > 0;

        data.append(bytes, length);
        offset += (uint32_t) (length / 4);

        std::free(reply);

        if (!more)
            break;
    }

    xcb_delete_property(xcb(), window, atoms().eacpSelection);
    connection.flush();

    return data;
}

// The owner writes one property at a time and waits for each to be deleted;
// the zero-length one ends the transfer. Each chunk gets the whole timeout,
// because an owner that is still feeding us is not an owner that has hung.
std::string X11Clipboard::readIncrementally()
{
    auto data = std::string {};

    while (connection.isConnected())
    {
        chunkArrived = false;

        if (!connection.dispatchUntil([this] { return chunkArrived; },
                                      Time::Deadline {x11ClipboardTimeout}))
            break;

        auto type = xcb_atom_t {XCB_ATOM_NONE};
        const auto chunk = readProperty(type);

        if (chunk.empty())
            break;

        data += chunk;
    }

    return data;
}
} // namespace eacp::Graphics
