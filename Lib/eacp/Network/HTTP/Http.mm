#import <Foundation/Foundation.h>
#include "Http.h"
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/Core/ObjC/Strings.h>
#include <eacp/Core/Threads/TaskSemaphore.h>
#include <stdexcept>

namespace eacp::HTTP
{
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

    auto result = SafeResult();
    auto semaphore = Threads::TaskSemaphore();
    ObjC::Ptr<NSURL> tempLocation;

    auto cppHandler = [&result, &semaphore, &tempLocation](
                          NSURL* location, NSURLResponse* res,
                          NSError* error)
    {
        if (location)
            tempLocation.reset([location copy]);

        result.response.reset(res);
        result.error.reset(error);
        semaphore.signal();
    };

    auto task = [getSharedSession()
        downloadTaskWithRequest:request
              completionHandler:^(NSURL* location,
                                  NSURLResponse* res,
                                  NSError* error) {
                  cppHandler(location, res, error);
              }];

    [task resume];
    semaphore.wait();

    if (result.error)
        throw std::runtime_error(
            Strings::toStdString(result.error.get()));

    auto response = Response();

    if (result.response.isKindOfClass<NSHTTPURLResponse>())
    {
        auto httpResponse =
            (NSHTTPURLResponse*) result.response.get();
        response.statusCode = (int) httpResponse.statusCode;
    }

    if (tempLocation)
    {
        auto destURL = [NSURL
            fileURLWithPath:Strings::toNSString(filePath)];
        auto fm = [NSFileManager defaultManager];

        [fm removeItemAtURL:destURL error:nil];

        NSError* moveError = nil;
        if (![fm moveItemAtURL:tempLocation.get()
                         toURL:destURL
                         error:&moveError])
        {
            throw std::runtime_error(
                Strings::toStdString(moveError));
        }
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
        return downloadFileInternal(req, filePath);
    }
    catch (const std::exception& e)
    {
        res.error = e.what();
        res.statusCode = 0;
    }

    return res;
}

} // namespace eacp::HTTP