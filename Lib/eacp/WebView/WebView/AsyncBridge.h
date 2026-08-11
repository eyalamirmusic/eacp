#pragma once

#include "../Common.h"

#include <eacp/Network/Rpc/AsyncCommand.h>

// WebView-era spellings for the async-command glue now shared via eacp::Rpc.
namespace eacp::Graphics
{

using Rpc::CommandExecution;
using Rpc::mapJson;
using Rpc::resolveWith;
using Rpc::runCommand;
using Rpc::runOnWorkerThread;

} // namespace eacp::Graphics
