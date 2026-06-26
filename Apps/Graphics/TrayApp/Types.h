#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <Miro/Miro.h>

#include <string>

struct SubmitPromptRequest
{
    std::string text;

    MIRO_REFLECT(text)
};

struct SearchDownloadsRequest
{
    std::string query;

    MIRO_REFLECT(query)
};

struct DownloadResult
{
    std::string name;
    std::string path;
    std::string kind;
    long long size = 0;
    int score = 0;

    MIRO_REFLECT(name, path, kind, size, score)
};

struct SearchDownloadsResponse
{
    EA::Vector<DownloadResult> results;

    MIRO_REFLECT(results)
};

struct ArmDragRequest
{
    EA::Vector<std::string> paths;

    MIRO_REFLECT(paths)
};

struct CopyFilesRequest
{
    EA::Vector<std::string> paths;

    MIRO_REFLECT(paths)
};

struct CopyFilesResponse
{
    bool copied = false;

    MIRO_REFLECT(copied)
};

struct ShowCopyToastRequest
{
    std::string sampleName;

    MIRO_REFLECT(sampleName)
};

struct PlayAudioRequest
{
    std::string path;

    MIRO_REFLECT(path)
};

struct PlaybackState
{
    bool playing = false;
    std::string path;

    MIRO_REFLECT(playing, path)
};
