#pragma once

#include <eacp/Core/Core.h>
#include <eacp/Core/Utils/StdPath.h>
#include <NanoTest/NanoTest.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// U+00DC n U+00EF c U+00F8 d U+00E9 - U+65E5 U+672C as UTF-8: Latin-1 letters
// every Western code page has, and two CJK ones a Western code page lacks.
inline std::string nonAsciiName()
{
    const auto text = std::u8string {u8"\u00dcn\u00efc\u00f8d\u00e9-\u65e5\u672c"};
    return {text.begin(), text.end()};
}

inline std::filesystem::path scratchDirectory(const std::string& name)
{
    auto dir = std::filesystem::temp_directory_path() / ("eacp-" + name);

    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    return dir;
}

inline void writeTestFile(const std::filesystem::path& path,
                          std::string_view contents)
{
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());

    auto out = std::ofstream {path, std::ios::binary | std::ios::trunc};
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

inline eacp::Processes::ProcessOptions shellCommand(const std::string& script)
{
    auto options = eacp::Processes::ProcessOptions {};
    options.executable = "/bin/sh";
    options.arguments = {"-c", script};
    return options;
}

inline std::string readTestFile(const std::filesystem::path& path)
{
    return eacp::Files::readFile(eacp::FilePath {path});
}

inline void writeAtomically(const std::filesystem::path& path,
                            std::string_view contents)
{
    eacp::Files::writeFileAtomically(
        eacp::FilePath {path},
        eacp::Span {reinterpret_cast<const std::uint8_t*>(contents.data()),
                    (int) contents.size()});
}

inline std::vector<std::string>
    relativeNames(const std::filesystem::path& root,
                  const eacp::Vector<eacp::Files::DirectoryEntry>& entries)
{
    auto names = std::vector<std::string> {};

    for (const auto& entry: entries)
        names.push_back(
            eacp::toStdPath(entry.path).lexically_relative(root).generic_string());

    return names;
}
