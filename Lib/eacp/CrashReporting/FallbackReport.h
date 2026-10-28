#pragma once

#include "ReportDatabase.h"
#include "SharedState.h"

#include <string>

// The report the crashing app writes itself when its handler process is gone.
// Everything it needs is laid out by start(), so the crash path allocates
// nothing: report.json is written from the shared block alone, with a writer
// of fixed JSON, because Miro builds its values on the heap.
//
// It holds the id, kind, timestamp, reason, message, fault address, the
// crashing thread's id and frames (module, base and offset; no symbol or
// module UUID), metadata, tags, extra, user, platform and capturedBy "app".
// No breadcrumbs, attachments or minidump.
namespace eacp::CrashReporting::Detail
{
#if defined(_WIN32)
using CrashTimePath = std::wstring;
#else
using CrashTimePath = std::string;
#endif

void prepareFallbackReport(const ReportDatabase& database);

// Async-signal-safe. Writes pending/<id>/report.json through a temporary file
// and marks crashedLastRun. False when nothing was prepared or a write failed.
bool writeFallbackReport(const SharedState& state);

// Each platform's half: the path as its file calls take it, and the calls.
CrashTimePath makeCrashTimePath(const FilePath& path);
bool createCrashTimeDirectory(const CrashTimePath& path);
bool writeCrashTimeFile(const CrashTimePath& path, std::string_view bytes);
bool renameCrashTimeFile(const CrashTimePath& from, const CrashTimePath& to);
} // namespace eacp::CrashReporting::Detail
