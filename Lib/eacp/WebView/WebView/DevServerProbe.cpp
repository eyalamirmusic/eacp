#include "DevServerProbe.h"

#include "../Common.h"

#include <eacp/Network/TCP/Connection.h>

namespace eacp::Graphics
{
namespace
{
struct HostPort
{
    std::string host;
    int port = 0;
};

int defaultPortForScheme(const std::string& url)
{
    return url.starts_with("https://") ? 443 : 80;
}

std::optional<HostPort> parseHostPort(const std::string& url)
{
    auto schemeEnd = url.find("://");

    if (schemeEnd == std::string::npos)
        return std::nullopt;

    auto hostStart = schemeEnd + 3;
    auto pathStart = url.find('/', hostStart);
    auto hostPart = url.substr(
        hostStart,
        pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);

    auto colon = hostPart.find(':');
    auto result = HostPort {};

    if (colon == std::string::npos)
    {
        result.host = hostPart;
        result.port = defaultPortForScheme(url);
        return result;
    }

    result.host = hostPart.substr(0, colon);
    auto parsedPort = Strings::tryParseInt(hostPart.substr(colon + 1));
    if (!parsedPort)
        return std::nullopt;
    result.port = *parsedPort;
    return result;
}

// A connect that completes is the whole answer; nothing is sent. A timeout of
// zero would be "wait forever" to TCP::Connection, so the shortest wait a
// caller can ask for is one millisecond.
bool probeTCP(const std::string& host, int port, int timeoutMs)
{
    if (port <= 0 || port > 65535)
        return false;

    auto wait = Time::MS {std::max(1, timeoutMs)};

    try
    {
        TCP::Connection::connect({host, (std::uint16_t) port}, {wait, wait});
        return true;
    }
    catch (const TCP::Error&)
    {
        return false;
    }
}
} // namespace

bool probeDevServer(const std::string& url, int timeoutMs)
{
    auto hp = parseHostPort(url);

    if (!hp)
        return false;

    return probeTCP(hp->host, hp->port, timeoutMs);
}
} // namespace eacp::Graphics
