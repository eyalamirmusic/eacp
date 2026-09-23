#include "Checkpoints.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/Network/OnlineResource/OnlineResource.h>

#include <cstdio>

namespace eacp::SA3Checkpoints
{
namespace
{
std::string token()
{
    if (auto fromEnvironment = Strings::trim(getEnvValue("HF_TOKEN"));
        !fromEnvironment.empty())
        return fromEnvironment;

    if (auto home = getEnvValue("HF_HOME"); !home.empty())
        if (auto fromHome =
                Strings::trim(Files::readFile(FilePath {home} / "token"));
            !fromHome.empty())
            return fromHome;

    return Strings::trim(Files::readFile(FilePath::homeDirectory() / ".cache"
                                         / "huggingface" / "token"));
}

double megabytes(std::int64_t bytes)
{
    return (double) bytes / (1024.0 * 1024.0);
}

void printProgress(const std::string& name, const OnlineResource::Progress& progress)
{
    if (progress.stage == OnlineResource::Progress::Stage::downloading)
        std::fprintf(stderr,
                     "%s: %.0f / %.0f MB\n",
                     name.c_str(),
                     megabytes(progress.bytesReceived),
                     megabytes(progress.totalBytes));
}
} // namespace

FilePath directory(const Repo& repo)
{
    auto folder = repo.id;
    folder.replace(folder.find('/'), 1, "--");

    return FilePath::appSupportDirectory("", "StableAudio3") / "Resources"
           / "huggingface" / folder / repo.revision;
}

FilePath fetch(const Repo& repo, const std::string& pathInRepo)
{
    auto options = OnlineResource::Options {};
    options.info.name = repo.id + "/" + pathInRepo;
    options.info.url = "https://huggingface.co/" + repo.id + "/resolve/"
                       + repo.revision + "/" + pathInRepo;

    if (auto bearer = token(); !bearer.empty())
        options.info.headers["Authorization"] = "Bearer " + bearer;

    options.directory = (directory(repo) / pathInRepo).parentDirectory();
    options.freshness = OnlineResource::Freshness::trust;
    options.progressInterval = Time::MS {1000};
    options.onProgress = [name = options.info.name](const auto& progress)
    { printProgress(name, progress); };

    return OnlineResource::fetch(std::move(options)).path;
}
} // namespace eacp::SA3Checkpoints
