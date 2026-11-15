#include "DxcResource.h"

#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Threads/ThreadUtils.h>

#include <stdexcept>

namespace eacp::GPU::Dxc
{
namespace
{
constexpr auto dxcReleaseTag = "v1.9.2609";
constexpr auto dxcReleaseUrl = "https://github.com/microsoft/DirectXShaderCompiler/"
                               "releases/download/v1.9.2609/dxc_2026_09_29.zip";

FilePath fetchRelease(const OnlineResource::Info& info, const FilePath& directory)
{
    if (!Threads::isMainThread())
        throw std::runtime_error {
            "the DXC release is fetched through OnlineResource, which runs on the "
            "main thread only, and the first shader was compiled on another; call "
            "Dxc::load() on the main thread before building shaders elsewhere"};

    auto result =
        OnlineResource::fetch({.info = info,
                               .directory = directory,
                               .freshness = OnlineResource::Freshness::trust});

    return binaryDirectory(result.path);
}
} // namespace

OnlineResource::Info releaseInfo()
{
    return {.name = "DirectX Shader Compiler",
            .url = dxcReleaseUrl,
            .version = dxcReleaseTag};
}

FilePath defaultDirectory()
{
    return FilePath::appCacheDirectory("eacp", "DXC");
}

std::string architectureFolder()
{
    return std::string {Platform::architectureName()};
}

FilePath binaryDirectory(const FilePath& unpackedRelease)
{
    return unpackedRelease / "bin" / architectureFolder();
}

std::function<FilePath()> onlineResourceLocator(OnlineResource::Info info,
                                                FilePath directory)
{
    if constexpr (Platform::isWindows())
        return [release = std::move(info), folder = std::move(directory)]
        { return fetchRelease(release, folder); };
    else
        return [] { return FilePath {}; };
}

void enableFromOnlineResource(OnlineResource::Info info, FilePath directory)
{
    setOptions({.enabled = true,
                .locateLibrary =
                    onlineResourceLocator(std::move(info), std::move(directory))});
}
} // namespace eacp::GPU::Dxc
