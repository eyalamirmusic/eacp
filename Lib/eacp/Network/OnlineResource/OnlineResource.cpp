#include "OnlineResource.h"
#include "OnlineResources.h"

#include "../HTTP/Http.h"

#include <eacp/Core/Utils/File.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/StdPath.h>
#include <eacp/Core/Utils/Zip.h>

#include <Miro/Json.h>
#include <Miro/Reflect.h>

#include <eacp/Core/Threads/Timer.h>

#include <filesystem>
#include <stdexcept>
#include <thread>

namespace eacp
{
namespace
{
constexpr auto sidecarSuffix = ".resource.json";
constexpr auto partialSuffix = ".part";
constexpr auto extractingSuffix = ".extracting";

// What a finished download leaves beside itself: enough to tell a later
// fetch whether the copy is the one asked for, and what to ask the server.
struct Sidecar
{
    std::string url;
    std::string version;
    std::string etag;
    std::string lastModified;

    MIRO_REFLECT(url, version, etag, lastModified)

    bool hasValidators() const { return !etag.empty() || !lastModified.empty(); }
};

std::string findHeader(const std::map<std::string, std::string>& headers,
                       std::string_view key)
{
    for (const auto& [k, v]: headers)
        if (Strings::equalsCaseInsensitive(k, key))
            return v;

    return {};
}

std::string fileNameFromUrl(const std::string& url)
{
    auto withoutQuery = url.substr(0, url.find_first_of("?#"));

    while (!withoutQuery.empty() && withoutQuery.back() == '/')
        withoutQuery.pop_back();

    return Files::filenameFromPath(withoutQuery);
}

bool isZipName(const std::string& fileName)
{
    return Strings::equalsCaseInsensitive(FilePath {fileName}.extension(), ".zip");
}

std::string withoutExtension(const std::string& fileName)
{
    auto extension = FilePath {fileName}.extension();
    return fileName.substr(0, fileName.size() - extension.size());
}

FilePath sidecarPathFor(const FilePath& target)
{
    return FilePath {target.str() + sidecarSuffix};
}

std::optional<Sidecar> readSidecar(const FilePath& target)
{
    auto sidecarPath = sidecarPathFor(target);

    if (!File {sidecarPath}.exists())
        return std::nullopt;

    try
    {
        return Miro::createFromJSONString<Sidecar>(Files::readFile(sidecarPath));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

void writeSidecar(const FilePath& target, const Sidecar& sidecar)
{
    auto text = Miro::toJSONString(sidecar);
    auto bytes = Span<const std::uint8_t> {
        reinterpret_cast<const std::uint8_t*>(text.data()), (int) text.size()};

    Files::writeFileAtomically(sidecarPathFor(target), bytes);
}

void removeQuietly(const FilePath& path)
{
    auto ignored = std::error_code {};
    std::filesystem::remove_all(toStdPath(path), ignored);
}

bool renameOver(const FilePath& from, const FilePath& to)
{
    removeQuietly(to);

    auto error = std::error_code {};
    std::filesystem::rename(toStdPath(from), toStdPath(to), error);
    return !error;
}

bool createDirectory(const FilePath& directory)
{
    auto error = std::error_code {};
    std::filesystem::create_directories(toStdPath(directory), error);
    return !error;
}
} // namespace

// Everything the worker touches, shared with it so the OnlineResource can go
// away mid-transfer. Progress is read by the UI from any thread; the promise
// is settled on the main thread only, and only if nobody abandoned it.
struct OnlineResource::Job
{
    using Stage = Progress::Stage;

    HTTP::DownloadProgress transfer;
    std::atomic<Stage> stage {Stage::idle};
    std::atomic<bool> running {false};
    std::atomic<bool> abandoned {false};
    Threads::AsyncPromise<Result> promise;

    Progress progress() const
    {
        auto result = Progress {};
        result.stage = stage;
        result.bytesReceived = transfer.bytesReceived;
        result.totalBytes = transfer.totalBytes;

        if (result.totalBytes > 0)
            result.fraction =
                (float) ((double) result.bytesReceived / (double) result.totalBytes);

        return result;
    }
};

namespace
{
// The worker's view of one fetch, captured by value so it reads nothing
// from the OnlineResource after start.
struct FetchPlan
{
    OnlineResource::Info info;
    FilePath directory;
    FilePath target;
    bool isZip = false;
    std::optional<Sidecar> previous;
};

FilePath partialPath(const FetchPlan& plan)
{
    return plan.directory / (plan.info.fileName + partialSuffix);
}

FilePath extractingPath(const FetchPlan& plan)
{
    return FilePath {plan.target.str() + extractingSuffix};
}

void discardLeftovers(const FetchPlan& plan)
{
    removeQuietly(partialPath(plan));
    removeQuietly(extractingPath(plan));
}

std::string describeFailure(const HTTP::Response& response)
{
    if (!response.error.empty())
        return response.error;

    return "HTTP " + std::to_string(response.statusCode);
}

HTTP::Request conditionalRequest(const FetchPlan& plan,
                                 HTTP::DownloadProgress& transfer)
{
    auto request = HTTP::Request {plan.info.url};
    request.progress = &transfer;
    request.timeout = plan.info.timeout;

    if (plan.previous)
    {
        if (!plan.previous->etag.empty())
            request.headers["If-None-Match"] = plan.previous->etag;

        if (!plan.previous->lastModified.empty())
            request.headers["If-Modified-Since"] = plan.previous->lastModified;
    }

    return request;
}

// From the finished .part to a resource in place: unpacked into a sibling
// folder first for a zip, so the rename is the only moment the old copy is
// gone, then the sidecar last, so a copy with no sidecar is one a crash cut
// short and the next fetch replaces it.
bool install(const FetchPlan& plan, const HTTP::Response& response)
{
    auto partial = partialPath(plan);

    if (plan.isZip)
    {
        auto reader = Zip::Reader {partial};
        auto extracting = extractingPath(plan);
        removeQuietly(extracting);

        auto extracted = reader.isValid() && reader.extractAll(extracting);
        removeQuietly(partial);

        if (!extracted || !renameOver(extracting, plan.target))
            return false;
    }
    else if (!renameOver(partial, plan.target))
    {
        return false;
    }

    auto sidecar = Sidecar {};
    sidecar.url = plan.info.url;
    sidecar.version = plan.info.version;
    sidecar.etag = findHeader(response.headers, "ETag");
    sidecar.lastModified = findHeader(response.headers, "Last-Modified");

    try
    {
        writeSidecar(plan.target, sidecar);
    }
    catch (const std::exception&)
    {
        return false;
    }

    return true;
}

OnlineResource::Result
    performFetch(const FetchPlan& plan,
                 HTTP::DownloadProgress& transfer,
                 std::atomic<OnlineResource::Progress::Stage>& stage)
{
    using Stage = OnlineResource::Progress::Stage;

    auto result = OnlineResource::Result {};
    result.path = plan.target;

    auto cancelled = [&] { return transfer.cancel.load(); };

    if (cancelled())
    {
        result.cancelled = true;
        return result;
    }

    if (!createDirectory(plan.directory))
    {
        result.error = "Could not create " + plan.directory.str();
        return result;
    }

    discardLeftovers(plan);
    stage = Stage::downloading;

    auto request = conditionalRequest(plan, transfer);
    auto response = request.downloadTo(partialPath(plan).str());

    if (cancelled())
    {
        discardLeftovers(plan);
        result.cancelled = true;
        return result;
    }

    if (response.statusCode == 304 && plan.previous)
    {
        discardLeftovers(plan);
        result.ok = true;
        return result;
    }

    if (!response.error.empty() || response.statusCode != 200)
    {
        discardLeftovers(plan);
        result.error = describeFailure(response);
        return result;
    }

    stage = Stage::extracting;

    if (!install(plan, response))
    {
        discardLeftovers(plan);
        result.error = plan.isZip ? "Could not unpack " + plan.info.fileName
                                  : "Could not write " + plan.target.str();
        return result;
    }

    result.ok = true;
    result.downloaded = true;
    return result;
}
} // namespace

FilePath OnlineResource::defaultDirectory()
{
    return OnlineResources::get().getDirectory();
}

OnlineResource::OnlineResource(Info infoToUse,
                               FilePath directoryToUse,
                               Freshness freshnessToUse)
    : resource(std::move(infoToUse))
    , folder(std::move(directoryToUse))
    , freshness(freshnessToUse)
{
    if (resource.fileName.empty())
        resource.fileName = fileNameFromUrl(resource.url);

    if (resource.fileName.empty())
        resource.fileName = resource.name;

    if (resource.name.empty())
        resource.name = resource.fileName;
}

OnlineResource::~OnlineResource()
{
    if (job == nullptr)
        return;

    job->abandoned = true;
    job->transfer.cancel = true;
    job->promise.abandon();
}

FilePath OnlineResource::path() const
{
    if (isZipName(resource.fileName))
        return folder / withoutExtension(resource.fileName);

    return folder / resource.fileName;
}

bool OnlineResource::isAvailable() const
{
    auto target = path();

    if (!File {target}.exists())
        return false;

    auto sidecar = readSidecar(target);

    return sidecar && sidecar->url == resource.url
           && sidecar->version == resource.version;
}

bool OnlineResource::isRunning() const
{
    return job != nullptr && job->running;
}

void OnlineResource::cancel()
{
    if (job != nullptr)
        job->transfer.cancel = true;
}

OnlineResource::Progress OnlineResource::progress() const
{
    return job == nullptr ? Progress {} : job->progress();
}

bool OnlineResource::remove()
{
    if (isRunning())
        return false;

    auto target = path();
    removeQuietly(target);
    removeQuietly(sidecarPathFor(target));
    OnlineResources::get().reportRemoved(target);
    return true;
}

Threads::Async<OnlineResource::Result> OnlineResource::start()
{
    if (isRunning())
    {
        auto refused = Threads::AsyncPromise<Result> {};
        auto result = Result {};
        result.error = resource.name + " is already being fetched";
        result.path = path();
        refused.resolve(std::move(result));
        return refused.get();
    }

    // A fresh job per fetch: a UI may still be reading the previous one's
    // progress, and the previous promise may still be settling.
    job = std::make_shared<Job>();
    auto current = job;

    auto plan = FetchPlan {};
    plan.info = resource;
    plan.directory = folder;
    plan.target = path();
    plan.isZip = isZipName(resource.fileName);

    if (File {plan.target}.exists())
        plan.previous = readSidecar(plan.target);

    auto matchesRequest = plan.previous && plan.previous->url == resource.url
                          && plan.previous->version == resource.version;

    if (!matchesRequest)
        plan.previous.reset();

    auto askServer = matchesRequest && freshness == Freshness::check
                     && plan.previous->hasValidators();

    auto& registry = OnlineResources::get();
    registry.reportStarted(
        *this,
        [current] { return current->progress(); },
        [current] { current->transfer.cancel = true; });

    if (matchesRequest && !askServer)
    {
        current->stage = Progress::Stage::done;

        auto result = Result {};
        result.ok = true;
        result.path = plan.target;
        registry.reportFinished(plan.target, result);
        current->promise.resolve(std::move(result));
        return current->promise.get();
    }

    current->running = true;

    // The registry hears the outcome even when the object is gone: the
    // file landed, or did not, whoever was waiting for it.
    std::thread {
        [current, plan = std::move(plan)]
        {
            auto result = performFetch(plan, current->transfer, current->stage);
            current->stage = Progress::Stage::done;

            Threads::callAsync(
                [current, target = plan.target, result = std::move(result)]() mutable
                {
                    current->running = false;
                    OnlineResources::get().reportFinished(target, result);

                    if (!current->abandoned)
                        current->promise.resolve(std::move(result));
                });
        }}
        .detach();

    return current->promise.get();
}

namespace
{
// Keeps the object and its progress timer alive from fetchAsync until the
// result has been delivered; the completion drops the timer, which breaks
// the cycle between this and the timer's own callback.
struct OwnedFetch
{
    std::unique_ptr<OnlineResource> resource;
    std::unique_ptr<Threads::Timer> timer;
};

Threads::Async<OnlineResource::Result>
    startOwned(const std::shared_ptr<OwnedFetch>& owned,
               OnlineResource::Options options)
{
    auto info = static_cast<OnlineResource::Info&>(options);
    owned->resource = std::make_unique<OnlineResource>(
        std::move(info), std::move(options.directory), options.freshness);

    auto async = owned->resource->start();

    if (!async.isReady())
        owned->timer = std::make_unique<Threads::Timer>(
            [owned, onProgress = options.onProgress]
            { onProgress(owned->resource->progress()); },
            options.progressInterval);

    async.then(
        [owned,
         onProgress = std::move(options.onProgress),
         onFinished =
             std::move(options.onFinished)](const OnlineResource::Result& result)
        {
            owned->timer.reset();
            onProgress(owned->resource->progress());
            onFinished(result);
        });

    return async;
}
} // namespace

Threads::Async<OnlineResource::Result> OnlineResource::fetchAsync(Options options)
{
    return startOwned(std::make_shared<OwnedFetch>(), std::move(options));
}

OnlineResource::Result OnlineResource::fetch(Options options)
{
    Threads::assertMainThread();

    auto owned = std::make_shared<OwnedFetch>();
    auto async = startOwned(owned, std::move(options));

    while (!async.isReady())
        Threads::runEventLoopFor(Time::MS {20});

    auto result = async.waitFor(Time::MS {0});
    const auto& name = owned->resource->info().name;

    if (result.cancelled)
        throw std::runtime_error {"Cancelled fetching " + name};

    if (!result.ok)
        throw std::runtime_error {"Could not fetch " + name + ": " + result.error};

    return result;
}
} // namespace eacp
