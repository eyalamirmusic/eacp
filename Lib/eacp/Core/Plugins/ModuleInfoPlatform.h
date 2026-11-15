#pragma once

#include "../Utils/FilePath.h"

namespace eacp::Plugins::Detail
{
// True when the platform cannot tell, so the copy stays standalone.
bool isInMainProgram(const void* address);

bool isSameImage(const void* first, const void* second);

// The eacpClaimedCopy the process can see, from whichever image defines it.
void* findClaimFunction();

#ifndef _WIN32
// The path dladdr reports for this copy's image, which for glibc's main
// program is argv[0].
FilePath loadedImagePath();
#endif
} // namespace eacp::Plugins::Detail
