#pragma once

// The per-platform primitives ModuleInfo.cpp decides a copy's role and product
// image from. Every one of them compares images by address, never by path.
namespace eacp::Plugins::Detail
{
// True when address lies in the process executable's image. Answers true when
// the platform cannot tell, so an unanswerable question leaves a copy
// standalone.
bool isInMainProgram(const void* address);

// True when both addresses lie in one loaded image.
bool isSameImage(const void* first, const void* second);

// The address of the eacpClaimedCopy function the process can see, or null.
// Not yet checked to be the executable's own: ModuleInfo.cpp does that.
void* findClaimFunction();
} // namespace eacp::Plugins::Detail
