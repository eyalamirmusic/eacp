#pragma once

#include <string>

namespace AppHub
{

struct PrivilegedHelperInstallResult
{
    bool ok = false;
    std::string error;
};

PrivilegedHelperInstallResult installPrivilegedHelper();

} // namespace AppHub
