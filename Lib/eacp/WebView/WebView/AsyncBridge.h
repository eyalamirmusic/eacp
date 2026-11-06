#pragma once

/** @file WebView-era aliases for the async command helpers now in eacp::Rpc. */

#include "../Common.h"

#include <eacp/Network/Rpc/AsyncCommand.h>

/// The async-command glue moved to eacp::Rpc when the IPC bridge started
/// sharing it; these usings keep the WebView-era spellings working.
namespace eacp::Graphics
{

using Rpc::CommandExecution;
using Rpc::mapJson;
using Rpc::resolveWith;
using Rpc::runCommand;
using Rpc::runOnWorkerThread;

} // namespace eacp::Graphics
