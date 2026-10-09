#pragma once

#include <eacp/Core/Utils/FilePath.h>

#include <optional>
#include <string>
#include <string_view>

namespace eacp::GPU::ShaderBinaryCache
{
// Compiled shader bytes kept on disk between runs, so that a backend whose
// shader compiler eacp runs itself - FXC on D3D12, glslang on Vulkan - pays it
// once per source rather than once per launch. Metal needs none of this: the
// OS keeps its own cache of compiled libraries, and a second launch finds every
// kernel there already.
//
// An entry is found by `compiler`, which names what produced the bytes and at
// which version, and by `key`, everything the compile read - the source, the
// entry point, the target. A newer compiler or a changed source is a miss, never
// a stale binary; the whole key is stored beside the bytes and compared on the
// way back in, so two keys that hash alike are told apart too.
//
// Best effort, and invisible when it fails: an unwritable or corrupt cache is a
// compile, exactly as if the cache were not there.
std::optional<std::string> load(std::string_view compiler, std::string_view key);
void store(std::string_view compiler, std::string_view key, std::string_view bytes);

// FilePath::appCacheDirectory() / "Shaders": the app's own cache folder, which
// is also where the Vulkan backend keeps its pipeline cache.
FilePath directory();
} // namespace eacp::GPU::ShaderBinaryCache
