#include "Common.h"

using namespace nano;
using eacp::WebSocket::parseUrl;

namespace
{
bool rejects(const std::string& url)
{
    try
    {
        parseUrl(url);
        return false;
    }
    catch (const eacp::WebSocket::Error&)
    {
        return true;
    }
}
} // namespace

auto tWsUrlPlain = test("WebSocketUrl/defaultsPortAndTarget") = []
{
    auto url = parseUrl("ws://host.example");

    check(!url.secure);
    check(url.host == "host.example");
    check(url.port == 80);
    check(url.target == "/");
};

auto tWsUrlSecureDefaultPort = test("WebSocketUrl/secureDefaultsTo443") = []
{
    auto url = parseUrl("wss://host.example/stream");

    check(url.secure);
    check(url.port == 443);
    check(url.target == "/stream");
};

auto tWsUrlExplicitPort = test("WebSocketUrl/readsExplicitPort") = []
{
    auto url = parseUrl("wss://host.example:9443/a/b");

    check(url.host == "host.example");
    check(url.port == 9443);
    check(url.target == "/a/b");
};

auto tWsUrlKeepsQuery = test("WebSocketUrl/keepsQueryInTarget") = []
{
    auto url = parseUrl("wss://host.example/s?token=abc&x=1");

    check(url.target == "/s?token=abc&x=1");
};

// Services do put credentials in the query with no path at all. Splitting
// the authority only on '/' would fold the whole query into the hostname.
auto tWsUrlQueryWithoutPath = test("WebSocketUrl/handlesQueryWithNoPath") = []
{
    auto url = parseUrl("wss://host.example?token=abc");

    check(url.host == "host.example");
    check(url.port == 443);
    check(url.target == "/?token=abc");
};

auto tWsUrlIpv6 = test("WebSocketUrl/parsesBracketedIpv6Host") = []
{
    auto url = parseUrl("ws://[::1]:8080/s");

    check(url.host == "::1");
    check(url.port == 8080);
    check(url.target == "/s");
};

auto tWsUrlIpv6DefaultPort = test("WebSocketUrl/parsesIpv6WithoutPort") = []
{
    auto url = parseUrl("wss://[2001:db8::1]/s");

    check(url.host == "2001:db8::1");
    check(url.port == 443);
};

auto tWsUrlRejectsOtherSchemes = test("WebSocketUrl/rejectsNonWebSocketSchemes") = []
{
    check(rejects("https://host.example"));
    check(rejects("http://host.example"));
    check(rejects("host.example"));
    check(rejects(""));
};

auto tWsUrlRejectsMissingHost = test("WebSocketUrl/rejectsMissingHost") = []
{
    check(rejects("wss://"));
    check(rejects("wss:///path"));
    check(rejects("wss://:443/path"));
};

auto tWsUrlRejectsBadPort = test("WebSocketUrl/rejectsUnusablePorts") = []
{
    check(rejects("wss://host.example:0/x"));
    check(rejects("wss://host.example:abc/x"));
    check(rejects("wss://host.example:/x"));
    check(rejects("wss://host.example:70000/x"));
    check(rejects("wss://host.example:999999999999/x"));
};

// Rejected rather than ignored: a credential silently dropped from the URL
// would produce an unauthenticated connection that looks authenticated.
auto tWsUrlRejectsUserinfo = test("WebSocketUrl/rejectsUserinfo") = []
{
    check(rejects("wss://user:pass@host.example/x"));
    check(rejects("wss://user@host.example/x"));
};

auto tWsUrlRejectsUnterminatedIpv6 =
    test("WebSocketUrl/rejectsUnterminatedIpv6") = []
{ check(rejects("ws://[::1/x")); };
