#include <eacp/WebView/WebView.h>

#include <NanoTest/NanoTest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

using namespace nano;
using namespace eacp::Graphics;

namespace
{
std::filesystem::path writeTempFile(const std::filesystem::path& dir,
                                    const std::string& name,
                                    const std::string& contents)
{
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    auto out = std::ofstream {path, std::ios::binary};
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return path;
}

std::string fileURL(const std::filesystem::path& path)
{
    // Absolute path already starts with '/', so this yields audiofile:///...
    return "audiofile://" + path.string();
}
} // namespace

auto tFileURLToPath = test("FileProvider/fileURLToPath") = []
{
    check(fileURLToPath("audiofile:///abs/path.wav") == "/abs/path.wav");
    check(fileURLToPath("audiofile:///a/b%20c.wav") == "/a/b c.wav");
    check(fileURLToPath("audiofile:///x.wav?q=1#frag") == "/x.wav");
    check(fileURLToPath("not-a-url").empty());
};

auto tMimeCaseInsensitive = test("FileProvider/mimeForPathCaseInsensitive") = []
{
    check(mimeForPath("song.MP3") == "audio/mpeg");
    check(mimeForPath("a/b/clip.WAV") == "audio/wav");
    check(mimeForPath("art.PNG") == "image/png");
    check(mimeForPath("data.unknownext") == "application/octet-stream");
};

auto tServesWithinRoot = test("FileProvider/servesFileWithinRoot") = []
{
    auto dir = std::filesystem::temp_directory_path() / "eacp-fsp";
    auto path = writeTempFile(dir, "clip.wav", "0123456789");

    auto provider = fileStreamProvider({dir.string()});
    auto resource = provider(fileURL(path));

    check(resource.has_value());
    check(resource->size == 10);
    check(resource->mimeType == "audio/wav");

    auto buffer = std::array<std::uint8_t, 4> {};
    check(resource->read(2, buffer) == 4);
    check(buffer[0] == '2' && buffer[3] == '5');

    std::filesystem::remove(path);
};

auto tRejectsOutsideRoot = test("FileProvider/rejectsFileOutsideRoot") = []
{
    auto allowed = std::filesystem::temp_directory_path() / "eacp-fsp-allowed";
    std::filesystem::create_directories(allowed);

    auto outside = writeTempFile(
        std::filesystem::temp_directory_path(), "eacp-fsp-outside.wav", "x");

    auto provider = fileStreamProvider({allowed.string()});
    check(!provider(fileURL(outside)).has_value());

    std::filesystem::remove(outside);
};

auto tMissingFileIsNotServed = test("FileProvider/missingFile") = []
{
    auto dir = std::filesystem::temp_directory_path() / "eacp-fsp";
    std::filesystem::create_directories(dir);

    auto provider = fileStreamProvider({dir.string()});
    check(!provider(fileURL(dir / "does-not-exist.wav")).has_value());
};
