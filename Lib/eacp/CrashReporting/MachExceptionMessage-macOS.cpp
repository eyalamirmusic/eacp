#include "MachExceptionMessage-macOS.h"

#include <algorithm>
#include <cstring>

namespace eacp::CrashReporting::Detail
{
namespace
{
// mach_exc.defs' routine number for mach_exception_raise_state_identity; a
// MIG reply's id is the request's plus this offset.
constexpr auto raiseStateIdentityRequestId = mach_msg_id_t {2407};
constexpr auto migReplyIdOffset = mach_msg_id_t {100};

struct MachExceptionName
{
    exception_type_t type;
    const char* name;
};

constexpr auto machExceptionNames = std::to_array<MachExceptionName>({
    {EXC_BAD_ACCESS, "EXC_BAD_ACCESS"},
    {EXC_BAD_INSTRUCTION, "EXC_BAD_INSTRUCTION"},
    {EXC_ARITHMETIC, "EXC_ARITHMETIC"},
    {EXC_BREAKPOINT, "EXC_BREAKPOINT"},
    {EXC_GUARD, "EXC_GUARD"},
    {EXC_CRASH, "EXC_CRASH"},
});

#pragma pack(push, 4)
struct RaiseStateIdentityRequest
{
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t thread;
    mach_msg_port_descriptor_t task;
    NDR_record_t ndr;
    exception_type_t exception;
    mach_msg_type_number_t codeCount;
    std::int64_t codes[2];
    int flavor;
    mach_msg_type_number_t stateCount;
    natural_t state[THREAD_STATE_MAX];
};
#pragma pack(pop)

struct ReceivedRaiseStateIdentityRequest
{
    RaiseStateIdentityRequest request;
    mach_msg_max_trailer_t trailer;
};

bool isRaiseStateIdentityRequest(const RaiseStateIdentityRequest& request)
{
    auto isComplex = (request.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0;
    return request.header.msgh_id == raiseStateIdentityRequestId && isComplex
           && request.body.msgh_descriptor_count == 2
           && request.codeCount <= std::size(request.codes)
           && request.stateCount <= THREAD_STATE_MAX;
}

MachException makeMachException(const RaiseStateIdentityRequest& request)
{
    auto exception = MachException {};
    exception.type = request.exception;
    std::copy_n(request.codes, request.codeCount, exception.codes.begin());
    exception.thread = request.thread.name;
    exception.task = request.task.name;
    exception.stateFlavor = request.flavor;
    exception.stateCount = request.stateCount;
    std::copy_n(request.state, request.stateCount, exception.state.begin());
    exception.replyPort = request.header.msgh_remote_port;
    exception.requestId = request.header.msgh_id;
    return exception;
}
} // namespace

std::optional<MachException> receiveExceptionMessage(mach_port_t exceptionPort)
{
    auto received = ReceivedRaiseStateIdentityRequest {};
    auto result = ::mach_msg(&received.request.header,
                             MACH_RCV_MSG,
                             0,
                             sizeof(received),
                             exceptionPort,
                             MACH_MSG_TIMEOUT_NONE,
                             MACH_PORT_NULL);

    if (result != MACH_MSG_SUCCESS)
        return std::nullopt;

    if (!isRaiseStateIdentityRequest(received.request))
    {
        ::mach_msg_destroy(&received.request.header);
        return std::nullopt;
    }

    return makeMachException(received.request);
}

void replyToException(const MachException& exception, kern_return_t result)
{
    auto reply = mig_reply_error_t {};
    reply.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    reply.Head.msgh_size = sizeof(reply);
    reply.Head.msgh_remote_port = exception.replyPort;
    reply.Head.msgh_id = exception.requestId + migReplyIdOffset;
    reply.NDR = NDR_record;
    reply.RetCode = result;

    ::mach_msg(&reply.Head,
               MACH_SEND_MSG,
               sizeof(reply),
               0,
               MACH_PORT_NULL,
               MACH_MSG_TIMEOUT_NONE,
               MACH_PORT_NULL);
}

void releaseExceptionPorts(const MachException& exception)
{
    ::mach_port_deallocate(::mach_task_self(), exception.thread);
    ::mach_port_deallocate(::mach_task_self(), exception.task);
}

const char* getMachExceptionName(exception_type_t type)
{
    for (const auto& exception: machExceptionNames)
    {
        if (exception.type == type)
            return exception.name;
    }

    return "EXC_UNKNOWN";
}
} // namespace eacp::CrashReporting::Detail
