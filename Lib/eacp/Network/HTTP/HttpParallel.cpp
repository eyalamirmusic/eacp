#include "HttpProtocol.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>

// A download split into ranged chunks, one thread each. Built where there are
// threads; elsewhere downloadFileParallel is the backend's single stream.
namespace eacp::HTTP
{
namespace
{
constexpr std::int64_t kMinParallelTotal = 1024 * 1024;
constexpr int kMaxParallelChunks = 8;

struct ChunkRange
{
    std::int64_t start;
    std::int64_t end;
};

struct RangeProbe
{
    bool supportsRanges = false;
    std::int64_t totalBytes = -1;
    std::map<std::string, std::string> headers;
};

RangeProbe probeRangeSupport(const Request& req)
{
    auto head = req;
    head.type = "HEAD";
    head.parallelChunks = 1;
    head.progress = nullptr;
    head.headers.erase("Range");
    preferIdentityEncoding(head);

    auto response = httpRequest(head);

    auto probe = RangeProbe();
    probe.headers = response.headers;

    auto succeeded = response.error.empty() && response.statusCode >= 200
                     && response.statusCode < 300;

    if (!succeeded)
        return probe;

    probe.supportsRanges =
        acceptsByteRanges(findHeaderIgnoringCase(response.headers, "Accept-Ranges"));

    if (!probe.supportsRanges)
        return probe;

    auto contentLength = findHeaderIgnoringCase(response.headers, "Content-Length");
    if (contentLength.empty())
        return probe;

    try
    {
        probe.totalBytes = std::stoll(contentLength);
    }
    catch (...)
    {
        probe.totalBytes = -1;
    }

    return probe;
}

Vector<ChunkRange> computeChunkRanges(std::int64_t total, int chunkCount)
{
    auto chunkSize = total / chunkCount;
    auto ranges = Vector<ChunkRange>(chunkCount);

    for (auto i = 0; i < chunkCount; ++i)
    {
        ranges[i].start = i * chunkSize;
        ranges[i].end = (i == chunkCount - 1) ? total - 1 : (i + 1) * chunkSize - 1;
    }

    return ranges;
}

Vector<std::string> makeChunkPaths(const std::string& filePath, int chunkCount)
{
    auto paths = Vector<std::string>(chunkCount);

    for (auto i = 0; i < chunkCount; ++i)
        paths[i] = filePath + ".part" + std::to_string(i);

    return paths;
}

void removeChunkFiles(const Vector<std::string>& chunkPaths)
{
    for (const auto& path: chunkPaths)
    {
        auto ec = std::error_code();
        std::filesystem::remove(path, ec);
    }
}

std::string downloadChunk(const Request& sourceReq,
                          const ChunkRange& range,
                          DownloadProgress& progress,
                          const std::string& destPath)
{
    auto worker = sourceReq;
    worker.parallelChunks = 1;
    worker.resumeDownload = false;
    worker.type = "GET";
    worker.progress = &progress;
    worker.headers["Range"] =
        "bytes=" + std::to_string(range.start) + "-" + std::to_string(range.end);
    preferIdentityEncoding(worker);

    auto res = downloadFile(worker, destPath);

    if (!res.error.empty())
        return res.error;

    if (res.statusCode != 206)
        return "Range request returned status " + std::to_string(res.statusCode);

    return {};
}

class ErrorRecorder
{
public:
    void record(std::string message)
    {
        auto lock = std::lock_guard(mutex);
        if (firstError.empty())
            firstError = std::move(message);
    }

    std::string take()
    {
        auto lock = std::lock_guard(mutex);
        return firstError;
    }

private:
    std::mutex mutex;
    std::string firstError;
};

std::int64_t sumReceivedBytes(const OwnedVector<DownloadProgress>& chunkProgress)
{
    auto sum = std::int64_t(0);
    for (auto i = 0; i < chunkProgress.size(); ++i)
        sum += chunkProgress[i]->bytesReceived.load();
    return sum;
}

void propagateCancelToChunks(OwnedVector<DownloadProgress>& chunkProgress)
{
    for (auto i = 0; i < chunkProgress.size(); ++i)
        chunkProgress[i]->cancel.store(true);
}

std::thread launchProgressAggregator(DownloadProgress* aggregate,
                                     OwnedVector<DownloadProgress>& chunkProgress,
                                     std::atomic<bool>& shouldStop)
{
    return std::thread(
        [aggregate, &chunkProgress, &shouldStop]
        {
            while (!shouldStop.load())
            {
                if (aggregate)
                {
                    aggregate->bytesReceived.store(sumReceivedBytes(chunkProgress));

                    if (aggregate->cancel.load())
                        propagateCancelToChunks(chunkProgress);
                }
                Time::sleepMS(25);
            }
        });
}

Vector<std::thread> launchChunkWorkers(const Request& sourceReq,
                                       const Vector<ChunkRange>& ranges,
                                       OwnedVector<DownloadProgress>& progress,
                                       const Vector<std::string>& chunkPaths,
                                       ErrorRecorder& errors)
{
    auto threads = Vector<std::thread>();
    threads.reserve(ranges.size());

    for (auto i = 0; i < ranges.size(); ++i)
    {
        threads.emplace_back(
            [&sourceReq, &ranges, &progress, &chunkPaths, &errors, i]
            {
                auto message =
                    downloadChunk(sourceReq, ranges[i], *progress[i], chunkPaths[i]);

                if (!message.empty())
                    errors.record(std::move(message));
            });
    }

    return threads;
}

std::string concatenateChunkFiles(const std::string& filePath,
                                  const Vector<std::string>& chunkPaths)
{
    auto out = std::ofstream(filePath, std::ios::binary | std::ios::trunc);

    if (!out)
        return "Failed to open destination file";

    for (const auto& path: chunkPaths)
    {
        auto in = std::ifstream(path, std::ios::binary);

        if (!in)
            return "Failed to read chunk file";

        out << in.rdbuf();
    }

    out.close();

    if (!out)
        return "Failed to write destination file";

    return {};
}

bool shouldUseSingleStreamDownload(const RangeProbe& probe, int requestedChunks)
{
    auto chunkCount = std::min(requestedChunks, kMaxParallelChunks);
    return !probe.supportsRanges || probe.totalBytes < kMinParallelTotal
           || chunkCount < 2;
}

Response makeParallelFailureResponse(const std::string& error,
                                     const Vector<std::string>& chunkPaths)
{
    removeChunkFiles(chunkPaths);
    auto response = Response();
    response.error = error;
    return response;
}
} // namespace

Response downloadFileParallel(const Request& req, const std::string& filePath)
{
    auto probe = probeRangeSupport(req);

    if (shouldUseSingleStreamDownload(probe, req.parallelChunks))
        return downloadFile(req, filePath);

    auto chunkCount = std::min(req.parallelChunks, kMaxParallelChunks);
    auto ranges = computeChunkRanges(probe.totalBytes, chunkCount);
    auto chunkPaths = makeChunkPaths(filePath, chunkCount);

    if (req.progress)
        req.progress->totalBytes.store(probe.totalBytes);

    auto chunkProgress = OwnedVector<DownloadProgress>();
    for (auto i = 0; i < chunkCount; ++i)
        chunkProgress.createNew();
    auto errors = ErrorRecorder();
    auto workers =
        launchChunkWorkers(req, ranges, chunkProgress, chunkPaths, errors);

    auto stopAggregator = std::atomic<bool>(false);
    auto aggregator =
        launchProgressAggregator(req.progress, chunkProgress, stopAggregator);

    for (auto& t: workers)
        t.join();

    stopAggregator.store(true);
    aggregator.join();

    if (req.progress)
        req.progress->bytesReceived.store(sumReceivedBytes(chunkProgress));

    auto error = errors.take();
    if (req.progress && req.progress->cancel.load() && error.empty())
        error = "cancelled";

    if (!error.empty())
        return makeParallelFailureResponse(error, chunkPaths);

    auto mergeError = concatenateChunkFiles(filePath, chunkPaths);
    removeChunkFiles(chunkPaths);

    if (!mergeError.empty())
    {
        auto response = Response();
        response.error = mergeError;
        return response;
    }

    auto response = Response();
    response.statusCode = 200;
    response.headers = probe.headers;
    return response;
}
} // namespace eacp::HTTP
