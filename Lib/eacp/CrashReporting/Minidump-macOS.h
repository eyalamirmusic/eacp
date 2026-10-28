#pragma once

#include "TaskCapture-macOS.h"

#include <eacp/Core/Utils/FilePath.h>

#include <cstdint>
#include <string>

// A Breakpad-format minidump of a crashed macOS app: the system, every
// thread's registers and stack, every image with its UUID as the CodeView
// record, and the exception. Enough for minidump-stackwalk and lldb to open
// the crash. The format is Microsoft's (MINIDUMP_HEADER, MINIDUMP_DIRECTORY
// and the MINIDUMP_*_STREAM structs, learn.microsoft.com, minidumpapiset.h)
// with Breakpad's Mac additions, all in google/breakpad
// src/google_breakpad/common/minidump_format.h; no library is linked, the
// file is written field by field in Minidump-macOS.cpp.
namespace eacp::CrashReporting::Detail
{
struct MinidumpThread
{
    std::uint64_t threadId = 0;
    TaskThreadRegisters registers;
    MemoryRange stack;
    Vector<std::byte> stackBytes;
};

struct MinidumpContents
{
    exception_type_t exceptionType = 0;
    std::array<std::int64_t, 2> exceptionCodes {};
    std::uint64_t exceptionAddress = 0;

    // The crashing thread first; the exception stream points at it.
    Vector<MinidumpThread> threads;
    Vector<AppImage> images;
    std::string operatingSystemVersion;
    std::string operatingSystemBuild;
    std::uint32_t processorCount = 0;
};

bool writeMinidump(const FilePath& path, const MinidumpContents& contents);
} // namespace eacp::CrashReporting::Detail
