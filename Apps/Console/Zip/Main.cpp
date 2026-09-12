#include <eacp/Core/Core.h>

using namespace eacp;

// Builds an archive in memory, writes it beside the temp directory, reads it
// back through the file path and unpacks it: every road in and out of Zip.
namespace
{
void listEntries(const Zip::Reader& reader)
{
    for (const auto& entry: reader.entries())
        LOG("  ", entry.name, " ", entry.size, " -> ", entry.compressedSize);
}

Zip::Bytes buildArchive()
{
    auto writer = Zip::Writer {};
    writer.add("readme.txt", "Hello from eacp::Zip");

    auto noise = std::string {};

    for (auto i = 0; i < 1000; ++i)
        noise += "line " + std::to_string(i) + "\n";

    writer.add("data/lines.txt", noise);
    writer.add("data/stored.txt", "kept as-is", Zip::Level::none);

    return writer.finish();
}
} // namespace

int main()
{
    const auto bytes = buildArchive();
    LOG("archive of ", bytes.getSize(), " bytes");

    const auto inMemory = Zip::Reader {bytes};
    listEntries(inMemory);
    LOG("readme says: ", inMemory.readText("readme.txt").value_or("<missing>"));

    const auto directory = FilePath::tempDirectory() / "eacp-zip-demo";
    const auto archivePath = directory / "demo.zip";

    Files::writeFile(archivePath, bytes);
    LOG("written to ", archivePath.str());

    const auto fromDisk = Zip::Reader {archivePath};

    if (!fromDisk.isValid())
    {
        LOG("cannot open: ", fromDisk.errorMessage());
        return 1;
    }

    const auto unpacked = directory / "unpacked";
    LOG("extracting to ",
        unpacked.str(),
        ": ",
        fromDisk.extractAll(unpacked) ? "ok" : "failed");

    const auto stream = Zip::compress(Files::readFile(unpacked / "data/lines.txt"));
    LOG("lines.txt alone deflates to ", stream.getSize(), " bytes");

    return 0;
}
