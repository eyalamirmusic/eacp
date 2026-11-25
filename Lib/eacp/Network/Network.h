#pragma once

#include "HTTP/HttpAsync.h"
#include "HTTP/HttpProtocol.h"
#include "OnlineResource/OnlineResource.h"
#include "OnlineResource/OnlineResources.h"
#include "WebSocket/WebSocket.h"

// The servers, TCP and IPC open sockets of their own, which the web cannot.
#if EACP_HAS_SOCKETS
#include "HTTPServer/HttpServerDispatcher.h"
#include "HTTPServer/ResponseBuilders.h"
#include "IPC/Channel.h"
#include "IPC/Lock.h"
#include "IPC/Messenger.h"
#include "TCP/Listener.h"
#include "WebSocket/Server.h"
#endif
