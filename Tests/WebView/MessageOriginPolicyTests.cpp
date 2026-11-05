#include <eacp/WebView/WebView/MessageOrigin.h>
#include <NanoTest/NanoTest.h>
// The sender rule behind Options::trustedOrigins, with no web runtime under
// it. MessageOriginTests drives the same rule through a real WebView.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace MessageOriginPolicyTests
{
WebView::Options withTrustedOrigins(Vector<std::string> origins)
{
    auto options = WebView::Options {};
    options.trustedOrigins = std::move(origins);
    return options;
}

auto tOriginOfUrl = test("MessageOriginPolicy/originOfKeepsSchemeHostAndPort") = []
{
    check(detail::originOf("https://Example.COM/path?q=1#frag")
          == "https://example.com");
    check(detail::originOf("https://example.com:443/") == "https://example.com");
    check(detail::originOf("http://localhost:5173") == "http://localhost:5173");
    check(detail::originOf("http://user:pw@host:8080/x") == "http://host:8080");
    check(detail::originOf("http://[::1]:8080/") == "http://[::1]:8080");
    check(detail::originOf("app://local/index.html") == "app://local");
};

auto tOriginOfOpaque = test("MessageOriginPolicy/originOfOpaqueIsEmpty") = []
{
    check(detail::originOf("about:blank").empty());
    check(detail::originOf("data:text/html,hi").empty());
    check(detail::originOf("").empty());
    check(detail::originOf("https://host:notaport/").empty());
};

auto tMakeOrigin = test("MessageOriginPolicy/makeOriginDropsDefaultPort") = []
{
    check(detail::makeOrigin("HTTPS", "Example.com", 443) == "https://example.com");
    check(detail::makeOrigin("http", "example.com", 8080)
          == "http://example.com:8080");
    check(detail::makeOrigin("", "", 0).empty());
};

auto tOwnSchemes = test("MessageOriginPolicy/defaultTrustsOnlyOwnSchemes") = []
{
    auto options = WebView::Options {};
    options.schemes["app"] = [](std::string_view)
    { return std::optional<ResourceResponse> {}; };
    options.streamingSchemes["media"] = [](std::string_view)
    { return std::optional<StreamingResource> {}; };

    auto policy = detail::MessageOriginPolicy {options};
    check(policy.allows("app://local", false));
    check(policy.allows("media://anything", false));
    check(!policy.allows("https://example.com", false));
    check(!policy.allows("", false));
};

auto tListed = test("MessageOriginPolicy/listedOriginsMatchExactly") = []
{
    auto policy = detail::MessageOriginPolicy {withTrustedOrigins(
        {"https://example.com:443/ui/", "http://localhost:8080"})};

    check(policy.allows("https://example.com", false));
    check(policy.allows("http://localhost:8080", false));
    check(!policy.allows("http://example.com", false));
    check(!policy.allows("https://sub.example.com", false));
    check(!policy.allows("http://localhost:8081", false));
};

auto tWildcard = test("MessageOriginPolicy/wildcardTrustsEverySender") = []
{
    auto policy = detail::MessageOriginPolicy {withTrustedOrigins({"*"})};
    check(policy.allows("https://example.com", false));
    check(policy.allows("", false));
    check(policy.allows("", true));
};

auto tInline = test("MessageOriginPolicy/inlineDocumentNeedsLoadHTML") = []
{
    auto policy = detail::MessageOriginPolicy {WebView::Options {}};
    check(!policy.allows("", true));

    policy.trustInlineDocument();
    check(policy.allows("", true));
    check(!policy.allows("", false));
    check(!policy.forPopup().allows("", true));
};

auto tAdmit = test("MessageOriginPolicy/admitAnswersAsAllows") = []
{
    auto policy =
        detail::MessageOriginPolicy {withTrustedOrigins({"https://example.com"})};
    check(policy.admit("https://example.com", false));
    check(!policy.admit("https://evil.example", false));
    check(!policy.admit("https://evil.example", false));
};
} // namespace MessageOriginPolicyTests
