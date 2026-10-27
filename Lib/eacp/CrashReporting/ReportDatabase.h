#pragma once

#include "CrashReport.h"

namespace eacp::CrashReporting
{
// The folders reports live in:
//
//   <directory>/pending/<id>/report.json, minidump.dmp, attachments/
//   <directory>/handled/<id>/...
//   <directory>/crashed-last-run
//
// report.json is written last, so a folder without one is a capture that did
// not finish.
class ReportDatabase
{
public:
    explicit ReportDatabase(FilePath directoryToUse);

    const FilePath& getDirectory() const;
    FilePath getPendingDirectory() const;
    FilePath getHandledDirectory() const;
    FilePath getReportFolder(std::string_view id) const;

    // A marker the handler leaves beside the folders when it writes a report,
    // so the next launch can tell its last run crashed; cleared by hand.
    FilePath getCrashedLastRunMarker() const;
    bool markCrashedLastRun() const;
    bool hasCrashedLastRun() const;
    bool clearCrashedLastRun() const;

    // Makes pending/<id>/ and its attachments/ folder.
    FilePath createReportFolder(std::string_view id) const;

    // Copies each file into the report's attachments/ and returns the copies.
    Vector<FilePath> copyAttachments(std::string_view id,
                                     const Vector<FilePath>& files) const;

    bool writeReport(const CrashReport& report) const;

    // Every folder in pending/, oldest first. A folder with no readable
    // report.json comes back as an incomplete report, which is written to it so
    // the next read agrees.
    Vector<CrashReport> readPendingReports() const;

    std::optional<CrashReport> readReport(const FilePath& folder) const;

    // Moves pending/<id>/ to handled/<id>/ and keeps the newest
    // maxHandledReports of handled/.
    bool markReportHandled(std::string_view id, int maxHandledReports) const;
    void pruneHandledReports(int maxHandledReports) const;

private:
    CrashReport makeIncompleteReport(const FilePath& folder) const;

    FilePath directory;
};

// A random version 4 UUID, lower case with dashes.
std::string createReportId();

std::int64_t getMillisecondsSinceEpoch();

// The platform name a report carries, from Platform::name().
std::string getPlatformName();
} // namespace eacp::CrashReporting
