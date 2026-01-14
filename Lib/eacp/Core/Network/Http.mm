#import <Foundation/Foundation.h>
#include "Http.h"
#include "../ObjC/ObjC.h"
#include "../ObjC/AutoReleasePool.h"
#include "../ObjC/Strings.h"
#include "../Threads/TaskSemaphore.h"
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
        result.data = data;
        result.response = res;
        result.error = error;

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

} // namespace eacp::HTTP