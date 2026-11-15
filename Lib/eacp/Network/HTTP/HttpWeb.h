#pragma once

#include "Http.h"

#include <functional>

// The web's HTTP client underneath everything else on the web: the async
// calls and OnlineResource sit on these, and the blocking backend seam waits
// for them where the executable can suspend.
namespace eacp::HTTP
{
using WebHttpDone = std::function<void(Response)>;

// Starts the request and returns at once; done gets the response, body in
// memory, on a later turn of the main thread's loop. The request is sent as
// given: Request::perform's multipart preparation is the caller's.
void startWebRequest(const Request& req, WebHttpDone done);

// The same, downloading to filePath with Request::resumeDownload honoured as
// downloadFile does, and DownloadProgress::done set before done is called.
void startWebDownload(const Request& req,
                      const std::string& filePath,
                      WebHttpDone done);

// Whether this executable can suspend the main thread for a blocking call:
// one that links eacp-core-nested-pump (and so -sJSPI).
bool webHttpCanBlock();
} // namespace eacp::HTTP
