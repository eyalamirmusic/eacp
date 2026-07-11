#import <Foundation/Foundation.h>
#include "Http.h"
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/Core/ObjC/RuntimeClass.h>
#include <eacp/Core/ObjC/Strings.h>
#include <eacp/Core/Threads/TaskSemaphore.h>
#include <stdexcept>

namespace eacp::HTTP
{
struct DownloadContext
{
    DownloadProgress* progress = nullptr;
    Threads::TaskSemaphore* semaphore = nullptr;
    std::string filePath;
    std::string moveError;
    ObjC::Ptr<NSURLResponse> response;
    ObjC::Ptr<NSError> error;
};

namespace
{
DownloadContext* downloadDelegateContext(id self)
{
    return (DownloadContext*) ObjC::getIvar<void*>(self, "ctx");
}

void downloadDelegateDidWriteData(id self,
                                  SEL,
                                  NSURLSession*,
                                  NSURLSessionDownloadTask* task,
                                  int64_t bytesWritten,
                                  int64_t totalBytesWritten,
                                  int64_t totalBytesExpectedToWrite)
{
    if (auto* p = downloadDelegateContext(self)->progress)
    {
        p->bytesReceived.store(totalBytesWritten);
        p->totalBytes.store(totalBytesExpectedToWrite);

        if (p->cancel.load())
            [task cancel];
    }
}

void downloadDelegateDidFinishDownloading(id self,
                                          SEL,
                                          NSURLSession*,
                                          NSURLSessionDownloadTask* task,
                                          NSURL* location)
{
    auto* ctx = downloadDelegateContext(self);

    ctx->response.reset(task.response);

    auto destURL = [NSURL fileURLWithPath:Strings::toNSString(ctx->filePath)];
    auto fm = [NSFileManager defaultManager];
    [fm removeItemAtURL:destURL error:nil];

    NSError* moveError = nil;
    if (![fm moveItemAtURL:location toURL:destURL error:&moveError])
        ctx->moveError = Strings::toStdString(moveError);
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

        builder->registerClass();
        return builder;
    }();

    return instance->get();
}
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

    for (const auto& pair: req.headers)
    {
        auto key = Strings::toNSString(pair.first);
        auto val = Strings::toNSString(pair.second);

        if (key && val)
            [request setValue:val forHTTPHeaderField:key];
    }

    if (!req.body.empty())
        request.HTTPBody = Strings::toNSData(req.body);

    return request;
}

struct SafeResult
{
    ObjC::Ptr<NSData> data;
    ObjC::Ptr<NSURLResponse> response;
    ObjC::Ptr<NSError> error;
};

NSURLSession* getSharedSession()
{
    return [NSURLSession sharedSession];
}

void copyResponseHeaders(NSHTTPURLResponse* httpResponse, Response& response)
{
    auto allHeaders = [httpResponse allHeaderFields];
    for (id key in allHeaders)
    {
        if (![key isKindOfClass:[NSString class]])
            continue;
        id value = allHeaders[key];
        if (![value isKindOfClass:[NSString class]])
            continue;
        response.headers[Strings::toStdString((NSString*) key)] =
            Strings::toStdString((NSString*) value);
    }
}

SafeResult performSyncRequest(NSURLRequest* request)
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

    [[getSharedSession()
        dataTaskWithRequest:request
          completionHandler:^(NSData* data, NSURLResponse* res, NSError* error) {
            cppHandler(data, res, error);
          }] resume];

    semaphore.wait();

    return result;
}

Response httpRequestInternal(const Request& req)
{
    auto request = getRequest(req);
    auto raw = performSyncRequest(request);

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
                              const std::string& filePath)
{
    auto request = getRequest(req);

    auto semaphore = Threads::TaskSemaphore();

    auto ctx = DownloadContext();
    ctx.progress = req.progress;
    ctx.semaphore = &semaphore;
    ctx.filePath = filePath;

    auto delegate =
        ObjC::Ptr<NSObject>([[getDownloadDelegateClass() alloc] init]);
    ObjC::getIvar<void*>(delegate.get(), "ctx") = &ctx;

    auto config = [NSURLSessionConfiguration defaultSessionConfiguration];
    auto session = ObjC::attachPtr([NSURLSession
        sessionWithConfiguration:config
                        delegate:(id<NSURLSessionDelegate>) delegate.get()
                   delegateQueue:nil]);

    auto task = [session.get() downloadTaskWithRequest:request];
    [task resume];
    semaphore.wait();
    [session.get() finishTasksAndInvalidate];

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

Response downloadFile(const Request& req,
                      const std::string& filePath)
{
    auto res = Response();
    auto pool = ObjC::AutoReleasePool();

    try
    {
        res = downloadFileInternal(req, filePath);
    }
    catch (const std::exception& e)
    {
        res.error = e.what();
        res.statusCode = 0;
    }

    if (req.progress)
        req.progress->done.store(true);

    return res;
}

} // namespace eacp::HTTP