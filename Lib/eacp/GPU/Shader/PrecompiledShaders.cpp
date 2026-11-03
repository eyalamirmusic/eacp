#include "PrecompiledShaders.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Logging.h>
#include <eacp/Core/Utils/Strings.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace eacp::GPU::Precompiled
{
namespace
{
using PrecompiledBytes = Vector<std::uint8_t>;

struct PrecompiledState
{
    std::mutex mutex;
    std::map<std::string, std::unique_ptr<PrecompiledBytes>, std::less<>> injected;
    std::vector<std::unique_ptr<PrecompiledBytes>> retired;
    std::optional<bool> requiredOverride;
    std::set<std::string, std::less<>> reported;
};

PrecompiledState& precompiledState()
{
    static auto instance = PrecompiledState {};
    return instance;
}

bool requiredByEnvironment()
{
    static const auto required =
        getEnvValue("EACP_REQUIRE_PRECOMPILED_SHADERS") == "1";
    return required;
}

bool anyInjected()
{
    auto& s = precompiledState();
    auto lock = std::lock_guard(s.mutex);
    return !s.injected.empty();
}

void reportRequiredMiss(std::string_view resourceName, std::string_view reason)
{
    LOG("GPU: EACP_REQUIRE_PRECOMPILED_SHADERS=1 and the precompiled shader ",
        resourceName,
        " ",
        reason,
        ", so the shader library is invalid");
}
} // namespace

ResEmbed::DataView find(std::string_view resourceName)
{
    {
        auto& s = precompiledState();
        auto lock = std::lock_guard(s.mutex);

        if (auto found = s.injected.find(resourceName); found != s.injected.end())
            return ResEmbed::View {found->second->data(), found->second->getSize()};
    }

    return ResEmbed::get(std::string(resourceName), std::string(resourceCategory));
}

void setForTesting(std::string resourceName, Vector<std::uint8_t> bytes)
{
    auto& s = precompiledState();
    auto lock = std::lock_guard(s.mutex);

    auto& slot = s.injected[std::move(resourceName)];

    if (slot != nullptr)
        s.retired.push_back(std::move(slot));

    slot = std::make_unique<PrecompiledBytes>(std::move(bytes));
}

void clearForTesting()
{
    auto& s = precompiledState();
    auto lock = std::lock_guard(s.mutex);

    for (auto& [name, bytes]: s.injected)
        s.retired.push_back(std::move(bytes));

    s.injected.clear();
    s.requiredOverride.reset();
}

bool isRequired()
{
    {
        auto& s = precompiledState();
        auto lock = std::lock_guard(s.mutex);

        if (s.requiredOverride.has_value())
            return *s.requiredOverride;
    }

    return requiredByEnvironment();
}

void setRequiredForTesting(bool required)
{
    auto& s = precompiledState();
    auto lock = std::lock_guard(s.mutex);
    s.requiredOverride = required;
}

bool isAvailable()
{
    return anyInjected() || embeddedCount() > 0;
}

// ResEmbed::get leaves an empty entry behind for every name it missed, so the
// category is non-empty after a single lookup; only a real resource counts.
int embeddedCount()
{
    auto count = 0;

    for (const auto& [name, view]:
         ResEmbed::getCategory(std::string(resourceCategory)))
        if (!view.empty())
            ++count;

    return count;
}

void reportMiss(std::string_view resourceName, std::string_view reason)
{
    {
        auto& s = precompiledState();
        auto lock = std::lock_guard(s.mutex);

        if (!s.reported.emplace(resourceName).second)
            return;
    }

    LOG("GPU: precompiled shader ",
        resourceName,
        " not used (",
        reason,
        "); compiling from source");
}

bool fallBackAfterMiss(std::string_view resourceName)
{
    if (isRequired())
    {
        reportRequiredMiss(resourceName, "is not embedded");
        return false;
    }

    if (isAvailable())
        reportMiss(resourceName, "not embedded");

    return true;
}

bool fallBackAfterLoadFailure(std::string_view resourceName, std::string_view reason)
{
    if (isRequired())
    {
        reportRequiredMiss(resourceName, Strings::concat("did not load: ", reason));
        return false;
    }

    reportMiss(resourceName, reason);
    return true;
}
} // namespace eacp::GPU::Precompiled
