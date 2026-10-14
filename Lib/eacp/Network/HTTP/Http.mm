#import <Foundation/Foundation.h>
#include "Http.h"
#include "HttpProtocol.h"
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Threads/TaskSemaphore.h>
#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/Core/ObjC/RuntimeClass.h>
#include <eacp/Core/ObjC/Strings.h>

#include <fstream>
#include <memory>

namespace eacp::HTTP
{
struct TransferRules
{
    explicit TransferRules(const Request& req)
        : followRedirects(req.followRedirects)
        , maxRedirects(req.maxRedirects)
    {
        limit.maxBytes = req.maxResponseSize;
    }

    bool followRedirects = true;
    int maxRedirects = -1;
    int redirectsFollowed = 0;
    bool tooManyRedirects = false;
    ResponseSizeLimit limit;
    bool tooLarge = false;
};

struct DownloadContext
{
    explicit DownloadContext(const Request& req)
        : rules(req)
    {
    }

    TransferRules rules;
    DownloadProgress* progress = nullptr;
    Threads::TaskSemaphore* semaphore = nullptr;
    std::string filePath;
    std::int64_t resumeFrom = 0;
    std::string moveError;
    ObjC::Ptr<NSURLResponse> response;
    ObjC::Ptr<NSError> error;
};

struct DataContext
{
    explicit DataContext(const Request& req)
        : rules(req)
    {
    }

    TransferRules rules;
    Threads::TaskSemaphore* semaphore = nullptr;
    std::string body;
    ObjC::Ptr<NSURLResponse> response;
    ObjC::Ptr<NSError> error;
};

namespace
{
template <typename Context>
Context* delegateContext(id self)
{
    return (Context*) ObjC::getIvar<void*>(self, "ctx");
}

DownloadContext* downloadDelegateContext(id self)
{
    return delegateContext<DownloadContext>(self);
}

DataContext* dataDelegateContext(id self)
{
    return delegateContext<DataContext>(self);
}

bool exceedsRedirectLimit(TransferRules& rules)
{
    if (rules.maxRedirects < 0)
        return false;

    ++rules.redirectsFollowed;
    return rules.redirectsFollowed > rules.maxRedirects;
}

template <typename Context>
void delegateWillRedirect(id self,
                          SEL,
                          NSURLSession*,
                          NSURLSessionTask* task,
                          NSHTTPURLResponse*,
                          NSURLRequest* newRequest,
                          void (^completionHandler)(NSURLRequest*))
{
    auto& rules = delegateContext<Context>(self)->rules;

    if (!rules.followRedirects)
    {
        completionHandler(nil);
        return;
    }

    if (exceedsRedirectLimit(rules))
    {
        rules.tooManyRedirects = true;
        [task cancel];
        completionHandler(nil);
        return;
    }

    completionHandler(newRequest);
}

void readResponseHeaders(NSHTTPURLResponse* httpResponse,
                         std::map<std::string, std::string>& headers)
{
    auto allHeaders = [httpResponse allHeaderFields];
    for (id key in allHeaders)
    {
        if (![key isKindOfClass:[NSString class]])
            continue;
        id value = allHeaders[key];
        if (![value isKindOfClass:[NSString class]])
            continue;
        headers[Strings::toStdString((NSString*) key)] =
            Strings::toStdString((NSString*) value);
    }
}

DownloadWrite downloadWriteForResponse(NSURLResponse* response,
                                       std::int64_t resumeFrom)
{
    if (resumeFrom <= 0)
        return DownloadWrite::Replace;

    if (![response isKindOfClass:[NSHTTPURLResponse class]])
        return DownloadWrite::Keep;

    auto* httpResponse = (NSHTTPURLResponse*) response;
    auto headers = std::map<std::string, std::string>();
    readResponseHeaders(httpResponse, headers);

    return downloadWriteFor((int) httpResponse.statusCode, headers, resumeFrom);
}

void reportDownloadProgress(DownloadContext& ctx,
                            NSURLSessionTask* task,
                            int64_t totalBytesWritten,
                            int64_t totalBytesExpectedToWrite)
{
    auto* progress = ctx.progress;

    if (progress == nullptr)
        return;

    auto write = downloadWriteForResponse(task.response, ctx.resumeFrom);

    if (write != DownloadWrite::Keep)
    {
        auto offset = write == DownloadWrite::Append ? ctx.resumeFrom : 0;
        auto expected = totalBytesExpectedToWrite;

        progress->bytesReceived.store(offset + totalBytesWritten);
        progress->totalBytes.store(expected >= 0 ? offset + expected : -1);
    }

    if (progress->cancel.load())
        [task cancel];
}

void downloadDelegateDidWriteData(id self,
                                  SEL,
                                  NSURLSession*,
                                  NSURLSessionDownloadTask* task,
                                  int64_t /*bytesWritten*/,
                                  int64_t totalBytesWritten,
                                  int64_t totalBytesExpectedToWrite)
{
    auto* ctx = downloadDelegateContext(self);
    auto& rules = ctx->rules;

    if (rules.limit.exceeds(totalBytesWritten)
        || rules.limit.exceeds(totalBytesExpectedToWrite))
    {
        rules.tooLarge = true;
        [task cancel];
        return;
    }

    reportDownloadProgress(
        *ctx, task, totalBytesWritten, totalBytesExpectedToWrite);
}

std::string replaceFile(NSURL* source, const std::string& filePath)
{
    auto destURL = [NSURL fileURLWithPath:Strings::toNSString(filePath)];
    auto fm = [NSFileManager defaultManager];
    [fm removeItemAtURL:destURL error:nil];

    NSError* moveError = nil;
    if (![fm moveItemAtURL:source toURL:destURL error:&moveError])
        return Strings::toStdString(moveError);

    return {};
}

std::string appendFile(NSURL* source, const std::string& filePath)
{
    auto in = std::ifstream(source.fileSystemRepresentation, std::ios::binary);
    auto out = std::ofstream(filePath, std::ios::binary | std::ios::app);

    if (!in || !out)
        return "Failed to open destination file";

    // Inserting an empty stream buffer sets failbit, so an empty body is
    // checked for rather than appended.
    if (in.peek() != std::ifstream::traits_type::eof())
        out << in.rdbuf();

    out.close();

    if (!out)
        return "Failed to write destination file";

    return {};
}

void downloadDelegateDidFinishDownloading(id self,
                                          SEL,
                                          NSURLSession*,
                                          NSURLSessionDownloadTask* task,
                                          NSURL* location)
{
    auto* ctx = downloadDelegateContext(self);

    if (ctx->rules.tooLarge || ctx->rules.tooManyRedirects)
        return;

    ctx->response.reset(task.response);

    switch (downloadWriteForResponse(task.response, ctx->resumeFrom))
    {
        case DownloadWrite::Replace:
            ctx->moveError = replaceFile(location, ctx->filePath);
            break;
        case DownloadWrite::Append:
            ctx->moveError = appendFile(location, ctx->filePath);
            break;
        case DownloadWrite::Keep:
            break;
    }
}

void downloadDelegateDidComplete(
    id self, SEL, NSURLSession*, NSURLSessionTask* task, NSError* error)
{
    auto* ctx = downloadDelegateContext(self);

    if (error)
        ctx->error.reset(error);

    if (!ctx->response)
        ctx->response.reset(task.response);

    ctx->semaphore->signal();
}

Class getDownloadDelegateClass()
{
    static auto instance = []
    {
        auto builder = new ObjC::RuntimeClass<NSObject>("EacpDownloadDelegate");

        builder->addIvar<void*>("ctx");
        builder->addProtocol(@protocol(NSURLSessionDownloadDelegate));

        builder->addMethod(@selector(URLSession:
                                       downloadTask:didWriteData:totalBytesWritten
                                                   :totalBytesExpectedToWrite:),
                           downloadDelegateDidWriteData);
        builder->addMethod(
            @selector(URLSession:downloadTask:didFinishDownloadingToURL:),
            downloadDelegateDidFinishDownloading);
        builder->addMethod(@selector(URLSession:task:didCompleteWithError:),
                           downloadDelegateDidComplete);
        builder->addMethod(
            @selector(URLSession:
                            task:willPerformHTTPRedirection:newRequest
                                :completionHandler:),
            delegateWillRedirect<DownloadContext>);

        builder->registerClass();
        return builder;
    }();

    return instance->get();
}

void dataDelegateDidReceiveResponse(
    id self,
    SEL,
    NSURLSession*,
    NSURLSessionDataTask*,
    NSURLResponse* response,
    void (^completionHandler)(NSURLSessionResponseDisposition))
{
    auto& rules = dataDelegateContext(self)->rules;
    auto expected = response.expectedContentLength;

    if (expected != NSURLResponseUnknownLength && rules.limit.exceeds(expected))
    {
        rules.tooLarge = true;
        completionHandler(NSURLSessionResponseCancel);
        return;
    }

    completionHandler(NSURLSessionResponseAllow);
}

void dataDelegateDidReceiveData(
    id self, SEL, NSURLSession*, NSURLSessionDataTask* task, NSData* data)
{
    auto* ctx = dataDelegateContext(self);

    if (ctx->rules.tooLarge)
        return;

    if (!ctx->rules.limit.add((int64_t) data.length))
    {
        ctx->rules.tooLarge = true;
        [task cancel];
        return;
    }

    auto* body = &ctx->body;

    [data enumerateByteRangesUsingBlock:^(
              const void* bytes, NSRange range, BOOL*) {
      body->append((const char*) bytes, range.length);
    }];
}

void dataDelegateDidComplete(
    id self, SEL, NSURLSession*, NSURLSessionTask* task, NSError* error)
{
    auto* ctx = dataDelegateContext(self);

    if (error)
        ctx->error.reset(error);

    ctx->response.reset(task.response);
    ctx->semaphore->signal();
}

Class getDataDelegateClass()
{
    static auto instance = []
    {
        auto builder = new ObjC::RuntimeClass<NSObject>("EacpDataDelegate");

        builder->addIvar<void*>("ctx");
        builder->addProtocol(@protocol(NSURLSessionDataDelegate));

        builder->addMethod(
            @selector(URLSession:dataTask:didReceiveResponse:completionHandler:),
            dataDelegateDidReceiveResponse);
        builder->addMethod(@selector(URLSession:dataTask:didReceiveData:),
                           dataDelegateDidReceiveData);
        builder->addMethod(@selector(URLSession:task:didCompleteWithError:),
                           dataDelegateDidComplete);
        builder->addMethod(
            @selector(URLSession:
                            task:willPerformHTTPRedirection:newRequest
                                :completionHandler:),
            delegateWillRedirect<DataContext>);

        builder->registerClass();
        return builder;
    }();

    return instance->get();
}

double toSeconds(Time::MS timeout)
{
    return (double) timeout.count / 1000.0;
}

NSURLRequestCachePolicy nativeCachePolicy(const Request& req)
{
    if (req.cachePolicy == CachePolicy::UseLocalCache)
        return NSURLRequestUseProtocolCachePolicy;

    return NSURLRequestReloadIgnoringLocalCacheData;
}

// The whole-request limit is the configuration's resource timeout; the idle
// one is its request timeout, which NSURLSession resets whenever data
// arrives.
void applyTimeouts(NSURLSessionConfiguration* config, const Request& req)
{
    auto idle = idleLimit(req);

    if (idle.count > 0)
        config.timeoutIntervalForRequest = toSeconds(idle);

    if (req.timeout.count > 0)
        config.timeoutIntervalForResource = toSeconds(req.timeout);
}

void applyCachePolicy(NSURLSessionConfiguration* config, const Request& req)
{
    config.requestCachePolicy = nativeCachePolicy(req);

    if (req.cachePolicy == CachePolicy::IgnoreLocalCache)
        config.URLCache = nil;
}

NSURLSessionConfiguration* makeConfiguration(const Request& req)
{
    auto config = [NSURLSessionConfiguration defaultSessionConfiguration];
    applyTimeouts(config, req);
    applyCachePolicy(config, req);
    return config;
}

// NSURLSession reports no connect event, so the nearest thing to one stands
// in for it: a task that has neither received a response nor sent a byte of
// its body by the deadline is cancelled.
class ConnectDeadline
{
public:
    void arm(NSURLSessionTask* task, Time::MS limit)
    {
        if (limit.count <= 0)
            return;

        auto flag = fired;
        auto delay = dispatch_time(DISPATCH_TIME_NOW,
                                   (int64_t) limit.count * (int64_t) NSEC_PER_MSEC);
        auto queue = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);

        dispatch_after(delay, queue, ^{
          if (!isStillConnecting(task))
              return;

          flag->store(true);
          [task cancel];
        });
    }

    // The task can finish between the deadline's check and its cancel, so it
    // only counts as a timeout when the task did end cancelled.
    void throwIfFired(NSError* error) const
    {
        if (fired->load() && wasCancelled(error))
            throw std::runtime_error(timedOutError());
    }

private:
    static bool wasCancelled(NSError* error)
    {
        return error != nil && [error.domain isEqualToString:NSURLErrorDomain]
               && error.code == NSURLErrorCancelled;
    }

    static bool isStillConnecting(NSURLSessionTask* task)
    {
        return task.state == NSURLSessionTaskStateRunning && task.response == nil
               && task.countOfBytesSent == 0;
    }

    std::shared_ptr<std::atomic<bool>> fired =
        std::make_shared<std::atomic<bool>>(false);
};
} // namespace

NSMutableURLRequest* getRequest(const Request& req)
{
    if (req.url.empty())
        throw std::invalid_argument("URL cannot be empty");

    auto urlString = Strings::toNSString(req.url);

    if (!urlString)
        throw std::runtime_error("URL contains invalid UTF-8 characters");

    auto url = [NSURL URLWithString:urlString];

    if (!url)
        throw std::runtime_error("Malformed URL format");

    auto request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = Strings::toNSString(req.type);
    request.cachePolicy = nativeCachePolicy(req);

    if (auto agent = userAgentToAdd(req); !agent.empty())
        [request setValue:Strings::toNSString(agent)
            forHTTPHeaderField:@"User-Agent"];

    for (const auto& pair: req.headers)
    {
        auto key = Strings::toNSString(pair.first);
        auto val = Strings::toNSString(pair.second);

        if (key && val)
            [request setValue:val forHTTPHeaderField:key];
    }

    if (!req.body.empty())
        request.HTTPBody = Strings::toNSData(req.body);

    auto idle = idleLimit(req);

    if (idle.count > 0)
        request.timeoutInterval = toSeconds(idle);

    return request;
}

struct SafeResult
{
    ObjC::Ptr<NSData> data;
    ObjC::Ptr<NSURLResponse> response;
    ObjC::Ptr<NSError> error;
};

// sharedSession writes what it fetches into the shared NSURLCache whatever a
// request's policy, so a request that ignores the cache shares a session of
// eacp's own that has none instead - still one session for the process, so
// its connections are reused as before.
NSURLSession* getUncachedSession()
{
    static auto session = []
    {
        auto config = [NSURLSessionConfiguration defaultSessionConfiguration];
        config.URLCache = nil;
        config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        return ObjC::attachPtr([NSURLSession sessionWithConfiguration:config]);
    }();

    return session.get();
}

NSURLSession* getSharedSession(const Request& req)
{
    if (req.cachePolicy == CachePolicy::UseLocalCache)
        return [NSURLSession sharedSession];

    return getUncachedSession();
}

// A timed request needs its own session: timeoutIntervalForResource - the
// only setting that bounds the whole transfer rather than the gaps between
// packets - lives on the configuration, and the shared session's is
// process-wide. Invalidated once the task has finished, so it is not leaked.
class SessionForRequest
{
public:
    explicit SessionForRequest(const Request& req)
    {
        if (req.timeout.count <= 0)
        {
            session = getSharedSession(req);
            return;
        }

        session = [NSURLSession sessionWithConfiguration:makeConfiguration(req)];
        owned = true;
    }

    ~SessionForRequest()
    {
        if (owned)
            [session finishTasksAndInvalidate];
    }

    SessionForRequest(const SessionForRequest&) = delete;
    SessionForRequest& operator=(const SessionForRequest&) = delete;

    NSURLSession* get() const { return session; }

private:
    NSURLSession* session = nil;
    bool owned = false;
};

void copyResponseHeaders(NSHTTPURLResponse* httpResponse, Response& response)
{
    readResponseHeaders(httpResponse, response.headers);
}

SafeResult performSyncRequest(NSURLRequest* request,
                              NSURLSession* session,
                              const Request& req)
{
    auto result = SafeResult();

    auto semaphore = Threads::TaskSemaphore();

    auto cppHandler =
        [&result, &semaphore](NSData* data, NSURLResponse* res, NSError* error)
    {
        result.data.reset(data);
        result.response.reset(res);
        result.error.reset(error);

        semaphore.signal();
    };

    auto task = [session
        dataTaskWithRequest:request
          completionHandler:^(NSData* data, NSURLResponse* res, NSError* error) {
            cppHandler(data, res, error);
          }];

    auto connectDeadline = ConnectDeadline();
    [task resume];
    connectDeadline.arm(task, req.connectTimeout);

    semaphore.wait();
    connectDeadline.throwIfFired(result.error.get());

    return result;
}

SafeResult performUndelegatedRequest(NSURLRequest* request, const Request& req)
{
    auto session = SessionForRequest(req);
    return performSyncRequest(request, session.get(), req);
}

bool needsDataDelegate(const Request& req)
{
    return !req.followRedirects || req.maxResponseSize > 0 || req.maxRedirects >= 0;
}

void throwIfRuleBroken(const TransferRules& rules)
{
    if (rules.tooLarge)
        throw std::runtime_error(responseTooLargeError(rules.limit.maxBytes));

    if (rules.tooManyRedirects)
        throw std::runtime_error(tooManyRedirectsError(rules.maxRedirects));
}

SafeResult performDelegatedRequest(NSURLRequest* request, const Request& req)
{
    auto semaphore = Threads::TaskSemaphore();

    auto ctx = DataContext(req);
    ctx.semaphore = &semaphore;

    auto delegate = ObjC::Ptr<NSObject>([[getDataDelegateClass() alloc] init]);
    ObjC::getIvar<void*>(delegate.get(), "ctx") = &ctx;

    auto session = ObjC::attachPtr([NSURLSession
        sessionWithConfiguration:makeConfiguration(req)
                        delegate:(id<NSURLSessionDelegate>) delegate.get()
                   delegateQueue:nil]);

    auto task = [session.get() dataTaskWithRequest:request];
    auto connectDeadline = ConnectDeadline();
    [task resume];
    connectDeadline.arm(task, req.connectTimeout);

    semaphore.wait();
    [session.get() finishTasksAndInvalidate];

    connectDeadline.throwIfFired(ctx.error.get());
    throwIfRuleBroken(ctx.rules);

    auto result = SafeResult();
    result.data.reset(Strings::toNSData(ctx.body));
    result.response = ctx.response;
    result.error = ctx.error;
    return result;
}

Response httpRequestInternal(const Request& req)
{
    auto request = getRequest(req);
    auto raw = needsDataDelegate(req) ? performDelegatedRequest(request, req)
                                      : performUndelegatedRequest(request, req);

    if (raw.error)
        throw std::runtime_error(Strings::toStdString(raw.error.get()));

    auto response = Response();

    if (raw.response.isKindOfClass<NSHTTPURLResponse>())
    {
        auto httpResponse = (NSHTTPURLResponse*) raw.response.get();
        response.statusCode = (int) httpResponse.statusCode;
        copyResponseHeaders(httpResponse, response);
    }

    response.content = Strings::toStdString(raw.data.get());

    return response;
}

Response httpRequest(const Request& req)
{
    auto res = Response();
    auto pool = ObjC::AutoReleasePool();

    try
    {
        return httpRequestInternal(req);
    }
    catch (const std::exception& e)
    {
        res.error = e.what();
        res.statusCode = 0;
    }

    return res;
}

Response downloadFileInternal(const Request& req,
                              const std::string& filePath,
                              std::int64_t resumeFrom)
{
    auto request = getRequest(req);

    auto semaphore = Threads::TaskSemaphore();

    auto ctx = DownloadContext(req);
    ctx.progress = req.progress;
    ctx.semaphore = &semaphore;
    ctx.filePath = filePath;
    ctx.resumeFrom = resumeFrom;

    auto delegate =
        ObjC::Ptr<NSObject>([[getDownloadDelegateClass() alloc] init]);
    ObjC::getIvar<void*>(delegate.get(), "ctx") = &ctx;

    auto session = ObjC::attachPtr([NSURLSession
        sessionWithConfiguration:makeConfiguration(req)
                        delegate:(id<NSURLSessionDelegate>) delegate.get()
                   delegateQueue:nil]);

    auto task = [session.get() downloadTaskWithRequest:request];
    auto connectDeadline = ConnectDeadline();
    [task resume];
    connectDeadline.arm(task, req.connectTimeout);

    semaphore.wait();
    [session.get() finishTasksAndInvalidate];

    connectDeadline.throwIfFired(ctx.error.get());
    throwIfRuleBroken(ctx.rules);

    if (ctx.error)
        throw std::runtime_error(Strings::toStdString(ctx.error.get()));

    if (!ctx.moveError.empty())
        throw std::runtime_error(ctx.moveError);

    auto response = Response();

    if (ctx.response.isKindOfClass<NSHTTPURLResponse>())
    {
        auto httpResponse = (NSHTTPURLResponse*) ctx.response.get();
        response.statusCode = (int) httpResponse.statusCode;
        copyResponseHeaders(httpResponse, response);
    }

    return response;
}

Response downloadFileFrom(const Request& req,
                          const std::string& filePath,
                          std::int64_t resumeFrom)
{
    auto res = Response();
    auto pool = ObjC::AutoReleasePool();

    try
    {
        res = downloadFileInternal(req, filePath, resumeFrom);
    }
    catch (const std::exception& e)
    {
        res.error = e.what();
        res.statusCode = 0;
    }

    return res;
}

} // namespace eacp::HTTP
