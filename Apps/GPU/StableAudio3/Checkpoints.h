#pragma once

#include <eacp/Core/Utils/FilePath.h>

#include <string>

namespace eacp::SA3Checkpoints
{
struct Repo
{
    std::string id;
    std::string revision;
};

inline const auto smallMusic = Repo {"stabilityai/stable-audio-3-small-music",
                                     "0fef1392cd842149a2b6d445e181c97608faac06"};

inline const auto medium = Repo {"stabilityai/stable-audio-3-medium",
                                 "27b5a21b791b1b033d193a9e1e3ce78493f102f9"};

inline const auto sameL =
    Repo {"stabilityai/SAME-L", "41acf79dd242877d6499a1108ca5dba5d5eecfc5"};

// Where a repo's files live once fetched, shared by the app and the tests:
// <app support>/StableAudio3/Resources/huggingface/<owner>--<name>/<revision>
FilePath directory(const Repo& repo);

// Blocking; prints progress to stderr. Throws when the file could not be had.
FilePath fetch(const Repo& repo, const std::string& pathInRepo);
} // namespace eacp::SA3Checkpoints
