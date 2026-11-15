#include "FilesPlatform.h"

// iOS and Android: an app's files live in its own sandbox, and there is no
// file manager the system would show them in, so there is nothing to hand
// them to.
namespace eacp::Detail
{
bool openWithSystem(const FilePath& /*target*/)
{
    return false;
}

bool revealInFileManager(const FilePath& /*target*/)
{
    return false;
}
} // namespace eacp::Detail
