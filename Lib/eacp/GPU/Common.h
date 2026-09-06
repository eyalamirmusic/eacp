#pragma once

// Deliberately not the Graphics.h umbrella. The GPU module names geometry and
// colours everywhere and a Window nowhere, and the codegen half of it
// (eacp-gpu-codegen) has to compile where eacp-graphics is not built at all.
// The headers that do want the native tier — GPUView.h above all — include it
// themselves.
#include <eacp/Graphics/Primitives/Primitives.h>

#include <array>
#include <concepts>
