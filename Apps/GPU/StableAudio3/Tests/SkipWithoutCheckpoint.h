#pragma once

#include <Checkpoints.h>

#include <NanoTest/NanoTest.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/File.h>

#include <cstdio>
#include <string>

namespace eacp::SA3Checkpoints
{
// Nothing fetches a checkpoint for a test: the suites run where the app has
// already cached one and skip, saying so, where it has not - the way the font
// tests skip a family fontconfig cannot resolve. EACP_REQUIRE_CHECKPOINTS=1
// turns the skip into a failure, for a machine that is meant to have them.
//
//     if (skipWithoutCheckpoint(SA3Checkpoints::medium, "model.safetensors"))
//         return;
inline bool skipWithoutCheckpoint(const Repo& repo, const std::string& pathInRepo)
{
    auto path = directory(repo) / pathInRepo;

    if (File {path}.isRegularFile())
        return false;

    nano::check(getEnvValue("EACP_REQUIRE_CHECKPOINTS") != "1",
                "EACP_REQUIRE_CHECKPOINTS=1 but a checkpoint is not cached");

    std::printf("skipped: checkpoint not cached (%s)\n", path.str().c_str());
    return true;
}
} // namespace eacp::SA3Checkpoints
