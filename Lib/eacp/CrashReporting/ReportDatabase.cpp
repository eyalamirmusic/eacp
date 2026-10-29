#include "ReportDatabase.h"

#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Utils/File.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Time.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <random>

namespace eacp::CrashReporting
{
namespace
{
constexpr auto pendingFolderName = "pending";
constexpr auto crashedLastRunMarkerName = "crashed-last-run";
constexpr auto sessionMarkerName = "session.active";
constexpr auto handledFolderName = "handled";
constexpr auto reportFileName = "report.json";
constexpr auto attachmentsFolderName = "attachments";

constexpr auto uuidByteCount = 16;
constexpr auto uuidVersionByte = 6;
constexpr auto uuidVariantByte = 8;
constexpr auto uuidVersionMask = 0x0F;
constexpr auto uuidVersionFour = 0x40;
constexpr auto uuidVariantMask = 0x3F;
constexpr auto uuidVariantRfc4122 = 0x80;
constexpr auto uuidDashPositions = std::to_array({4, 6, 8, 10});

Span<const std::uint8_t> getTextBytes(std::string_view text)
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    return {bytes, static_cast<int>(text.size())};
}

Vector<FilePath> listReportFolders(const FilePath& parent)
{
    auto folders = Vector<FilePath> {};

    for (const auto& entry: Files::listDirectory(parent))
        if (entry.kind == Files::EntryKind::directory)
            folders.add(entry.path);

    return folders;
}

std::int64_t getFolderAge(const FilePath& folder)
{
    return File {folder}.modificationTime();
}

void sortOldestFirst(Vector<CrashReport>& reports)
{
    std::sort(reports.begin(),
              reports.end(),
              [](const CrashReport& a, const CrashReport& b)
              { return a.timestamp < b.timestamp; });
}
} // namespace

ReportDatabase::ReportDatabase(FilePath directoryToUse)
    : directory(std::move(directoryToUse))
{
}

const FilePath& ReportDatabase::getDirectory() const
{
    return directory;
}

FilePath ReportDatabase::getPendingDirectory() const
{
    return directory / pendingFolderName;
}

FilePath ReportDatabase::getHandledDirectory() const
{
    return directory / handledFolderName;
}

FilePath ReportDatabase::getCrashedLastRunMarker() const
{
    return directory / crashedLastRunMarkerName;
}

bool ReportDatabase::markCrashedLastRun() const
{
    Files::createDirectories(directory);
    Files::writeFile(getCrashedLastRunMarker(), Span<const std::uint8_t> {});
    return hasCrashedLastRun();
}

bool ReportDatabase::hasCrashedLastRun() const
{
    return Files::exists(getCrashedLastRunMarker());
}

bool ReportDatabase::clearCrashedLastRun() const
{
    return Files::removeFile(getCrashedLastRunMarker());
}

FilePath ReportDatabase::getSessionMarker() const
{
    return directory / sessionMarkerName;
}

void ReportDatabase::writeSessionMarker(const AppSession& session) const
{
    Files::createDirectories(directory);
    Files::writeFile(getSessionMarker(), session.toJSON());
}

std::optional<AppSession> ReportDatabase::readSessionMarker() const
{
    if (!Files::isFile(getSessionMarker()))
        return std::nullopt;

    return AppSession::fromJSON(Files::readFile(getSessionMarker()));
}

bool ReportDatabase::clearSessionMarker() const
{
    return Files::removeFile(getSessionMarker());
}

FilePath ReportDatabase::getReportFolder(std::string_view id) const
{
    return getPendingDirectory() / id;
}

FilePath ReportDatabase::getReportFile(std::string_view id) const
{
    return getReportFolder(id) / reportFileName;
}

FilePath ReportDatabase::createReportFolder(std::string_view id) const
{
    auto folder = getReportFolder(id);
    Files::createDirectories(folder / attachmentsFolderName);
    return folder;
}

Vector<FilePath> ReportDatabase::copyAttachments(std::string_view id,
                                                 const Vector<FilePath>& files) const
{
    auto copies = Vector<FilePath> {};
    auto attachmentsFolder = getReportFolder(id) / attachmentsFolderName;

    for (const auto& file: files)
    {
        auto copy = attachmentsFolder / file.fileName();

        if (Files::isFile(file) && Files::copyFile(file, copy))
            copies.add(copy);
    }

    return copies;
}

bool ReportDatabase::writeReport(const CrashReport& report) const
{
    auto folder = getReportFolder(report.id);

    if (!Files::createDirectories(folder))
        return false;

    try
    {
        Files::writeFileAtomically(getReportFile(report.id),
                                   getTextBytes(report.toJSON()));
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

std::optional<CrashReport> ReportDatabase::readReport(const FilePath& folder) const
{
    auto reportFile = folder / reportFileName;

    if (!Files::isFile(reportFile))
        return std::nullopt;

    auto report = CrashReport::fromJSON(Files::readFile(reportFile));

    if (report)
        report->folder = folder;

    return report;
}

CrashReport ReportDatabase::makeIncompleteReport(const FilePath& folder) const
{
    auto report = CrashReport {};
    report.id = folder.fileName();
    report.timestamp = Time::sinceEpoch().count;
    report.reason = incompleteReason;
    report.platform = getPlatformName();
    report.capturedBy = CaptureSource::incomplete;
    report.folder = folder;

    auto minidump = folder / "minidump.dmp";

    if (Files::isFile(minidump))
        report.minidumpPath = minidump;

    return report;
}

Vector<CrashReport> ReportDatabase::readPendingReports() const
{
    auto reports = Vector<CrashReport> {};

    for (const auto& folder: listReportFolders(getPendingDirectory()))
    {
        if (auto report = readReport(folder))
        {
            reports.add(*report);
            continue;
        }

        auto incomplete = makeIncompleteReport(folder);
        writeReport(incomplete);
        reports.add(incomplete);
    }

    sortOldestFirst(reports);
    return reports;
}

bool ReportDatabase::markReportHandled(std::string_view id,
                                       int maxHandledReports) const
{
    auto from = getReportFolder(id);

    if (!Files::isDirectory(from))
        return false;

    auto to = getHandledDirectory() / id;
    Files::createDirectories(getHandledDirectory());
    Files::removeAll(to);

    if (!Files::moveFile(from, to))
        return false;

    pruneHandledReports(maxHandledReports);
    return true;
}

void ReportDatabase::pruneHandledReports(int maxHandledReports) const
{
    auto folders = listReportFolders(getHandledDirectory());

    if (folders.size() <= maxHandledReports)
        return;

    std::sort(folders.begin(),
              folders.end(),
              [](const FilePath& a, const FilePath& b)
              { return getFolderAge(a) > getFolderAge(b); });

    for (auto index = maxHandledReports; index < folders.size(); ++index)
        Files::removeAll(folders[index]);
}

std::string createReportId()
{
    auto device = std::random_device {};
    auto bytes = std::array<std::uint8_t, uuidByteCount> {};
    auto distribution = std::uniform_int_distribution<int> {0, 255};

    for (auto& byte: bytes)
        byte = static_cast<std::uint8_t>(distribution(device));

    bytes[uuidVersionByte] = static_cast<std::uint8_t>(
        (bytes[uuidVersionByte] & uuidVersionMask) | uuidVersionFour);
    bytes[uuidVariantByte] = static_cast<std::uint8_t>(
        (bytes[uuidVariantByte] & uuidVariantMask) | uuidVariantRfc4122);

    auto id = std::string {};

    for (auto index = 0; index < uuidByteCount; ++index)
    {
        if (std::find(uuidDashPositions.begin(), uuidDashPositions.end(), index)
            != uuidDashPositions.end())
            id += '-';

        auto digits = std::array<char, 3> {};
        std::snprintf(digits.data(), digits.size(), "%02x", bytes[index]);
        id += digits.data();
    }

    return id;
}

std::string getPlatformName()
{
    return std::string {Platform::name()};
}
} // namespace eacp::CrashReporting
