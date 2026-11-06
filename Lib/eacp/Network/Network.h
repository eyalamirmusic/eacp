#pragma once

/** @file
    Networking: an HTTP client (blocking httpRequest/downloadFile, or
    HTTP::asyncRequest answering on the main thread), a small HTTP server,
    WebSocket client and server, raw TCP, OnlineResource for files fetched
    once and kept on disk, and IPC (Lock, Channel, Messenger, RpcServer)
    between this user's processes on one machine.

    Builds on every platform with no EACP_HAS_* gate: NSURLSession on Apple,
    WinHTTP on Windows, libcurl on Linux, HttpURLConnection through JNI on
    Android.

    - The plain HTTP, TCP and Channel calls block the calling thread; keep
      them off the main thread or use the async forms.
    - Every callback-based object here (WebSocket, Messenger, RpcServer,
      OnlineResource, async HTTP) delivers on the main thread and is used
      from it.
    - Servers bind to loopback unless told BindInterface::any.
*/

#include "HTTP/HttpAsync.h"
#include "HTTP/HttpProtocol.h"
#include "HTTPServer/HttpServerDispatcher.h"
#include "HTTPServer/ResponseBuilders.h"
#include "IPC/Channel.h"
#include "IPC/Lock.h"
#include "IPC/Messenger.h"
#include "OnlineResource/OnlineResource.h"
#include "OnlineResource/OnlineResources.h"
#include "TCP/Listener.h"
#include "WebSocket/Server.h"
#include "WebSocket/WebSocket.h"
