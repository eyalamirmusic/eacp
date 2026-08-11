#include "ChannelInternal.h"

#include <string>
#include <unistd.h>

namespace eacp::IPC::detail
{

FilePath channelRoot()
{
    // NOT the per-user temp dir: DARWIN_USER_TEMP_DIR is per-process-context,
    // so an out-of-process AU host resolves a private one and never meets its
    // peer. /tmp is shared, hence the uid in the name plus an ownership check.
    return FilePath {"/tmp"} / ("eacp.channels-" + std::to_string(::getuid()));
}

} // namespace eacp::IPC::detail
