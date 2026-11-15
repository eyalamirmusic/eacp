#pragma once

#include <eacp/GPU/Shader/Dxc.h>
#include <eacp/Network/OnlineResource/OnlineResource.h>

// Dxc::Options::locateLibrary answered by the official DirectX Shader
// Compiler release, fetched once through OnlineResource and kept unpacked on
// disk. Its own target, eacp-gpu-dxc, so eacp-gpu never links eacp-network.
// Every function builds everywhere; off Windows the locator finds nothing and
// the options change nothing.
namespace eacp::GPU::Dxc
{
// The release zip and its tag, which is the version: a later release is a
// new download.
OnlineResource::Info releaseInfo();

// <per-user cache root>/eacp/DXC: one copy for every eacp app of this user.
FilePath defaultDirectory();

// The release's bin/ folder this process can load: "arm64" or "x64".
std::string architectureFolder();

FilePath binaryDirectory(const FilePath& unpackedRelease);

// Fetches the release with Freshness::trust, so a copy on disk is used without
// asking the server, and answers its bin/<arch> folder. Main thread only, as
// OnlineResource::fetch is: called anywhere else, or when the fetch fails, it
// throws, and the loader keeps the message as Status::error and compiles
// through FXC.
std::function<FilePath()>
    onlineResourceLocator(OnlineResource::Info info = releaseInfo(),
                          FilePath directory = defaultDirectory());

// setOptions({.enabled = true, .locateLibrary = onlineResourceLocator(...)}).
void enableFromOnlineResource(OnlineResource::Info info = releaseInfo(),
                              FilePath directory = defaultDirectory());
} // namespace eacp::GPU::Dxc
