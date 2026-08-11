#include "Mp4Demuxer.h"

#include "BoxReader.h"

namespace eacp::Video
{
namespace
{
using namespace Mp4;

constexpr auto boxFtyp = fourcc("ftyp");
constexpr auto boxMoov = fourcc("moov");
constexpr auto boxTrak = fourcc("trak");
constexpr auto boxMdia = fourcc("mdia");
constexpr auto boxMdhd = fourcc("mdhd");
constexpr auto boxHdlr = fourcc("hdlr");
constexpr auto boxMinf = fourcc("minf");
constexpr auto boxStbl = fourcc("stbl");
constexpr auto boxStsd = fourcc("stsd");
constexpr auto boxStts = fourcc("stts");
constexpr auto boxCtts = fourcc("ctts");
constexpr auto boxStsc = fourcc("stsc");
constexpr auto boxStco = fourcc("stco");
constexpr auto boxCo64 = fourcc("co64");
constexpr auto boxStsz = fourcc("stsz");
constexpr auto boxStss = fourcc("stss");

// A SampleEntry header plus the fixed VisualSampleEntry fields; the codec
// configuration boxes start here.
constexpr auto visualSampleEntrySize = std::size_t {78};

// Far beyond any real file; stops a hostile count sizing a table in gigabytes.
constexpr auto maxTableEntries = std::uint64_t {1} << 30;

struct Mp4SttsEntry
{
    std::uint32_t count = 0;
    std::uint32_t delta = 0;
};

struct Mp4CttsEntry
{
    std::uint32_t count = 0;
    std::int64_t offset = 0;
};

struct Mp4StscEntry
{
    std::uint32_t firstChunk = 0;
    std::uint32_t samplesPerChunk = 0;
};

// Everything read out of one trak's stbl before it is resolved into samples.
struct Mp4TrackTables
{
    Vector<Mp4SttsEntry> stts;
    Vector<Mp4CttsEntry> ctts;
    Vector<Mp4StscEntry> stsc;
    Vector<std::uint64_t> chunkOffsets;
    Vector<std::uint32_t> sampleSizes;
    Vector<std::uint32_t> syncSamples;
    std::uint32_t constantSampleSize = 0;
    std::uint32_t sampleCount = 0;
    bool hasCtts = false;
    bool hasStss = false;
};

// A declared entry count whose table cannot fit in the box is malformed.
bool tableFits(const BoxReader& reader,
               std::uint64_t entryCount,
               std::uint64_t entrySize)
{
    return entryCount <= maxTableEntries
           && entryCount * entrySize <= reader.remaining();
}

struct Mp4TrackParser
{
    bool parseFile(std::span<const std::uint8_t> fileBytes)
    {
        auto reader = BoxReader {fileBytes};
        auto box = Box {};

        if (!nextBox(reader, box) || box.type != boxFtyp)
            return false;

        while (nextBox(reader, box))
            if (box.type == boxMoov)
                return parseMoov(box.payload);

        return false;
    }

    bool parseMoov(std::span<const std::uint8_t> payload)
    {
        auto reader = BoxReader {payload};
        auto box = Box {};

        while (nextBox(reader, box))
            if (box.type == boxTrak && trakIsVideo(box.payload))
                return parseTrak(box.payload);

        return false;
    }

    bool trakIsVideo(std::span<const std::uint8_t> trak) const
    {
        auto mdia = Box {};
        auto hdlr = Box {};

        if (!findChild(trak, boxMdia, mdia)
            || !findChild(mdia.payload, boxHdlr, hdlr))
            return false;

        auto reader = BoxReader {hdlr.payload};
        reader.skip(8);
        return reader.readU32() == fourcc("vide") && reader.ok();
    }

    bool parseTrak(std::span<const std::uint8_t> trak)
    {
        auto mdia = Box {};
        auto mdhd = Box {};
        auto minf = Box {};
        auto stbl = Box {};

        return findChild(trak, boxMdia, mdia)
               && findChild(mdia.payload, boxMdhd, mdhd) && parseMdhd(mdhd.payload)
               && findChild(mdia.payload, boxMinf, minf)
               && findChild(minf.payload, boxStbl, stbl) && parseStbl(stbl.payload);
    }

    bool parseMdhd(std::span<const std::uint8_t> payload)
    {
        auto reader = BoxReader {payload};
        auto is64Bit = reader.readU8() == 1;
        reader.skip(3);
        reader.skip(is64Bit ? 16 : 8);

        info.timescale = reader.readU32();
        auto duration =
            is64Bit ? reader.readU64() : std::uint64_t {reader.readU32()};

        if (!reader.ok() || info.timescale == 0)
            return false;

        // All-ones is the container's "unknown duration" sentinel.
        auto unknown =
            is64Bit ? ~std::uint64_t {0} : std::uint64_t {~std::uint32_t {0}};
        info.duration = duration == unknown ? 0 : duration;
        return true;
    }

    bool parseStbl(std::span<const std::uint8_t> stbl)
    {
        auto reader = BoxReader {stbl};
        auto box = Box {};

        while (nextBox(reader, box))
        {
            auto parsed = true;

            switch (box.type)
            {
                case boxStsd:
                    parsed = parseStsd(box.payload);
                    break;
                case boxStts:
                    parsed = parseStts(box.payload);
                    break;
                case boxCtts:
                    parsed = parseCtts(box.payload);
                    break;
                case boxStsc:
                    parsed = parseStsc(box.payload);
                    break;
                case boxStco:
                    parsed = parseChunkOffsets(box.payload, false);
                    break;
                case boxCo64:
                    parsed = parseChunkOffsets(box.payload, true);
                    break;
                case boxStsz:
                    parsed = parseStsz(box.payload);
                    break;
                case boxStss:
                    parsed = parseStss(box.payload);
                    break;
                default:
                    break;
            }

            if (!parsed)
                return false;
        }

        return info.codec != Mp4Codec::Unknown && !tables.stts.empty()
               && !tables.stsc.empty() && !tables.chunkOffsets.empty()
               && tables.sampleCount > 0;
    }

    bool parseStsd(std::span<const std::uint8_t> payload)
    {
        auto reader = BoxReader {payload};
        reader.skip(4);
        auto entryCount = reader.readU32();

        if (!reader.ok() || entryCount == 0)
            return false;

        auto entry = Box {};
        return nextBox(reader, entry) && parseSampleEntry(entry);
    }

    bool parseSampleEntry(const Box& entry)
    {
        auto isAvc = entry.type == fourcc("avc1") || entry.type == fourcc("avc3");
        auto isHevc = entry.type == fourcc("hvc1") || entry.type == fourcc("hev1");

        if ((!isAvc && !isHevc) || entry.payload.size() < visualSampleEntrySize)
            return false;

        auto reader = BoxReader {entry.payload};
        reader.skip(24);
        info.width = reader.readU16();
        info.height = reader.readU16();

        auto config = Box {};
        auto configType = isAvc ? fourcc("avcC") : fourcc("hvcC");

        if (!findChild(
                entry.payload.subspan(visualSampleEntrySize), configType, config))
            return false;

        info.codecConfig.assign(config.payload.begin(), config.payload.end());
        info.codec = isAvc ? Mp4Codec::H264 : Mp4Codec::Hevc;
        return true;
    }

    bool parseStts(std::span<const std::uint8_t> payload)
    {
        auto reader = BoxReader {payload};
        reader.skip(4);
        auto entryCount = reader.readU32();

        if (!reader.ok() || !tableFits(reader, entryCount, 8))
            return false;

        tables.stts.reserve(static_cast<int>(entryCount));

        for (auto i = std::uint32_t {0}; i < entryCount; ++i)
        {
            auto count = reader.readU32();
            auto delta = reader.readU32();
            tables.stts.push_back({count, delta});
        }

        return reader.ok();
    }

    bool parseCtts(std::span<const std::uint8_t> payload)
    {
        auto reader = BoxReader {payload};
        auto isSigned = reader.readU8() == 1;
        reader.skip(3);
        auto entryCount = reader.readU32();

        if (!reader.ok() || !tableFits(reader, entryCount, 8))
            return false;

        tables.ctts.reserve(static_cast<int>(entryCount));

        for (auto i = std::uint32_t {0}; i < entryCount; ++i)
        {
            auto count = reader.readU32();
            auto offset = isSigned ? std::int64_t {reader.readS32()}
                                   : std::int64_t {reader.readU32()};
            tables.ctts.push_back({count, offset});
        }

        tables.hasCtts = true;
        return reader.ok();
    }

    bool parseStsc(std::span<const std::uint8_t> payload)
    {
        auto reader = BoxReader {payload};
        reader.skip(4);
        auto entryCount = reader.readU32();

        if (!reader.ok() || !tableFits(reader, entryCount, 12))
            return false;

        tables.stsc.reserve(static_cast<int>(entryCount));

        for (auto i = std::uint32_t {0}; i < entryCount; ++i)
        {
            auto firstChunk = reader.readU32();
            auto samplesPerChunk = reader.readU32();
            reader.skip(4);
            tables.stsc.push_back({firstChunk, samplesPerChunk});
        }

        return reader.ok();
    }

    bool parseChunkOffsets(std::span<const std::uint8_t> payload, bool is64Bit)
    {
        auto reader = BoxReader {payload};
        reader.skip(4);
        auto entryCount = reader.readU32();
        auto entrySize = is64Bit ? std::uint64_t {8} : std::uint64_t {4};

        if (!reader.ok() || !tableFits(reader, entryCount, entrySize))
            return false;

        tables.chunkOffsets.reserve(static_cast<int>(entryCount));

        for (auto i = std::uint32_t {0}; i < entryCount; ++i)
            tables.chunkOffsets.push_back(
                is64Bit ? reader.readU64() : std::uint64_t {reader.readU32()});

        return reader.ok();
    }

    bool parseStsz(std::span<const std::uint8_t> payload)
    {
        auto reader = BoxReader {payload};
        reader.skip(4);
        tables.constantSampleSize = reader.readU32();
        tables.sampleCount = reader.readU32();

        if (!reader.ok() || tables.sampleCount > maxTableEntries)
            return false;

        if (tables.constantSampleSize == 0)
        {
            if (!tableFits(reader, tables.sampleCount, 4))
                return false;

            tables.sampleSizes.reserve(static_cast<int>(tables.sampleCount));

            for (auto i = std::uint32_t {0}; i < tables.sampleCount; ++i)
                tables.sampleSizes.push_back(reader.readU32());
        }

        return reader.ok();
    }

    bool parseStss(std::span<const std::uint8_t> payload)
    {
        auto reader = BoxReader {payload};
        reader.skip(4);
        auto entryCount = reader.readU32();

        if (!reader.ok() || !tableFits(reader, entryCount, 4))
            return false;

        tables.syncSamples.reserve(static_cast<int>(entryCount));

        for (auto i = std::uint32_t {0}; i < entryCount; ++i)
            tables.syncSamples.push_back(reader.readU32());

        tables.hasStss = true;
        return reader.ok();
    }

    Mp4TrackInfo info;
    Mp4TrackTables tables;
};

bool stscEntriesAreOrdered(const Mp4TrackTables& tables)
{
    auto chunkCount = static_cast<std::uint32_t>(tables.chunkOffsets.size());

    for (auto i = 0; i < tables.stsc.size(); ++i)
    {
        auto& entry = tables.stsc[i];

        if (entry.firstChunk == 0 || entry.firstChunk > chunkCount)
            return false;

        if (i > 0 && entry.firstChunk <= tables.stsc[i - 1].firstChunk)
            return false;
    }

    return true;
}

// stsc runs x chunk offsets x sample sizes -> one byte range per sample,
// every range checked against the file before it is handed out.
bool resolveSampleRanges(const Mp4TrackTables& tables,
                         std::uint64_t fileSize,
                         Vector<Mp4Sample>& out)
{
    if (!stscEntriesAreOrdered(tables))
        return false;

    auto sampleSizeAt = [&](std::uint32_t index)
    {
        return tables.constantSampleSize != 0
                   ? tables.constantSampleSize
                   : tables.sampleSizes[static_cast<int>(index)];
    };

    auto chunkCount = static_cast<std::uint32_t>(tables.chunkOffsets.size());
    out.reserve(static_cast<int>(tables.sampleCount));
    auto sampleIndex = std::uint32_t {0};

    for (auto entryIndex = 0; entryIndex < tables.stsc.size(); ++entryIndex)
    {
        auto& entry = tables.stsc[entryIndex];
        auto lastChunk = entryIndex + 1 < tables.stsc.size()
                             ? tables.stsc[entryIndex + 1].firstChunk - 1
                             : chunkCount;

        for (auto chunk = entry.firstChunk; chunk <= lastChunk; ++chunk)
        {
            auto offset = tables.chunkOffsets[static_cast<int>(chunk - 1)];

            for (auto i = std::uint32_t {0};
                 i < entry.samplesPerChunk && sampleIndex < tables.sampleCount;
                 ++i, ++sampleIndex)
            {
                auto size = std::uint64_t {sampleSizeAt(sampleIndex)};

                if (offset > fileSize || size > fileSize - offset)
                    return false;

                auto sample = Mp4Sample {};
                sample.byteRange = {offset, size};
                out.push_back(sample);
                offset += size;
            }
        }
    }

    return static_cast<std::uint32_t>(out.size()) == tables.sampleCount;
}

// stts accumulation -> decode times; ctts offsets -> presentation times,
// equal to the decode times when the box is absent.
bool applyTimestamps(const Mp4TrackTables& tables, Vector<Mp4Sample>& out)
{
    auto decodeTime = std::int64_t {0};
    auto sttsIndex = 0;
    auto sttsUsed = std::uint32_t {0};

    for (auto& sample: out)
    {
        while (sttsIndex < tables.stts.size()
               && sttsUsed == tables.stts[sttsIndex].count)
        {
            ++sttsIndex;
            sttsUsed = 0;
        }

        if (sttsIndex == tables.stts.size())
            return false;

        sample.decodeTime = decodeTime;
        sample.duration = tables.stts[sttsIndex].delta;
        decodeTime += sample.duration;
        ++sttsUsed;
    }

    if (!tables.hasCtts)
    {
        for (auto& sample: out)
            sample.presentationTime = sample.decodeTime;

        return true;
    }

    auto cttsIndex = 0;
    auto cttsUsed = std::uint32_t {0};

    for (auto& sample: out)
    {
        while (cttsIndex < tables.ctts.size()
               && cttsUsed == tables.ctts[cttsIndex].count)
        {
            ++cttsIndex;
            cttsUsed = 0;
        }

        if (cttsIndex == tables.ctts.size())
            return false;

        sample.presentationTime = sample.decodeTime + tables.ctts[cttsIndex].offset;
        ++cttsUsed;
    }

    return true;
}

bool markKeyframes(const Mp4TrackTables& tables, Vector<Mp4Sample>& out)
{
    if (!tables.hasStss)
    {
        for (auto& sample: out)
            sample.keyframe = true;

        return true;
    }

    auto sampleCount = static_cast<std::uint32_t>(out.size());

    for (auto syncSample: tables.syncSamples)
    {
        if (syncSample == 0 || syncSample > sampleCount)
            return false;

        out[static_cast<int>(syncSample - 1)].keyframe = true;
    }

    return true;
}
} // namespace

bool Mp4Demuxer::open(const FilePath& path)
{
    file.emplace(path);

    if (!file->isValid() || !parse(file->bytes()))
    {
        file.reset();
        return false;
    }

    return true;
}

bool Mp4Demuxer::parse(std::span<const std::uint8_t> fileBytes)
{
    valid = false;
    trackInfo = {};
    sampleList.clear();
    fileData = {};

    auto parser = Mp4TrackParser {};

    if (!parser.parseFile(fileBytes))
        return false;

    auto samples = Vector<Mp4Sample> {};

    if (!resolveSampleRanges(parser.tables, fileBytes.size(), samples)
        || !applyTimestamps(parser.tables, samples)
        || !markKeyframes(parser.tables, samples))
        return false;

    trackInfo = std::move(parser.info);
    sampleList = std::move(samples);
    fileData = fileBytes;
    valid = true;
    return true;
}

std::span<const std::uint8_t> Mp4Demuxer::sampleBytes(int index) const
{
    if (index < 0 || index >= sampleList.size())
        return {};

    auto& range = sampleList[index].byteRange;

    if (range.end() > fileData.size())
        return {};

    return fileData.subspan(static_cast<std::size_t>(range.start),
                            static_cast<std::size_t>(range.length));
}

double Mp4Demuxer::toSeconds(std::int64_t timeUnits) const
{
    if (trackInfo.timescale == 0)
        return 0.0;

    return static_cast<double>(timeUnits) / trackInfo.timescale;
}
} // namespace eacp::Video
