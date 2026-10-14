#include "Common.h"
#include <eacp/Network/HTTP/HttpProtocol.h>

#include <clocale>
#include <initializer_list>

using namespace nano;
using eacp::HTTP::DownloadWrite;
using eacp::HTTP::QueryParameter;
using eacp::HTTP::Request;
using eacp::HTTP::URL;
using eacp::HTTP::urlDecode;
using eacp::HTTP::urlEncode;

namespace
{
std::string percentEscaped(unsigned char byte)
{
    constexpr auto digits = std::string_view("0123456789ABCDEF");
    auto escaped = std::string("%");
    escaped.push_back(digits[byte >> 4]);
    escaped.push_back(digits[byte & 0x0F]);
    return escaped;
}

bool everyHighByteIsEscaped()
{
    for (auto value = 0x80; value <= 0xFF; ++value)
    {
        auto byte = (unsigned char) value;
        auto text = std::string(1, (char) byte);

        if (urlEncode(text) != percentEscaped(byte))
            return false;
    }

    return true;
}

// A Latin-1 locale is the one where isalnum() calls bytes such as 0xE9 letters,
// which is how a locale-dependent check let them through unescaped.
class Latin1Locale
{
public:
    Latin1Locale()
        : previous(std::setlocale(LC_CTYPE, nullptr))
    {
        for (auto* name: {"en_US.ISO8859-1", "en_US.ISO-8859-1", "en_US.UTF-8"})
            if (std::setlocale(LC_CTYPE, name) != nullptr)
                return;
    }

    ~Latin1Locale() { std::setlocale(LC_CTYPE, previous.c_str()); }

private:
    std::string previous;
};

DownloadWrite writeFor(int status, const std::string& contentRange, int64_t from)
{
    auto headers = std::map<std::string, std::string>();

    if (!contentRange.empty())
        headers["Content-Range"] = contentRange;

    return eacp::HTTP::downloadWriteFor(status, headers, from);
}
} // namespace

auto tEncodesNonAsciiBytes = test("HttpUrlEncode/escapesEveryNonAsciiByte") = []
{ check(everyHighByteIsEscaped()); };

auto tEncodingIgnoresLocale =
    test("HttpUrlEncode/escapesNonAsciiWhateverTheLocale") = []
{
    auto locale = Latin1Locale();
    check(everyHighByteIsEscaped());
    check(urlEncode("caf\xE9") == "caf%E9");
};

auto tEncodesReservedSet = test("HttpUrlEncode/escapesEveryReservedCharacter") = []
{
    auto reserved = std::string(":/?#[]@!$&'()*+,;=");

    for (auto c: reserved)
        check(urlEncode(std::string(1, c)) == percentEscaped((unsigned char) c));

    check(urlEncode("(x)") == "%28x%29");
};

auto tEncodesSpaceAndControls = test("HttpUrlEncode/escapesSpaceAndControls") = []
{
    check(urlEncode(" ") == "%20");
    check(urlEncode("a b c") == "a%20b%20c");
    check(urlEncode("\t\n") == "%09%0A");
    check(urlEncode(std::string(1, '\0')) == "%00");
};

auto tKeepsOnlyUnreserved = test("HttpUrlEncode/keepsExactlyTheUnreservedSet") = []
{
    for (auto value = 0; value < 0x80; ++value)
    {
        auto c = (char) value;
        auto unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                          || (c >= '0' && c <= '9') || c == '-' || c == '.'
                          || c == '_' || c == '~';
        auto encoded = urlEncode(std::string(1, c));

        check(unreserved ? encoded == std::string(1, c) : encoded.size() == 3);
    }
};

auto tNonAsciiRoundTrips = test("HttpUrlEncode/nonAsciiRoundTrips") = []
{
    auto text =
        std::string("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xF0\x9F\x8E\xB5 caf\xC3\xA9");
    check(urlDecode(urlEncode(text)) == text);
};

auto tUrlDefaults = test("HttpURL/startsWithJustTheBase") = []
{
    auto url = URL("https://example.com/api");
    check(url.base == "https://example.com/api");
    check(url.parameters.empty());
    check(url.toString() == "https://example.com/api");
};

auto tUrlAddsInOrder = test("HttpURL/addParameterKeepsOrderAndEncodes") = []
{
    auto url = URL("https://example.com/search");
    auto& same = url.addParameter("q", "rock & roll").addParameter("page", "2");

    check(&same == &url);
    check(url.parameters.size() == 2);
    check(url.parameters[0].name == "q");
    check(url.query() == "q=rock%20%26%20roll&page=2");
    check(url.toString() == "https://example.com/search?q=rock%20%26%20roll&page=2");
};

auto tUrlRepeatsNames = test("HttpURL/keepsRepeatedNames") = []
{
    auto url = URL("http://h/p");
    url.addParameter("tag", "a").addParameter("tag", "b");
    check(url.toString() == "http://h/p?tag=a&tag=b");
    check(url.getParameter("tag") == "a");
};

auto tUrlParses = test("HttpURL/parseSplitsBaseQueryAndFragment") = []
{
    auto url = URL::parse("https://example.com/a/b?x=1&name=caf%C3%A9&flag#top");

    check(url.base == "https://example.com/a/b");
    check(url.parameters.size() == 3);
    check(url.parameters[0] == QueryParameter {"x", "1"});
    check(url.parameters[1] == QueryParameter {"name", "caf\xC3\xA9"});
    check(url.parameters[2] == QueryParameter {"flag", ""});
    check(url.fragment == "top");
    check(url.hasParameter("flag"));
    check(!url.hasParameter("missing"));
    check(url.getParameter("missing").empty());
};

auto tUrlParsesWithoutQuery = test("HttpURL/parseWithoutQuery") = []
{
    auto url = URL::parse("http://127.0.0.1:8080/path");
    check(url.base == "http://127.0.0.1:8080/path");
    check(url.parameters.empty());
    check(url.fragment.empty());
};

auto tUrlRoundTrips = test("HttpURL/roundTripsThroughToString") = []
{
    auto text =
        std::string("https://example.com/p?a=1&b=two%20words&c=%2F%3F%26#frag");
    auto url = URL::parse(text);

    check(url.toString() == text);
    check(URL::parse(url.toString()) == url);
    check(url.getParameter("c") == "/?&");
};

auto tUrlBuiltRoundTrips = test("HttpURL/builtUrlParsesBack") = []
{
    auto url = URL("http://h/x");
    url.addParameter("k e y", "v&l=u+e").addParameter("\xC3\xBC", "()");

    auto parsed = URL::parse(url.toString());
    check(parsed == url);
    check(parsed.getParameter("k e y") == "v&l=u+e");
};

auto tQueryParametersKeepOrder =
    test("HttpParseQueryString/parametersKeepOrder") = []
{
    auto parameters = eacp::HTTP::parseQueryParameters("b=2&a=1&b=3");
    check(parameters.size() == 3);
    check(parameters[0] == QueryParameter {"b", "2"});
    check(parameters[2] == QueryParameter {"b", "3"});

    auto map = eacp::HTTP::parseQueryString("b=2&a=1&b=3");
    check(map.size() == 2);
    check(map["b"] == "3");
};

auto tResolvesAbsolute = test("HttpRedirectLocation/absoluteIsTakenAsIs") = []
{
    check(eacp::HTTP::resolveRedirectLocation("http://a/x", "https://b/y")
          == "https://b/y");
};

auto tResolvesSchemeRelative =
    test("HttpRedirectLocation/schemeRelativeKeepsTheScheme") = []
{
    check(eacp::HTTP::resolveRedirectLocation("https://a:1/x/y", "//b/z")
          == "https://b/z");
};

auto tResolvesHostRelative = test("HttpRedirectLocation/hostRelative") = []
{
    check(eacp::HTTP::resolveRedirectLocation("http://a:8080/x/y?q=1", "/z")
          == "http://a:8080/z");
    check(eacp::HTTP::resolveRedirectLocation("http://a", "/z") == "http://a/z");
};

auto tResolvesPathRelative = test("HttpRedirectLocation/pathRelative") = []
{
    check(eacp::HTTP::resolveRedirectLocation("http://a/x/y?q=1", "z?r=2")
          == "http://a/x/z?r=2");
    check(eacp::HTTP::resolveRedirectLocation("http://a", "z") == "http://a/z");
    check(eacp::HTTP::resolveRedirectLocation("http://a/x", "?p=1")
          == "http://a/x?p=1");
};

auto tRedirectTurnsPostIntoGet = test("HttpRedirectedRequest/seeOtherIsAGet") = []
{
    auto post = Request::post("http://a/form", "data");
    post.headers["Content-Type"] = "text/plain";

    auto next = eacp::HTTP::redirectedRequest(post, 303, "/done");

    check(next.url == "http://a/done");
    check(next.type == "GET");
    check(next.body.empty());
    check(!next.hasHeader("Content-Type"));
};

auto tRedirectKeepsMethodOn307 = test("HttpRedirectedRequest/307KeepsThePost") = []
{
    auto post = Request::post("http://a/form", "data");
    auto next = eacp::HTTP::redirectedRequest(post, 307, "/again");

    check(next.type == "POST");
    check(next.body == "data");
};

auto tRedirectDropsAuthAcrossOrigins =
    test("HttpRedirectedRequest/authorizationStaysWithItsOrigin") = []
{
    auto req = Request("http://a/x");
    req.headers["Authorization"] = "Bearer t";

    check(eacp::HTTP::redirectedRequest(req, 302, "/y").hasHeader("Authorization"));
    check(!eacp::HTTP::redirectedRequest(req, 302, "http://b/y")
               .hasHeader("Authorization"));
};

auto tParsesContentRange = test("HttpContentRange/parsesEveryForm") = []
{
    auto span = eacp::HTTP::parseContentRange("bytes 100-199/1000");
    check(span.first == 100 && span.last == 199 && span.total == 1000);

    auto unknownTotal = eacp::HTTP::parseContentRange("bytes 5-9/*");
    check(unknownTotal.first == 5 && unknownTotal.total == -1);

    auto unsatisfied = eacp::HTTP::parseContentRange("bytes */1000");
    check(unsatisfied.first == -1 && unsatisfied.total == 1000);

    auto garbage = eacp::HTTP::parseContentRange("items 1-2/3");
    check(garbage.first == -1 && garbage.total == -1);
};

auto tDownloadWriteDecisions = test("HttpDownloadWrite/followsTheStatus") = []
{
    check(writeFor(200, "", 0) == DownloadWrite::Replace);
    check(writeFor(404, "", 0) == DownloadWrite::Replace);
    check(writeFor(206, "bytes 400-999/1000", 400) == DownloadWrite::Append);
    check(writeFor(206, "bytes 0-999/1000", 400) == DownloadWrite::Keep);
    check(writeFor(200, "", 400) == DownloadWrite::Replace);
    check(writeFor(416, "bytes */400", 400) == DownloadWrite::Keep);
    check(writeFor(500, "", 400) == DownloadWrite::Keep);
};

auto tUrlJoinsOntoAQuery = test("HttpURL/joinsParametersOntoAnExistingQuery") = []
{
    auto url = URL("http://h/p");
    url.base = "http://h/p?a=1";
    url.addParameter("b", "2");
    check(url.toString() == "http://h/p?a=1&b=2");

    auto open = URL::parse("http://h/x?");
    check(open.toString() == "http://h/x?");
    open.addParameter("k", "v");
    check(open.toString() == "http://h/x?k=v");
};

auto tUrlConstructorParses = test("HttpURL/constructorParsesItsArgument") = []
{
    auto url = URL("http://h/p?a=1");
    check(url.base == "http://h/p");
    url.addParameter("b", "2");
    check(url.toString() == "http://h/p?a=1&b=2");
};

auto tUrlFragmentStartsAtFirstHash =
    test("HttpURL/fragmentStartsAtTheFirstHash") = []
{
    auto url = URL::parse("http://h/p#frag?not=query");
    check(url.base == "http://h/p");
    check(url.parameters.empty());
    check(url.fragment == "frag?not=query");
};

auto tUrlPlusIsASpace = test("HttpURL/plusInAParsedQueryIsASpace") = []
{
    auto url = URL::parse("http://h/?q=a+b&p=a%2Bb");
    check(url.getParameter("q") == "a b");
    check(url.getParameter("p") == "a+b");
    check(url.toString() == "http://h/?q=a%20b&p=a%2Bb");
};

auto tResolvesDotSegments = test("HttpRedirectLocation/removesDotSegments") = []
{
    auto resolve = [](const std::string& location)
    { return eacp::HTTP::resolveRedirectLocation("http://a/b/c/d?q", location); };

    check(resolve("../e") == "http://a/b/e");
    check(resolve("./e") == "http://a/b/c/e");
    check(resolve("../../../../e") == "http://a/e");
    check(resolve("/x/./y/../z?r") == "http://a/x/z?r");
    check(resolve("..") == "http://a/b/");
    check(resolve(".") == "http://a/b/c/");
};

auto tResolvesFragment = test("HttpRedirectLocation/fragmentOnlyKeepsTheUrl") = []
{
    check(eacp::HTTP::resolveRedirectLocation("http://a/x?q=1#old", "#new")
          == "http://a/x?q=1#new");
};

auto tRedirectDropsCredentials =
    test("HttpRedirectedRequest/credentialsStayWithTheirOrigin") = []
{
    auto req = Request("http://a/x");
    req.headers["Cookie"] = "s=1";
    req.headers["Proxy-Authorization"] = "Basic x";

    auto same = eacp::HTTP::redirectedRequest(req, 302, "/y");
    check(same.hasHeader("Cookie") && same.hasHeader("Proxy-Authorization"));

    auto other = eacp::HTTP::redirectedRequest(req, 302, "http://b/y");
    check(!other.hasHeader("Cookie") && !other.hasHeader("Proxy-Authorization"));
};

auto tDetectsDowngrade = test("HttpRedirectedRequest/detectsADowngrade") = []
{
    check(eacp::HTTP::isSchemeDowngrade("https://a/x", "http://a/y"));
    check(!eacp::HTTP::isSchemeDowngrade("http://a/x", "https://a/y"));
    check(!eacp::HTTP::isSchemeDowngrade("https://a/x", "https://b/y"));
};

auto tRedirectErrorWithoutLimit =
    test("HttpRedirect/errorNamesNoLimitWhenThereIsNone") = []
{
    check(eacp::HTTP::tooManyRedirectsError(-1).find("-1") == std::string::npos);
    check(eacp::HTTP::tooManyRedirectsError(3).find('3') != std::string::npos);
};

auto tIdentityEncoding = test("HttpRange/prefersTheIdentityEncoding") = []
{
    auto req = Request();
    eacp::HTTP::preferIdentityEncoding(req);
    check(eacp::HTTP::asksForIdentityEncoding(req));

    auto chosen = Request();
    chosen.headers["accept-encoding"] = "gzip";
    eacp::HTTP::preferIdentityEncoding(chosen);
    check(chosen.getHeader("Accept-Encoding") == "gzip");
    check(!eacp::HTTP::asksForIdentityEncoding(chosen));
};
