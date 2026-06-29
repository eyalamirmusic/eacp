#include "PrivilegedHelperClient.h"

#import <Security/Authorization.h>
#import <ServiceManagement/ServiceManagement.h>

namespace AppHub
{
namespace
{

std::string stringFromCFString(CFStringRef value)
{
    if (value == nullptr)
        return {};

    auto length = CFStringGetLength(value);
    auto maxSize =
        CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    auto buffer = std::string(static_cast<std::size_t>(maxSize), '\0');
    if (!CFStringGetCString(value,
                            buffer.data(),
                            maxSize,
                            kCFStringEncodingUTF8))
        return {};

    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    return buffer;
}

std::string stringFromCFError(CFErrorRef error)
{
    if (error == nullptr)
        return {};

    auto description = CFErrorCopyDescription(error);
    auto out = stringFromCFString(description);
    if (description != nullptr)
        CFRelease(description);
    return out;
}

} // namespace

PrivilegedHelperInstallResult installPrivilegedHelper()
{
    auto result = PrivilegedHelperInstallResult();

    AuthorizationRef auth = nullptr;
    auto status = AuthorizationCreate(nullptr,
                                      kAuthorizationEmptyEnvironment,
                                      kAuthorizationFlagDefaults,
                                      &auth);
    if (status != errAuthorizationSuccess)
    {
        result.error = "AuthorizationCreate failed";
        return result;
    }

    AuthorizationItem right = {
        kSMRightBlessPrivilegedHelper,
        0,
        nullptr,
        0,
    };
    AuthorizationRights rights = {1, &right};
    auto flags = static_cast<AuthorizationFlags>(
        kAuthorizationFlagDefaults | kAuthorizationFlagInteractionAllowed
        | kAuthorizationFlagPreAuthorize | kAuthorizationFlagExtendRights);

    status = AuthorizationCopyRights(auth,
                                     &rights,
                                     kAuthorizationEmptyEnvironment,
                                     flags,
                                     nullptr);
    if (status != errAuthorizationSuccess)
    {
        AuthorizationFree(auth, kAuthorizationFlagDefaults);
        result.error = "AuthorizationCopyRights failed";
        return result;
    }

    CFErrorRef error = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    auto blessed = SMJobBless(kSMDomainSystemLaunchd,
                              CFSTR(EACP_APPHUB_HELPER_LABEL),
                              auth,
                              &error);
#pragma clang diagnostic pop
    AuthorizationFree(auth, kAuthorizationFlagDefaults);

    if (!blessed)
    {
        result.error = "SMJobBless failed";
        auto description = stringFromCFError(error);
        if (error != nullptr)
            CFRelease(error);
        if (!description.empty())
            result.error += ": " + description;
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace AppHub
