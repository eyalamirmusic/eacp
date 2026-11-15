#include "MessageOrigin.h"

#include <eacp/Core/Utils/Logging.h>
#include <eacp/Core/Utils/Strings.h>

namespace eacp::Graphics::detail
{
namespace
{
constexpr auto everyOrigin = std::string_view {"*"};

int defaultPortOf(std::string_view scheme)
{
    if (scheme == "http" || scheme == "ws")
        return 80;

    if (scheme == "https" || scheme == "wss")
        return 443;

    return 0;
}

std::string_view schemeOf(std::string_view origin)
{
    auto end = origin.find("://");
    return end == std::string_view::npos ? std::string_view {}
                                         : origin.substr(0, end);
}

std::string_view withoutUserInfo(std::string_view authority)
{
    auto at = authority.rfind('@');
    return at == std::string_view::npos ? authority : authority.substr(at + 1);
}

// The colon that starts a port, skipping the ones inside an IPv6 literal.
std::size_t portSeparatorIn(std::string_view authority)
{
    auto closingBracket = authority.rfind(']');
    auto colon = authority.rfind(':');

    if (colon == std::string_view::npos
        || (closingBracket != std::string_view::npos && colon < closingBracket))
        return std::string_view::npos;

    return colon;
}
} // namespace

std::string makeOrigin(std::string_view scheme, std::string_view host, int port)
{
    if (scheme.empty() || host.empty())
        return {};

    auto lowerScheme = Strings::toLower(scheme);
    auto origin = lowerScheme + "://" + Strings::toLower(host);

    if (port > 0 && port != defaultPortOf(lowerScheme))
        origin += ":" + std::to_string(port);

    return origin;
}

std::string originOf(std::string_view url)
{
    auto schemeEnd = url.find("://");

    if (schemeEnd == std::string_view::npos)
        return {};

    auto scheme = url.substr(0, schemeEnd);
    auto rest = url.substr(schemeEnd + 3);
    auto authority = withoutUserInfo(rest.substr(0, rest.find_first_of("/?#")));
    auto separator = portSeparatorIn(authority);

    if (separator == std::string_view::npos)
        return makeOrigin(scheme, authority, 0);

    auto port = Strings::tryParse<int>(authority.substr(separator + 1));

    if (!port && separator + 1 != authority.size())
        return {};

    return makeOrigin(scheme, authority.substr(0, separator), port.value_or(0));
}

MessageOriginPolicy::MessageOriginPolicy(const WebView::Options& options)
{
    for (auto& [scheme, _]: options.schemes)
        ownSchemes.add(Strings::toLower(scheme));

    for (auto& [scheme, _]: options.streamingSchemes)
        ownSchemes.add(Strings::toLower(scheme));

    for (auto& entry: options.trustedOrigins)
    {
        if (entry == everyOrigin)
        {
            trustsEveryOrigin = true;
            continue;
        }

        auto origin = originOf(entry);

        if (origin.empty())
            LOG("WebView: ignoring trusted origin '", entry, "', not an origin");
        else
            listedOrigins.add(std::move(origin));
    }
}

void MessageOriginPolicy::trustInlineDocument()
{
    servesInlineDocument = true;
}

MessageOriginPolicy MessageOriginPolicy::forPopup() const
{
    auto popup = *this;
    popup.servesInlineDocument = false;
    popup.reportedOrigins.clear();
    return popup;
}

bool MessageOriginPolicy::isOwnScheme(std::string_view origin) const
{
    auto scheme = schemeOf(origin);
    return !scheme.empty() && ownSchemes.contains(std::string {scheme});
}

bool MessageOriginPolicy::allows(std::string_view origin,
                                 bool isInlineDocument) const
{
    if (trustsEveryOrigin)
        return true;

    if (isInlineDocument)
        return servesInlineDocument;

    return isOwnScheme(origin) || listedOrigins.contains(std::string {origin});
}

bool MessageOriginPolicy::admit(std::string_view origin, bool isInlineDocument)
{
    if (allows(origin, isInlineDocument))
        return true;

    auto shown = origin.empty() ? std::string_view {"an opaque origin"} : origin;

    if (reportedOrigins.emplace(shown).second)
        LOG("WebView: dropped page messages from ",
            shown,
            ", which is not in Options::trustedOrigins");

    return false;
}
} // namespace eacp::Graphics::detail
