#include "WebCommon.h"

#include <eacp/Core/Utils/Zip.h>

using namespace nano;
using eacp::OnlineResource;

namespace OnlineResourceWebTests
{
using namespace WebTests;

OnlineResource::Options optionsFor(const std::string& path,
                                   const std::string& fileName,
                                   const std::string& directory)
{
    auto options = OnlineResource::Options {};
    options.info.url = url(path);
    options.info.fileName = fileName;
    options.directory = scratchDirectory(directory);
    return options;
}

std::optional<OnlineResource::Result>
    fetchedAsync(const OnlineResource::Options& options)
{
    auto result = std::optional<OnlineResource::Result> {};
    auto withCallback = options;
    withCallback.onFinished = [&](const OnlineResource::Result& finished)
    { result = finished; };

    OnlineResource::fetchAsync(withCallback);
    check(waitFor(result));
    return result;
}

std::string zipOf(const std::map<std::string, std::string>& files)
{
    auto writer = eacp::Zip::Writer {};

    for (const auto& [name, text]: files)
        check(writer.add(name, std::string_view {text}));

    auto bytes = writer.finish();
    return {reinterpret_cast<const char*>(bytes.data()), (std::size_t) bytes.size()};
}

auto tPlain = test("WebOnlineResource/fetchAsyncDownloadsAFile") = []
{
    auto options = optionsFor("/bytes?size=20000", "plain.bin", "web-or-plain");
    auto result = fetchedAsync(options);

    check(result->ok);
    check(result->downloaded);
    check(readFile(result->path.str()) == pattern(0, 20000));
    check(!exists(eacp::FilePath {result->path.str() + ".part"}));
};

auto tZip = test("WebOnlineResource/fetchAsyncUnpacksAZip") = []
{
    auto archive = tempPath("web-or-archive.zip");
    writeBytes(archive, zipOf({{"a.txt", "first"}, {"dir/b.txt", "second"}}));

    auto options = optionsFor(
        "/file?path=" + eacp::HTTP::urlEncode(archive), "bundle.zip", "web-or-zip");
    auto result = fetchedAsync(options);

    check(result->ok);
    check(result->path.str().ends_with("bundle"));
    check(readFile((result->path / "a.txt").str()) == "first");
    check(readFile((result->path / "dir" / "b.txt").str()) == "second");
};

auto tRevalidate = test("WebOnlineResource/aSecondFetchRevalidatesWith304") = []
{
    auto options =
        optionsFor("/etag?tag=web-v1&size=5000", "etag.bin", "web-or-etag");

    auto first = fetchedAsync(options);
    check(first->ok);
    check(first->downloaded);
    check(readFile(first->path.str()) == pattern(0, 5000));

    auto second = fetchedAsync(options);
    check(second->ok);
    check(!second->downloaded);
    check(readFile(second->path.str()) == pattern(0, 5000));
};

auto tCancel = test("WebOnlineResource/cancelEndsAStalledFetch") = []
{
    auto options =
        optionsFor("/stall?after=1000&ms=0", "stalled.bin", "web-or-cancel");
    auto resource = OnlineResource {options.info, options.directory};

    auto result = std::optional<OnlineResource::Result> {};
    resource.start().then([&](const OnlineResource::Result& finished)
                          { result = finished; });
    eacp::Threads::callAfter(eacp::Time::MS {200}, [&] { resource.cancel(); });

    check(waitFor(result));
    check(result->cancelled);
    check(!result->ok);
    check(!resource.isAvailable());
};

auto tFailure = test("WebOnlineResource/anErrorStatusIsAFailure") = []
{
    auto options = optionsFor("/status?code=404", "missing.bin", "web-or-404");
    auto result = fetchedAsync(options);

    check(!result->ok);
    check(result->error == "HTTP 404");
};

auto tBlocking =
    test("WebOnlineResource/blockingFetchWorksWhereTheSuiteCanBlock") = []
{
    auto options = optionsFor("/bytes?size=3000", "blocking.bin", "web-or-blocking");
    auto result = OnlineResource::fetch(options);

    check(result.ok);
    check(readFile(result.path.str()) == pattern(0, 3000));
};
} // namespace OnlineResourceWebTests
