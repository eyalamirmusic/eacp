#include "WebSocket.h"

namespace eacp::WebSocket
{

Message Message::text(std::string payload)
{
    return {std::move(payload), MessageType::text};
}

Message Message::binary(std::string payload)
{
    return {std::move(payload), MessageType::binary};
}

namespace
{
struct Scheme
{
    bool secure;
    std::uint16_t defaultPort;
    std::size_t prefixLength;
};

std::optional<Scheme> matchScheme(const std::string& url)
{
    if (url.starts_with("wss://"))
        return Scheme {true, 443, 6};

    if (url.starts_with("ws://"))
        return Scheme {false, 80, 5};

    return {};
}

// Splits host from port, honouring the bracket form an IPv6 literal needs
// ("[::1]:8080") - without it the first colon of the address reads as the
// port separator and the host comes out as "[".
void parseAuthority(const std::string& authority, Url& out)
{
    if (authority.empty())
        throw Error("WebSocket URL has no host");

    if (authority.find('@') != std::string::npos)
        throw Error("WebSocket URL must not carry userinfo: " + authority);

    auto hostEnd = std::string::npos;

    if (authority.front() == '[')
    {
        auto bracket = authority.find(']');

        if (bracket == std::string::npos)
            throw Error("WebSocket URL has an unterminated IPv6 host");

        out.host = authority.substr(1, bracket - 1);
        hostEnd = authority.find(':', bracket);
    }
    else
    {
        hostEnd = authority.find(':');
        out.host = authority.substr(0, hostEnd);
    }

    if (out.host.empty())
        throw Error("WebSocket URL has no host");

    if (hostEnd == std::string::npos)
        return;

    auto portText = authority.substr(hostEnd + 1);

    if (portText.empty())
        throw Error("WebSocket URL has an empty port");

    for (auto c: portText)
        if (c < '0' || c > '9')
            throw Error("WebSocket URL has a non-numeric port: " + portText);

    // Checked before converting: a port long enough to overflow would make
    // the conversion itself throw, and out_of_range is not the error the
    // caller is meant to catch.
    if (portText.size() > 5)
        throw Error("WebSocket URL port out of range: " + portText);

    auto value = std::stoul(portText);

    if (value == 0 || value > 65535)
        throw Error("WebSocket URL port out of range: " + portText);

    out.port = (std::uint16_t) value;
}
} // namespace

Url parseUrl(const std::string& url)
{
    auto scheme = matchScheme(url);

    if (!scheme)
        throw Error("WebSocket URL must start with ws:// or wss://: " + url);

    auto result = Url();
    result.secure = scheme->secure;
    result.port = scheme->defaultPort;

    auto rest = url.substr(scheme->prefixLength);

    // A query with no path ("wss://host?token=x") still ends the authority,
    // and services do send credentials that way - splitting only on '/'
    // would fold the whole query into the hostname.
    auto pathStart = rest.find_first_of("/?");

    if (pathStart == std::string::npos)
    {
        parseAuthority(rest, result);
        return result;
    }

    parseAuthority(rest.substr(0, pathStart), result);

    result.target = rest[pathStart] == '?' ? "/" + rest.substr(pathStart)
                                           : rest.substr(pathStart);

    return result;
}

} // namespace eacp::WebSocket
