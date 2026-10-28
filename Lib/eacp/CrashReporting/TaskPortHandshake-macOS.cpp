#include "TaskPortHandshake-macOS.h"

#include <array>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto handshakeRequestId = mach_msg_id_t {0x45414350};
constexpr auto handshakeReplyId = handshakeRequestId + 1;

constexpr auto crashExceptionMask = exception_mask_t {
    EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC
    | EXC_MASK_BREAKPOINT | EXC_MASK_GUARD | EXC_MASK_CRASH};

constexpr auto crashExceptionBehavior = static_cast<exception_behavior_t>(
    EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES);

#if defined(__arm64__)
constexpr auto crashThreadStateFlavor = thread_state_flavor_t {ARM_THREAD_STATE64};
#else
constexpr auto crashThreadStateFlavor = thread_state_flavor_t {x86_THREAD_STATE64};
#endif

struct HandshakeRequest
{
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t exceptionPort;
};

struct ReceivedHandshakeRequest
{
    HandshakeRequest request;
    mach_msg_trailer_t trailer;
};

struct HandshakeReply
{
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t appTask;
    mach_msg_port_descriptor_t appBootstrap;
};

struct ReceivedHandshakeReply
{
    HandshakeReply reply;
    mach_msg_trailer_t trailer;
};

struct PreviousExceptionPorts
{
    mach_msg_type_number_t count = 0;
    std::array<exception_mask_t, EXC_TYPES_COUNT> masks {};
    std::array<mach_port_t, EXC_TYPES_COUNT> ports {};
    std::array<exception_behavior_t, EXC_TYPES_COUNT> behaviors {};
    std::array<thread_state_flavor_t, EXC_TYPES_COUNT> flavors {};
};

PreviousExceptionPorts& getPreviousExceptionPorts()
{
    static auto previous = PreviousExceptionPorts {};
    return previous;
}

mach_msg_port_descriptor_t makePortDescriptor(mach_port_t port,
                                              mach_msg_type_name_t disposition)
{
    auto descriptor = mach_msg_port_descriptor_t {};
    descriptor.name = port;
    descriptor.disposition = disposition;
    descriptor.type = MACH_MSG_PORT_DESCRIPTOR;
    return descriptor;
}

mach_msg_timeout_t toMachTimeout(std::chrono::milliseconds timeout)
{
    return static_cast<mach_msg_timeout_t>(timeout.count());
}

std::optional<mach_port_t> allocateReceivePort()
{
    auto port = mach_port_t {MACH_PORT_NULL};
    auto self = ::mach_task_self();

    if (::mach_port_allocate(self, MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS)
        return std::nullopt;

    return port;
}

bool addSendRight(mach_port_t port)
{
    auto self = ::mach_task_self();
    return ::mach_port_insert_right(self, port, port, MACH_MSG_TYPE_MAKE_SEND)
           == KERN_SUCCESS;
}

bool isHandshakeRequest(const HandshakeRequest& request)
{
    auto isComplex = (request.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0;
    return request.header.msgh_id == handshakeRequestId && isComplex
           && request.body.msgh_descriptor_count == 1;
}

// Anything else that reaches the port, such as a lookup the child's runtime
// makes before it has its real bootstrap port, is dropped.
std::optional<HandshakeRequest>
    receiveHandshakeRequest(const HandshakePort& port,
                            std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true)
    {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());

        auto received = ReceivedHandshakeRequest {};
        auto result = ::mach_msg(&received.request.header,
                                 MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                                 0,
                                 sizeof(received),
                                 port.receiveRight,
                                 toMachTimeout(std::max(remaining, {})),
                                 MACH_PORT_NULL);

        auto hasTimeLeft = remaining.count() > 0;

        if (result == MACH_RCV_TOO_LARGE && hasTimeLeft)
            continue;

        if (result != MACH_MSG_SUCCESS)
            return std::nullopt;

        if (isHandshakeRequest(received.request))
            return received.request;

        ::mach_msg_destroy(&received.request.header);

        if (!hasTimeLeft)
            return std::nullopt;
    }
}

bool redirectCrashExceptionsTo(mach_port_t exceptionPort)
{
    auto& previous = getPreviousExceptionPorts();
    previous.count = EXC_TYPES_COUNT;

    return ::task_swap_exception_ports(::mach_task_self(),
                                       crashExceptionMask,
                                       exceptionPort,
                                       crashExceptionBehavior,
                                       crashThreadStateFlavor,
                                       previous.masks.data(),
                                       &previous.count,
                                       previous.ports.data(),
                                       previous.behaviors.data(),
                                       previous.flavors.data())
           == KERN_SUCCESS;
}

bool sendTaskPortReply(mach_port_t replyPort)
{
    auto reply = HandshakeReply {};
    reply.header.msgh_bits =
        MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
    reply.header.msgh_size = sizeof(reply);
    reply.header.msgh_remote_port = replyPort;
    reply.header.msgh_id = handshakeReplyId;
    reply.body.msgh_descriptor_count = 2;
    reply.appTask = makePortDescriptor(::mach_task_self(), MACH_MSG_TYPE_COPY_SEND);
    reply.appBootstrap = makePortDescriptor(bootstrap_port, MACH_MSG_TYPE_COPY_SEND);

    return ::mach_msg(&reply.header,
                      MACH_SEND_MSG,
                      sizeof(reply),
                      0,
                      MACH_PORT_NULL,
                      MACH_MSG_TIMEOUT_NONE,
                      MACH_PORT_NULL)
           == MACH_MSG_SUCCESS;
}

std::optional<mach_port_t> getInheritedHandshakePort()
{
    auto port = mach_port_t {MACH_PORT_NULL};
    auto result =
        ::task_get_special_port(::mach_task_self(), TASK_BOOTSTRAP_PORT, &port);

    if (result != KERN_SUCCESS || port == MACH_PORT_NULL)
        return std::nullopt;

    return port;
}

bool sendHandshakeRequest(mach_port_t handshakePort,
                          mach_port_t exceptionPort,
                          mach_port_t replyPort)
{
    auto request = HandshakeRequest {};
    request.header.msgh_bits =
        MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE)
        | MACH_MSGH_BITS_COMPLEX;
    request.header.msgh_size = sizeof(request);
    request.header.msgh_remote_port = handshakePort;
    request.header.msgh_local_port = replyPort;
    request.header.msgh_id = handshakeRequestId;
    request.body.msgh_descriptor_count = 1;
    request.exceptionPort =
        makePortDescriptor(exceptionPort, MACH_MSG_TYPE_MAKE_SEND);

    return ::mach_msg(&request.header,
                      MACH_SEND_MSG,
                      sizeof(request),
                      0,
                      MACH_PORT_NULL,
                      MACH_MSG_TIMEOUT_NONE,
                      MACH_PORT_NULL)
           == MACH_MSG_SUCCESS;
}

std::optional<HandshakeReply>
    receiveHandshakeReply(mach_port_t replyPort, std::chrono::milliseconds timeout)
{
    auto received = ReceivedHandshakeReply {};
    auto result = ::mach_msg(&received.reply.header,
                             MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                             0,
                             sizeof(received),
                             replyPort,
                             toMachTimeout(timeout),
                             MACH_PORT_NULL);

    if (result != MACH_MSG_SUCCESS)
        return std::nullopt;

    const auto& reply = received.reply;

    if (reply.header.msgh_id != handshakeReplyId
        || reply.body.msgh_descriptor_count != 2)
    {
        ::mach_msg_destroy(&received.reply.header);
        return std::nullopt;
    }

    return reply;
}

// launchd's port, which the app passed back, so the handler's own lookups
// (an uploader's network calls) work as in any other process.
void restoreBootstrapPort(mach_port_t appBootstrap)
{
    ::task_set_special_port(::mach_task_self(), TASK_BOOTSTRAP_PORT, appBootstrap);
    bootstrap_port = appBootstrap;
}

void destroyReceivePort(mach_port_t port)
{
    ::mach_port_mod_refs(::mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
}
} // namespace

std::optional<PendingTaskPortRequest> sendAppTaskPortRequest()
{
    auto handshakePort = getInheritedHandshakePort();
    auto exceptionPort = allocateReceivePort();
    auto replyPort = allocateReceivePort();

    if (!handshakePort || !exceptionPort || !replyPort
        || !sendHandshakeRequest(*handshakePort, *exceptionPort, *replyPort))
        return std::nullopt;

    ::mach_port_deallocate(::mach_task_self(), *handshakePort);
    return PendingTaskPortRequest {*exceptionPort, *replyPort};
}

std::optional<HandlerMachPorts>
    receiveAppTaskPort(const PendingTaskPortRequest& request,
                       std::chrono::milliseconds timeout)
{
    auto reply = receiveHandshakeReply(request.replyPort, timeout);
    destroyReceivePort(request.replyPort);

    if (!reply)
    {
        destroyReceivePort(request.exceptionPort);
        return std::nullopt;
    }

    restoreBootstrapPort(reply->appBootstrap.name);
    return HandlerMachPorts {request.exceptionPort, reply->appTask.name};
}

std::optional<HandshakePort> createHandshakePort()
{
    auto port = allocateReceivePort();

    if (!port)
        return std::nullopt;

    auto handshake = HandshakePort {*port};

    if (!addSendRight(*port))
    {
        destroyHandshakePort(handshake);
        return std::nullopt;
    }

    return handshake;
}

void destroyHandshakePort(HandshakePort& port)
{
    if (port.receiveRight == MACH_PORT_NULL)
        return;

    ::mach_port_deallocate(::mach_task_self(), port.receiveRight);
    ::mach_port_mod_refs(
        ::mach_task_self(), port.receiveRight, MACH_PORT_RIGHT_RECEIVE, -1);
    port.receiveRight = MACH_PORT_NULL;
}

bool answerHandshakeWithTaskPort(const HandshakePort& port,
                                 std::chrono::milliseconds timeout)
{
    auto request = receiveHandshakeRequest(port, timeout);

    if (!request)
        return false;

    auto exceptionPort = request->exceptionPort.name;
    auto replyPort = request->header.msgh_remote_port;
    auto redirected = redirectCrashExceptionsTo(exceptionPort);
    ::mach_port_deallocate(::mach_task_self(), exceptionPort);

    if (!redirected || !sendTaskPortReply(replyPort))
    {
        ::mach_port_deallocate(::mach_task_self(), replyPort);
        return false;
    }

    return true;
}

void restorePreviousExceptionPorts()
{
    auto& previous = getPreviousExceptionPorts();

    for (auto index = mach_msg_type_number_t {0}; index < previous.count; ++index)
    {
        ::task_set_exception_ports(::mach_task_self(),
                                   previous.masks[index],
                                   previous.ports[index],
                                   previous.behaviors[index],
                                   previous.flavors[index]);
    }

    previous.count = 0;
}
} // namespace eacp::CrashReporting::Detail
