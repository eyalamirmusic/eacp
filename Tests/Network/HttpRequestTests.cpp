#include <eacp/Network/HTTP/Http.h>
#include <NanoTest/NanoTest.h>

using namespace nano;
using eacp::HTTP::Request;
using eacp::HTTP::Response;

auto tDefaultRequest = test("HttpRequest/defaultsToEmptyGet") = []
{
    auto req = Request();
    check(req.url.empty());
    check(req.type == "GET");
    check(req.body.empty());
    check(req.headers.empty());
    check(req.formFields.empty());
    check(req.fileFields.empty());
};

auto tConstructorSetsUrl = test("HttpRequest/constructorSetsUrl") = []
{
    auto req = Request("https://example.com/path");
    check(req.url == "https://example.com/path");
    check(req.type == "GET");
};

auto tPostFactorySetsType = test("HttpRequest/postFactorySetsTypeAndBody") = []
{
    auto req = Request::post("https://example.com/api", "{\"x\":1}");
    check(req.url == "https://example.com/api");
    check(req.type == "POST");
    check(req.body == "{\"x\":1}");
};

auto tPostFactoryEmptyBody = test("HttpRequest/postFactoryAllowsEmptyBody") = []
{
    auto req = Request::post("https://example.com/api");
    check(req.type == "POST");
    check(req.body.empty());
};

auto tAddFormFieldAppends = test("HttpRequest/addFormFieldAppendsField") = []
{
    auto req = Request("https://example.com");
    req.addFormField("name", "alice");
    check(req.formFields.size() == 1);
    check(req.formFields[0].name == "name");
    check(req.formFields[0].value == "alice");
};

auto tAddFormFieldSwitchesToPost =
    test("HttpRequest/addFormFieldSwitchesToPost") = []
{
    auto req = Request("https://example.com");
    check(req.type == "GET");
    req.addFormField("a", "1");
    check(req.type == "POST");
};

auto tAddFormFieldChains = test("HttpRequest/addFormFieldReturnsSelf") = []
{
    auto req = Request("https://example.com");
    auto& ret = req.addFormField("a", "1").addFormField("b", "2");
    check(&ret == &req);
    check(req.formFields.size() == 2);
    check(req.formFields[1].name == "b");
    check(req.formFields[1].value == "2");
};

auto tAddFileFieldAppends = test("HttpRequest/addFileFieldAppendsField") = []
{
    auto req = Request("https://example.com");
    req.addFileField("upload", "/tmp/data.json", "application/json");
    check(req.fileFields.size() == 1);
    check(req.fileFields[0].fieldName == "upload");
    check(req.fileFields[0].filePath == "/tmp/data.json");
    check(req.fileFields[0].contentType == "application/json");
};

auto tAddFileFieldDefaultContentType =
    test("HttpRequest/addFileFieldDefaultsToOctetStream") = []
{
    auto req = Request("https://example.com");
    req.addFileField("upload", "/tmp/data.bin");
    check(req.fileFields[0].contentType == "application/octet-stream");
};

auto tAddFileFieldExtractsName =
    test("HttpRequest/addFileFieldExtractsFileName") = []
{
    auto req = Request("https://example.com");
    req.addFileField("a", "/tmp/sub/dir/report.pdf");
    check(req.fileFields[0].fileName == "report.pdf");
};

auto tAddFileFieldNoSeparator = test("HttpRequest/addFileFieldKeepsBareName") = []
{
    auto req = Request("https://example.com");
    req.addFileField("a", "report.pdf");
    check(req.fileFields[0].fileName == "report.pdf");
};

auto tAddFileFieldWindowsSeparator =
    test("HttpRequest/addFileFieldHandlesBackslashes") = []
{
    auto req = Request("https://example.com");
    req.addFileField("a", "C:\\Users\\me\\report.pdf");
    check(req.fileFields[0].fileName == "report.pdf");
};

auto tAddFileFieldSwitchesToPost =
    test("HttpRequest/addFileFieldSwitchesToPost") = []
{
    auto req = Request("https://example.com");
    req.addFileField("upload", "/tmp/data.bin");
    check(req.type == "POST");
};

auto tAddFileFieldChains = test("HttpRequest/addFileFieldReturnsSelf") = []
{
    auto req = Request("https://example.com");
    auto& ret = req.addFileField("a", "/tmp/a.bin").addFileField("b", "/tmp/b.bin");
    check(&ret == &req);
    check(req.fileFields.size() == 2);
};

auto tHeadersAreUserOwned = test("HttpRequest/headersAreCallerControlled") = []
{
    auto req = Request("https://example.com");
    req.headers["X-Custom"] = "value";
    req.headers["Accept"] = "application/json";
    check(req.headers.size() == 2);
    check(req.headers["X-Custom"] == "value");
};

auto tDefaultResponse = test("HttpResponse/defaultsAreZeroed") = []
{
    auto res = Response();
    check(res.statusCode == 0);
    check(res.content.empty());
    check(res.error.empty());
    check(res.headers.empty());
};

auto tDefaultParamsEmpty = test("HttpRequest/paramsDefaultEmpty") = []
{
    auto req = Request();
    check(req.params.empty());
    check(req.remoteAddr.empty());
    check(req.remotePort == -1);
};

auto tUrlDecodePlain = test("HttpUrlDecode/passesThroughPlainText") = []
{
    check(eacp::HTTP::urlDecode("hello") == "hello");
    check(eacp::HTTP::urlDecode("") == "");
};

auto tUrlDecodePercent = test("HttpUrlDecode/decodesPercentEscapes") = []
{
    check(eacp::HTTP::urlDecode("hello%20world") == "hello world");
    check(eacp::HTTP::urlDecode("a%2Bb") == "a+b");
    check(eacp::HTTP::urlDecode("%2F") == "/");
};

auto tUrlDecodeMixedCase = test("HttpUrlDecode/acceptsMixedHexCase") = []
{
    check(eacp::HTTP::urlDecode("%2f") == "/");
    check(eacp::HTTP::urlDecode("%2F") == "/");
    check(eacp::HTTP::urlDecode("%aB") == "\xab");
};

auto tUrlDecodePlus = test("HttpUrlDecode/decodesPlusAsSpace") = []
{ check(eacp::HTTP::urlDecode("hello+world") == "hello world"); };

auto tUrlDecodeBadEscape = test("HttpUrlDecode/leavesInvalidEscapeIntact") = []
{
    check(eacp::HTTP::urlDecode("100%") == "100%");
    check(eacp::HTTP::urlDecode("%ZZ") == "%ZZ");
};

auto tParseQueryEmpty = test("HttpParseQueryString/emptyYieldsEmpty") = []
{ check(eacp::HTTP::parseQueryString("").empty()); };

auto tParseQuerySingle = test("HttpParseQueryString/singlePair") = []
{
    auto p = eacp::HTTP::parseQueryString("foo=bar");
    check(p.size() == 1);
    check(p["foo"] == "bar");
};

auto tParseQueryMultiple = test("HttpParseQueryString/multiplePairs") = []
{
    auto p = eacp::HTTP::parseQueryString("a=1&b=2&c=3");
    check(p.size() == 3);
    check(p["a"] == "1");
    check(p["b"] == "2");
    check(p["c"] == "3");
};

auto tParseQueryDecoded = test("HttpParseQueryString/decodesValues") = []
{
    auto p = eacp::HTTP::parseQueryString("msg=hello%20world&path=%2Ftmp");
    check(p["msg"] == "hello world");
    check(p["path"] == "/tmp");
};

auto tParseQueryDecodedKeys = test("HttpParseQueryString/decodesKeys") = []
{
    auto p = eacp::HTTP::parseQueryString("a%20b=c");
    check(p["a b"] == "c");
};

auto tParseQueryFlagOnly = test("HttpParseQueryString/keyWithoutValueIsEmpty") = []
{
    auto p = eacp::HTTP::parseQueryString("flag&other=1");
    check(p.size() == 2);
    check(p["flag"] == "");
    check(p["other"] == "1");
};

auto tParseQueryEmptyValue = test("HttpParseQueryString/explicitEmptyValue") = []
{
    auto p = eacp::HTTP::parseQueryString("k=");
    check(p.size() == 1);
    check(p["k"] == "");
};

auto tParseQuerySkipsBlankSegments =
    test("HttpParseQueryString/ignoresBlankSegments") = []
{
    auto p = eacp::HTTP::parseQueryString("&a=1&&b=2&");
    check(p.size() == 2);
    check(p["a"] == "1");
    check(p["b"] == "2");
};
