#include "ChannelInternal.h"

#include <string>
#include <unistd.h>

namespace eacp::IPC::detail
{

FilePath channelRoot()
{
    // NOT the per-user temp directory: DARWIN_USER_TEMP_DIR is per-process-
    // context on macOS — Apple's out-of-process AU host (AUHostingServiceXPC)
    // resolves its own private one, so peers rooting the rendezvous there can
    // never meet. /tmp is the short, context-stable home (sun_path caps the
    // whole socket path at 104 bytes); it is shared between users, so the
    // name carries the uid and channelDirectory() verifies ownership.
    return FilePath {"/tmp"} / ("eacp.channels-" + std::to_string(::getuid()));
}

} // namespace eacp::IPC::detail
