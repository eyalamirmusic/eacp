#include "PrivilegedHelperClient.h"

namespace AppHub
{

PrivilegedHelperInstallResult installPrivilegedHelper()
{
    auto result = PrivilegedHelperInstallResult();
    result.ok = false;
    result.error = "privileged helper blessing is only available on macOS";
    return result;
}

} // namespace AppHub
