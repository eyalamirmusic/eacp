#pragma once

#include <eacp/Core/Utils/Strings.h>

#include <string>
#include <string_view>

// The named objects one session shares between the app and its handler, each
// the channel name plus a suffix.
namespace eacp::CrashReporting::Detail
{
constexpr auto sharedStateObjectSuffix = std::string_view {".state"};
constexpr auto crashEventSuffix = std::string_view {".crash"};
constexpr auto readyEventSuffix = std::string_view {".ready"};

inline std::wstring makeChannelObjectName(std::string_view channel,
                                          std::string_view suffix)
{
    return Strings::widen(std::string {"Local\\"} + std::string {channel}
                          + std::string {suffix});
}
} // namespace eacp::CrashReporting::Detail
