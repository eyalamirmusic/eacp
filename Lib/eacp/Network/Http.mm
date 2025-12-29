#import <Foundation/Foundation.h>
#include "Http.h"
#include "../ObjC/ObjC.h"
#include "../ObjC/AutoReleasePool.h"
#include "../ObjC/Strings.h"
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
    NSData* __strong data = nil;
    NSURLResponse* __strong response = nil;
    NSError* __strong error = nil;
};

NSURLSession* getSharedSession()
{ return [NSURLSession sharedSession]; }

SafeResult performSyncRequest(NSURLRequest* request)
{
    auto result = SafeResult();
    auto semaphore = dispatch_semaphore_create(0);

    auto cppHandler = [&result, semaphore](NSData* d, NSURLResponse* r, NSError* e)
    {
        result.data = d;
        result.response = r;
        result.error = e;

        dispatch_semaphore_signal(semaphore);
    };

    [[getSharedSession()
        dataTaskWithRequest:request
          completionHandler:^(NSData* d, NSURLResponse* r, NSError* e) {
            cppHandler(d, r, e);
          }] resume];

    // 5. Wait
    dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);

    // 6. Return
    // ARC handles the ownership transfer of the internal pointers here.
    return result;
}

Response httpRequestInternal(const Request& req)
{
    auto request = getRequest(req);
    auto raw = performSyncRequest(request);

    // 3. Handle Errors
    if (raw.error)
        throw std::runtime_error(Strings::toStdString(raw.error));

    auto response = Response();

    if ([raw.response isKindOfClass:[NSHTTPURLResponse class]])
    {
        auto httpResponse = (NSHTTPURLResponse*) raw.response;
        response.statusCode = (int) httpResponse.statusCode;
    }

    response.content = Strings::toStdString(raw.data);

    return response;
}
// --- Public Interface: Catches exceptions and returns Response struct ---
Response httpRequest(const Request& req)
{
    auto res = Response();
    auto pool = ObjC::AutoReleasePool();

    @autoreleasepool
    {
        try
        {
            res = httpRequestInternal(req);
        }
        catch (const std::exception& e)
        {
            res.error = e.what();
            res.statusCode = 0; // Or generic error code like 500
        }
    }

    return res;
}
} // namespace eacp::HTTP