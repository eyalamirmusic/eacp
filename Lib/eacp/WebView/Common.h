#pragma once

// Deliberately not the Graphics.h umbrella. The page bridge — the shim, the
// wire format, the Miro command table and the EACP_STATE binders — runs over
// a ScriptHost and links no graphics library at all. (The
// eacp::Graphics::Detail the state macros call into is a namespace, not a
// dependency: EventRegistry.h declares it.) WebView/WebView.h, the native
// view, includes the umbrella itself.
#include <eacp/Core/Core.h>
#include <Miro/Bridge.h>
#include <Miro/Reflect.h>
#include <ResEmbed/ResEmbed.h>

#include <cctype>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <unordered_map>
