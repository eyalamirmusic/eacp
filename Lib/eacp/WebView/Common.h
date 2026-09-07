#pragma once

// Core rather than the Graphics.h umbrella: the page bridge runs over a
// ScriptHost and links no graphics library. WebView.h includes the umbrella.
#include <eacp/Core/Core.h>
#include <Miro/Bridge.h>
#include <Miro/Reflect.h>
#include <ResEmbed/ResEmbed.h>

#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
