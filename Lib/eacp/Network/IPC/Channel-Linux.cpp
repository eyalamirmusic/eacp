#include "ChannelInternal.h"

#include <cstdlib>
#include <string>
#include <unistd.h>

namespace eacp::IPC::detail
{

FilePath channelRoot()
{
    // The XDG runtime directory is the canonical per-user socket home on
    // Linux. The /tmp fallback is shared between users, so the directory
    // name carries the uid and channelDirectory() verifies ownership.
    if (auto* runtime = std::getenv("XDG_RUNTIME_DIR");
        runtime != nullptr && runtime[0] != '\0')
        return FilePath {runtime} / "eacp.channels";

    return FilePath::tempDirectory()
           / ("eacp.channels-" + std::to_string(::getuid()));
}

} // namespace eacp::IPC::detail
