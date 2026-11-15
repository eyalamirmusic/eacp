#pragma once

#include "WebView.h"

#include <set>

namespace eacp::Graphics::detail
{
// "scheme://host[:port]", lower case, with the scheme's default port left out:
// the one form a sender and an Options::trustedOrigins entry are compared in.
// Empty for a URL that has no host to speak of (about:blank, data:, an opaque
// origin), which therefore matches no listed origin.
std::string originOf(std::string_view url);
std::string makeOrigin(std::string_view scheme, std::string_view host, int port);

// Decides whether a page message may reach any handler, from where its sender
// is (see Options::trustedOrigins). Both backends ask it before they look a
// handler up, so the rule is stated once and the platforms only say who sent.
class MessageOriginPolicy
{
public:
    MessageOriginPolicy() = default;
    explicit MessageOriginPolicy(const WebView::Options& options);

    // loadHTML without a base URL: the view's own document has no origin of
    // its own (about:blank), so it is trusted by being the one the view loaded
    // rather than by where it came from.
    void trustInlineDocument();

    // A window.open popup serves the opener's schemes and trusts its list,
    // but has loaded no inline document of its own.
    MessageOriginPolicy forPopup() const;

    // `isInlineDocument` is the platform's word that the sender is the
    // top-level about:blank document, the form loadHTML's content takes.
    bool allows(std::string_view origin, bool isInlineDocument) const;

    // allows(), logging the first refusal of each origin.
    bool admit(std::string_view origin, bool isInlineDocument);

private:
    bool isOwnScheme(std::string_view origin) const;

    bool trustsEveryOrigin = false;
    bool servesInlineDocument = false;
    Vector<std::string> ownSchemes;
    Vector<std::string> listedOrigins;
    std::set<std::string, std::less<>> reportedOrigins;
};
} // namespace eacp::Graphics::detail
