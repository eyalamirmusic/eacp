#include "Common.h"

#include <eacp/Video/Demux/Mp4Demuxer.h>
#include <eacp/Video/SyntheticClip.h>

#include <cmath>
#include <span>
#include <string>
#include <utility>

using namespace nano;
using namespace eacp;

namespace
{
// The same small clip RoundTripTests uses: what the Media Foundation /
// AVFoundation encoder actually writes, as opposed to the hand-built
// structures below, which pin down every byte.
Video::SyntheticClipOptions demuxClipOptions()
{
    auto options = Video::SyntheticClipOptions {};
    options.width = 320;
    options.height = 240;
    options.fps = 10;
    options.duration = 1.6;
    return options;
}

const FilePath& demuxTestClip()
{
    static const auto path = Video::cachedSyntheticClip(demuxClipOptions());
    return path;
}

using Bytes = Vector<std::uint8_t>;

bool parseBytes(Video::Mp4Demuxer& demuxer, const Bytes& bytes)
{
    auto span = std::span<const std::uint8_t> {
        bytes.data(), static_cast<std::size_t>(bytes.size())};
    return demuxer.parse(span);
}

void appendBE16(Bytes& out, std::uint16_t value)
{
    out.push_back((value >> 8) & 0xFF);
    out.push_back(value & 0xFF);
}

void appendBE32(Bytes& out, std::uint32_t value)
{
    appendBE16(out, (value >> 16) & 0xFFFF);
    appendBE16(out, value & 0xFFFF);
}

void appendBE64(Bytes& out, std::uint64_t value)
{
    appendBE32(out, static_cast<std::uint32_t>(value >> 32));
    appendBE32(out, static_cast<std::uint32_t>(value));
}

void appendTag(Bytes& out, const char* tag)
{
    for (auto i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(tag[i]));
}

void appendRun(Bytes& out, int count, std::uint8_t value)
{
    for (auto i = 0; i < count; ++i)
        out.push_back(value);
}

Bytes mp4Box(const char* type, const Bytes& payload)
{
    auto out = Bytes {};
    appendBE32(out, static_cast<std::uint32_t>(payload.size() + 8));
    appendTag(out, type);
    out.addFrom(payload);
    return out;
}

Bytes mp4FullBox(const char* type, std::uint8_t version, const Bytes& payload)
{
    auto body = Bytes {version, 0, 0, 0};
    body.addFrom(payload);
    return mp4Box(type, body);
}

// Builds a minimal but structurally complete MP4 in memory, with knobs for
// every variation the demuxer must handle. Sample i is filled with the byte
// i + 1, so a resolved byte range can be checked against the sample it is
// supposed to cover, not just against being in bounds.
struct TestMp4Builder
{
    Vector<std::uint32_t> sampleSizes {10, 20, 30, 40, 50};
    std::uint32_t constantSampleSize = 0;
    int constantSampleCount = 0;
    Vector<std::uint32_t> samplesPerChunkPattern {5};
    std::uint32_t chunkGap = 0;
    std::uint32_t timescale = 1000;
    std::uint32_t sampleDelta = 100;
    std::uint32_t sttsSampleCount = 0;
    int cttsVersion = -1;
    Vector<std::int32_t> cttsOffsets;
    bool includeStss = true;
    Vector<std::uint32_t> syncSamples {1};
    int mdhdVersion = 0;
    bool hevc = false;
    bool useCo64 = false;
    bool mdatFirst = false;
    bool largesizeMdat = false;
    bool sizeZeroMdat = false;
    bool includeUnknownBoxes = false;
    bool omitConfigBox = false;
    bool stscFirstChunkZero = false;
    std::uint64_t chunkOffsetOverride = 0;
    Vector<std::string> omitBoxes;
    Bytes codecConfig {0x01, 0x64, 0x00, 0x28, 0xFF, 0xE1};

    bool omits(const char* name) const { return omitBoxes.contains(name); }

    int totalSamples() const
    {
        return constantSampleSize != 0 ? constantSampleCount : sampleSizes.size();
    }

    std::uint32_t sizeOf(int sample) const
    {
        return constantSampleSize != 0 ? constantSampleSize : sampleSizes[sample];
    }

    std::uint64_t durationUnits() const
    {
        return std::uint64_t {sampleDelta}
               * static_cast<std::uint64_t>(totalSamples());
    }

    Bytes mdatPayload() const
    {
        auto out = Bytes {};
        auto sample = 0;

        for (auto chunkSamples: samplesPerChunkPattern)
        {
            for (auto i = std::uint32_t {0};
                 i < chunkSamples && sample < totalSamples();
                 ++i, ++sample)
                appendRun(out,
                          static_cast<int>(sizeOf(sample)),
                          static_cast<std::uint8_t>(sample + 1));

            appendRun(out, static_cast<int>(chunkGap), 0xEE);
        }

        for (; sample < totalSamples(); ++sample)
            appendRun(out,
                      static_cast<int>(sizeOf(sample)),
                      static_cast<std::uint8_t>(sample + 1));

        return out;
    }

    Bytes buildMdat() const
    {
        auto payload = mdatPayload();

        if (largesizeMdat)
        {
            auto out = Bytes {};
            appendBE32(out, 1);
            appendTag(out, "mdat");
            appendBE64(out, static_cast<std::uint64_t>(payload.size()) + 16);
            out.addFrom(payload);
            return out;
        }

        if (sizeZeroMdat)
        {
            auto out = Bytes {};
            appendBE32(out, 0);
            appendTag(out, "mdat");
            out.addFrom(payload);
            return out;
        }

        return mp4Box("mdat", payload);
    }

    Bytes sampleEntry() const
    {
        auto body = Bytes {};
        appendRun(body, 6, 0);
        appendBE16(body, 1); // data_reference_index
        appendRun(body, 16, 0);
        appendBE16(body, 640);
        appendBE16(body, 360);
        appendBE32(body, 0x00480000);
        appendBE32(body, 0x00480000);
        appendBE32(body, 0);
        appendBE16(body, 1); // frame_count
        appendRun(body, 32, 0);
        appendBE16(body, 0x0018);
        appendBE16(body, 0xFFFF);

        if (!omitConfigBox)
            body.addFrom(mp4Box(hevc ? "hvcC" : "avcC", codecConfig));

        return mp4Box(hevc ? "hvc1" : "avc1", body);
    }

    Bytes buildStsd() const
    {
        auto payload = Bytes {};
        appendBE32(payload, 1);
        payload.addFrom(sampleEntry());
        return mp4FullBox("stsd", 0, payload);
    }

    Bytes buildStts() const
    {
        auto count = sttsSampleCount != 0
                         ? sttsSampleCount
                         : static_cast<std::uint32_t>(totalSamples());
        auto payload = Bytes {};
        appendBE32(payload, 1);
        appendBE32(payload, count);
        appendBE32(payload, sampleDelta);
        return mp4FullBox("stts", 0, payload);
    }

    Bytes buildCtts() const
    {
        auto payload = Bytes {};
        appendBE32(payload, static_cast<std::uint32_t>(cttsOffsets.size()));

        for (auto offset: cttsOffsets)
        {
            appendBE32(payload, 1);
            appendBE32(payload, static_cast<std::uint32_t>(offset));
        }

        return mp4FullBox("ctts", static_cast<std::uint8_t>(cttsVersion), payload);
    }

    Bytes buildStsc() const
    {
        auto entries = Vector<std::pair<std::uint32_t, std::uint32_t>> {};

        for (auto i = 0; i < samplesPerChunkPattern.size(); ++i)
            if (i == 0 || samplesPerChunkPattern[i] != samplesPerChunkPattern[i - 1])
                entries.push_back(
                    {static_cast<std::uint32_t>(i + 1), samplesPerChunkPattern[i]});

        auto payload = Bytes {};
        appendBE32(payload, static_cast<std::uint32_t>(entries.size()));

        for (auto& [firstChunk, samplesPerChunk]: entries)
        {
            auto first = stscFirstChunkZero && firstChunk == 1 ? 0u : firstChunk;
            appendBE32(payload, first);
            appendBE32(payload, samplesPerChunk);
            appendBE32(payload, 1);
        }

        return mp4FullBox("stsc", 0, payload);
    }

    Vector<std::uint64_t> chunkOffsets(std::uint64_t base) const
    {
        auto offsets = Vector<std::uint64_t> {};
        auto offset = base;
        auto sample = 0;

        for (auto chunkSamples: samplesPerChunkPattern)
        {
            offsets.push_back(offset);

            for (auto i = std::uint32_t {0};
                 i < chunkSamples && sample < totalSamples();
                 ++i, ++sample)
                offset += sizeOf(sample);

            offset += chunkGap;
        }

        if (chunkOffsetOverride != 0 && !offsets.empty())
            offsets.front() = chunkOffsetOverride;

        return offsets;
    }

    Bytes buildChunkOffsetBox(std::uint64_t mdatPayloadStart) const
    {
        auto offsets = chunkOffsets(mdatPayloadStart);
        auto payload = Bytes {};
        appendBE32(payload, static_cast<std::uint32_t>(offsets.size()));

        for (auto offset: offsets)
        {
            if (useCo64)
                appendBE64(payload, offset);
            else
                appendBE32(payload, static_cast<std::uint32_t>(offset));
        }

        return mp4FullBox(useCo64 ? "co64" : "stco", 0, payload);
    }

    Bytes buildStsz() const
    {
        auto payload = Bytes {};
        appendBE32(payload, constantSampleSize);
        appendBE32(payload, static_cast<std::uint32_t>(totalSamples()));

        if (constantSampleSize == 0)
            for (auto size: sampleSizes)
                appendBE32(payload, size);

        return mp4FullBox("stsz", 0, payload);
    }

    Bytes buildStss() const
    {
        auto payload = Bytes {};
        appendBE32(payload, static_cast<std::uint32_t>(syncSamples.size()));

        for (auto sample: syncSamples)
            appendBE32(payload, sample);

        return mp4FullBox("stss", 0, payload);
    }

    Bytes buildMdhd() const
    {
        auto payload = Bytes {};

        if (mdhdVersion == 1)
        {
            appendBE64(payload, 0);
            appendBE64(payload, 0);
            appendBE32(payload, timescale);
            appendBE64(payload, durationUnits());
        }
        else
        {
            appendBE32(payload, 0);
            appendBE32(payload, 0);
            appendBE32(payload, timescale);
            appendBE32(payload, static_cast<std::uint32_t>(durationUnits()));
        }

        appendBE16(payload, 0x55C4); // language 'und'
        appendBE16(payload, 0);
        return mp4FullBox("mdhd", static_cast<std::uint8_t>(mdhdVersion), payload);
    }

    Bytes buildHdlr() const
    {
        auto payload = Bytes {};
        appendBE32(payload, 0);
        appendTag(payload, "vide");
        appendRun(payload, 13, 0);
        return mp4FullBox("hdlr", 0, payload);
    }

    Bytes buildElst() const
    {
        auto payload = Bytes {};
        appendBE32(payload, 1);
        appendBE32(payload, static_cast<std::uint32_t>(durationUnits()));
        appendBE32(payload, 0);
        appendBE16(payload, 1);
        appendBE16(payload, 0);
        return mp4Box("edts", mp4FullBox("elst", 0, payload));
    }

    Bytes buildMoov(std::uint64_t mdatPayloadStart) const
    {
        auto stbl = Bytes {};

        if (!omits("stsd"))
            stbl.addFrom(buildStsd());
        if (!omits("stts"))
            stbl.addFrom(buildStts());
        if (cttsVersion >= 0)
            stbl.addFrom(buildCtts());
        if (!omits("stsc"))
            stbl.addFrom(buildStsc());
        if (!omits("stco"))
            stbl.addFrom(buildChunkOffsetBox(mdatPayloadStart));
        if (!omits("stsz"))
            stbl.addFrom(buildStsz());
        if (includeStss)
            stbl.addFrom(buildStss());

        auto mdia = Bytes {};
        mdia.addFrom(buildMdhd());
        mdia.addFrom(buildHdlr());
        mdia.addFrom(mp4Box("minf", mp4Box("stbl", stbl)));

        auto trak = Bytes {};

        if (includeUnknownBoxes)
            trak.addFrom(buildElst());

        trak.addFrom(mp4Box("mdia", mdia));

        auto moovPayload = Bytes {};

        if (includeUnknownBoxes)
            moovPayload.addFrom(mp4Box("udta", Bytes {1, 2, 3}));

        moovPayload.addFrom(mp4Box("trak", trak));
        return mp4Box("moov", moovPayload);
    }

    Bytes build() const
    {
        auto out = Bytes {};
        auto ftypPayload = Bytes {};
        appendTag(ftypPayload, "isom");
        appendBE32(ftypPayload, 512);
        appendTag(ftypPayload, "isom");
        out.addFrom(mp4Box("ftyp", ftypPayload));

        if (includeUnknownBoxes)
            out.addFrom(mp4Box("free", Bytes {0xAA, 0xBB}));

        auto mdatHeader = std::uint64_t {largesizeMdat ? 16u : 8u};
        auto moovSize = static_cast<std::uint64_t>(buildMoov(0).size());
        auto mdatPayloadStart =
            mdatFirst ? out.size() + mdatHeader : out.size() + moovSize + mdatHeader;

        auto moov = buildMoov(mdatPayloadStart);
        auto mdat = buildMdat();

        if (mdatFirst)
        {
            out.addFrom(mdat);

            if (includeUnknownBoxes)
                out.addFrom(mp4Box("moof", Bytes {0x00}));

            out.addFrom(moov);
        }
        else
        {
            out.addFrom(moov);
            out.addFrom(mdat);
        }

        return out;
    }
};

// Every resolved byte range points at the bytes the builder wrote for that
// sample — the strongest possible check that chunk mapping, sizes and
// offsets all agree, with no offset arithmetic repeated in the test.
bool sampleContentMatches(const Video::Mp4Demuxer& demuxer)
{
    for (auto i = 0; i < demuxer.samples().size(); ++i)
    {
        auto bytes = demuxer.sampleBytes(i);

        if (bytes.size() != demuxer.samples()[i].byteRange.length)
            return false;

        for (auto byte: bytes)
            if (byte != static_cast<std::uint8_t>(i + 1))
                return false;
    }

    return true;
}
} // namespace

// ---- Against the clip the platform encoder actually writes ----

// The demuxer opens a real encoder-written file and reads the track the
// same way the platform decoder does.
auto tOpensSyntheticClip = test("Mp4Demuxer/opensSyntheticClip") = []
{
    check(!demuxTestClip().empty());

    auto demuxer = Video::Mp4Demuxer {};
    check(demuxer.open(demuxTestClip()));
    check(demuxer.isValid());

    auto& track = demuxer.track();
    check(track.codec == Video::Mp4Codec::H264);
    check(track.width == 320);
    check(track.height == 240);
    check(track.timescale > 0);

    // An avcC record always starts with configurationVersion 1.
    check(!track.codecConfig.empty());
    check(track.codecConfig.front() == 0x01);
};

// The sample tables account for every frame the encoder produced.
auto tSampleCount = test("Mp4Demuxer/sampleCountMatchesEncoder") = []
{
    auto demuxer = Video::Mp4Demuxer {};
    check(demuxer.open(demuxTestClip()));
    check(demuxer.samples().size()
          == Video::syntheticFrameCount(demuxClipOptions()));
};

// Decode times rise monotonically and the durations add up to the clip
// length. Nothing here assumes a specific timescale or GOP structure —
// the encoder does not pin those.
auto tDecodeTimesRise = test("Mp4Demuxer/decodeTimesRise") = []
{
    auto demuxer = Video::Mp4Demuxer {};
    check(demuxer.open(demuxTestClip()));

    auto previous = std::int64_t {-1};
    auto totalDuration = std::int64_t {0};

    for (auto& sample: demuxer.samples())
    {
        check(sample.decodeTime > previous);
        check(sample.duration > 0);

        previous = sample.decodeTime;
        totalDuration += sample.duration;
    }

    check(std::abs(demuxer.toSeconds(totalDuration) - 1.6) < 0.15);
};

// The stream must start on a sync sample; whether later frames are
// keyframes is the encoder's business.
auto tFirstSampleKeyframe = test("Mp4Demuxer/firstSampleIsKeyframe") = []
{
    auto demuxer = Video::Mp4Demuxer {};
    check(demuxer.open(demuxTestClip()));
    check(demuxer.samples().front().keyframe);
};

// Every byte range lands inside the file, sampleBytes agrees with it, and
// the first sample starts with a plausible length-prefixed NAL unit — the
// shape avcC's lengthSizeMinusOne promises.
auto tSampleRangesInsideFile = test("Mp4Demuxer/sampleRangesInsideFile") = []
{
    auto demuxer = Video::Mp4Demuxer {};
    check(demuxer.open(demuxTestClip()));

    auto fileSize = MemoryMappedFile {demuxTestClip()}.size();

    for (auto i = 0; i < demuxer.samples().size(); ++i)
    {
        auto& range = demuxer.samples()[i].byteRange;
        check(!range.empty());
        check(range.end() <= fileSize);
        check(demuxer.sampleBytes(i).size() == range.length);
    }

    auto first = demuxer.sampleBytes(0);
    check(first.size() > 4);

    auto nalLength =
        std::uint64_t {first[0]} << 24 | first[1] << 16 | first[2] << 8 | first[3];
    check(nalLength > 0);
    check(nalLength + 4 <= first.size());
};

// ---- Against hand-built structures, where every byte is pinned ----

// The baseline builder file round-trips exactly: sizes, contiguous
// offsets, stts-derived decode times, and the codec config verbatim.
auto tParsesHandBuiltFile = test("Mp4Demuxer/parsesHandBuiltFile") = []
{
    auto builder = TestMp4Builder {};
    auto bytes = builder.build();
    auto demuxer = Video::Mp4Demuxer {};

    check(parseBytes(demuxer, bytes));
    check(demuxer.isValid());

    auto& track = demuxer.track();
    check(track.codec == Video::Mp4Codec::H264);
    check(track.width == 640);
    check(track.height == 360);
    check(track.timescale == 1000);
    check(track.duration == 500);
    check(track.codecConfig == builder.codecConfig);

    auto& samples = demuxer.samples();
    check(samples.size() == 5);

    for (auto i = 0; i < samples.size(); ++i)
    {
        check(samples[i].byteRange.length == builder.sampleSizes[i]);
        check(samples[i].decodeTime == std::int64_t {i} * 100);
        check(samples[i].presentationTime == samples[i].decodeTime);
        check(samples[i].duration == 100);

        if (i > 0)
            check(samples[i].byteRange.start == samples[i - 1].byteRange.end());
    }

    check(samples[0].keyframe);
    check(!samples[1].keyframe);
    check(sampleContentMatches(demuxer));
    check(std::abs(demuxer.toSeconds(500) - 0.5) < 1e-9);
};

// ctts offsets shift presentation times off decode times: v0 unsigned, v1
// signed, where a negative offset legally drives PTS below DTS.
auto tAppliesCttsOffsets = test("Mp4Demuxer/appliesCttsOffsets") = []
{
    auto builder = TestMp4Builder {};
    builder.cttsVersion = 0;
    builder.cttsOffsets = {0, 200, 100, 300, 0};

    auto demuxer = Video::Mp4Demuxer {};
    check(parseBytes(demuxer, builder.build()));

    for (auto i = 0; i < 5; ++i)
        check(demuxer.samples()[i].presentationTime
              == demuxer.samples()[i].decodeTime + builder.cttsOffsets[i]);

    builder.cttsVersion = 1;
    builder.cttsOffsets = {100, -100, 0, 50, -50};

    check(parseBytes(demuxer, builder.build()));
    check(demuxer.samples()[1].presentationTime == 0);

    for (auto i = 0; i < 5; ++i)
        check(demuxer.samples()[i].presentationTime
              == demuxer.samples()[i].decodeTime + builder.cttsOffsets[i]);
};

// co64 carries the same offsets in 64-bit form.
auto tReadsCo64 = test("Mp4Demuxer/readsCo64Offsets") = []
{
    auto builder = TestMp4Builder {};
    builder.useCo64 = true;
    auto bytes = builder.build();

    auto demuxer = Video::Mp4Demuxer {};
    check(parseBytes(demuxer, bytes));
    check(demuxer.samples().size() == 5);
    check(sampleContentMatches(demuxer));
};

// A varying samples-per-chunk pattern exercises the stsc run expansion:
// offsets accumulate within a chunk and jump the gap at chunk boundaries.
auto tMapsChunks = test("Mp4Demuxer/mapsChunksThroughStsc") = []
{
    auto builder = TestMp4Builder {};
    builder.samplesPerChunkPattern = {2, 2, 1};
    builder.chunkGap = 4;
    auto bytes = builder.build();

    auto demuxer = Video::Mp4Demuxer {};
    check(parseBytes(demuxer, bytes));

    auto& samples = demuxer.samples();
    check(samples.size() == 5);
    check(samples[1].byteRange.start == samples[0].byteRange.end());
    check(samples[2].byteRange.start == samples[1].byteRange.end() + 4);
    check(samples[3].byteRange.start == samples[2].byteRange.end());
    check(samples[4].byteRange.start == samples[3].byteRange.end() + 4);
    check(sampleContentMatches(demuxer));
};

// stss flags exactly the listed samples; a file without stss is legally
// all keyframes.
auto tStssSubset = test("Mp4Demuxer/stssMarksKeyframeSubset") = []
{
    auto builder = TestMp4Builder {};
    builder.syncSamples = {1, 4};

    auto demuxer = Video::Mp4Demuxer {};
    check(parseBytes(demuxer, builder.build()));

    const bool expected[] = {true, false, false, true, false};

    for (auto i = 0; i < 5; ++i)
        check(demuxer.samples()[i].keyframe == expected[i]);

    builder.includeStss = false;
    check(parseBytes(demuxer, builder.build()));

    for (auto& sample: demuxer.samples())
        check(sample.keyframe);
};

// A constant stsz sample size stands in for the whole table.
auto tConstantSampleSize = test("Mp4Demuxer/constantSampleSize") = []
{
    auto builder = TestMp4Builder {};
    builder.constantSampleSize = 16;
    builder.constantSampleCount = 6;
    builder.samplesPerChunkPattern = {6};
    auto bytes = builder.build();

    auto demuxer = Video::Mp4Demuxer {};
    check(parseBytes(demuxer, bytes));
    check(demuxer.samples().size() == 6);

    for (auto& sample: demuxer.samples())
        check(sample.byteRange.length == 16);

    check(sampleContentMatches(demuxer));
};

// A version 1 mdhd carries 64-bit times around a 32-bit timescale.
auto tVersion1Mdhd = test("Mp4Demuxer/readsVersion1Mdhd") = []
{
    auto builder = TestMp4Builder {};
    builder.mdhdVersion = 1;

    auto demuxer = Video::Mp4Demuxer {};
    check(parseBytes(demuxer, builder.build()));
    check(demuxer.track().timescale == 1000);
    check(demuxer.track().duration == 500);
};

// moov after mdat is how most muxers finalize; the parser walks past mdat
// to reach it.
auto tMoovAfterMdat = test("Mp4Demuxer/moovAfterMdat") = []
{
    auto builder = TestMp4Builder {};
    builder.mdatFirst = true;
    auto bytes = builder.build();

    auto demuxer = Video::Mp4Demuxer {};
    check(parseBytes(demuxer, bytes));
    check(demuxer.samples().size() == 5);
    check(sampleContentMatches(demuxer));
};

// The two special size encodings: size 1 with a 64-bit largesize, and a
// trailing size 0 that runs to the end of the file.
auto tSpecialBoxSizes = test("Mp4Demuxer/largesizeAndSizeZeroBoxes") = []
{
    auto builder = TestMp4Builder {};
    builder.largesizeMdat = true;
    auto largesizeBytes = builder.build();

    auto demuxer = Video::Mp4Demuxer {};
    check(parseBytes(demuxer, largesizeBytes));
    check(sampleContentMatches(demuxer));

    builder.largesizeMdat = false;
    builder.sizeZeroMdat = true;
    auto sizeZeroBytes = builder.build();

    check(parseBytes(demuxer, sizeZeroBytes));
    check(sampleContentMatches(demuxer));
};

// An hvc1 entry with an hvcC record is reported as HEVC.
auto tHevcTrack = test("Mp4Demuxer/hevcTrack") = []
{
    auto builder = TestMp4Builder {};
    builder.hevc = true;
    builder.codecConfig = {0x01, 0x02, 0x20, 0x00, 0x00};

    auto demuxer = Video::Mp4Demuxer {};
    check(parseBytes(demuxer, builder.build()));
    check(demuxer.track().codec == Video::Mp4Codec::Hevc);
    check(demuxer.track().codecConfig == builder.codecConfig);
};

// free, udta, edts/elst and moof are present but out of scope: the parser
// steps over them and still finds the track.
auto tSkipsUnknownBoxes = test("Mp4Demuxer/skipsUnknownBoxes") = []
{
    auto builder = TestMp4Builder {};
    builder.includeUnknownBoxes = true;
    builder.mdatFirst = true;
    auto bytes = builder.build();

    auto demuxer = Video::Mp4Demuxer {};
    check(parseBytes(demuxer, bytes));
    check(demuxer.samples().size() == 5);
    check(sampleContentMatches(demuxer));
};

// ---- Malformed input ----

// Not-MP4 bytes of every small size fail cleanly.
auto tRejectsGarbage = test("Mp4Demuxer/rejectsEmptyAndGarbage") = []
{
    auto demuxer = Video::Mp4Demuxer {};
    check(!parseBytes(demuxer, Bytes {}));

    for (auto size = 1; size < 8; ++size)
    {
        auto shortBytes = Bytes {};
        appendRun(shortBytes, size, 0x42);
        check(!parseBytes(demuxer, shortBytes));
    }

    auto garbage = Bytes {};
    appendRun(garbage, 64, 0xAB);
    check(!parseBytes(demuxer, garbage));
    check(!demuxer.isValid());
};

// Every possible truncation of a valid file fails without crashing. This
// sweeps each interesting boundary — mid-header, mid-table, missing mdat
// bytes — in one pass, since the whole file is only a few hundred bytes.
auto tTruncationSweep = test("Mp4Demuxer/survivesTruncationEverywhere") = []
{
    auto bytes = TestMp4Builder {}.build();
    auto demuxer = Video::Mp4Demuxer {};

    check(parseBytes(demuxer, bytes));

    auto anyPrefixParsed = false;

    for (auto length = 0; length < bytes.size(); ++length)
    {
        auto prefix = std::span<const std::uint8_t> {
            bytes.data(), static_cast<std::size_t>(length)};
        anyPrefixParsed = demuxer.parse(prefix) || anyPrefixParsed;
    }

    check(!anyPrefixParsed);
};

// Box sizes that lie: a child claiming more bytes than its parent holds,
// and 32/64-bit sizes chosen to overflow naive end arithmetic.
auto tRejectsOversizedBoxes = test("Mp4Demuxer/rejectsOversizedChildBox") = []
{
    auto ftyp = Bytes {};
    appendBE32(ftyp, 16);
    appendTag(ftyp, "ftyp");
    appendTag(ftyp, "isom");
    appendBE32(ftyp, 512);

    auto demuxer = Video::Mp4Demuxer {};

    auto oversizedChild = ftyp;
    auto moovPayload = Bytes {};
    appendBE32(moovPayload, 1000);
    appendTag(moovPayload, "trak");
    moovPayload.push_back(0);
    oversizedChild.addFrom(mp4Box("moov", moovPayload));
    check(!parseBytes(demuxer, oversizedChild));

    auto overflowing32 = ftyp;
    appendBE32(overflowing32, 0xFFFFFFF0u);
    appendTag(overflowing32, "moov");
    check(!parseBytes(demuxer, overflowing32));

    auto overflowing64 = ftyp;
    appendBE32(overflowing64, 1);
    appendTag(overflowing64, "moov");
    appendBE64(overflowing64, 0xFFFFFFFFFFFFFF00ull);
    check(!parseBytes(demuxer, overflowing64));

    auto shortLargesize = ftyp;
    appendBE32(shortLargesize, 1);
    appendTag(shortLargesize, "moov");
    appendBE64(shortLargesize, 8);
    check(!parseBytes(demuxer, shortLargesize));
};

// A track missing any required sample table, or its codec configuration,
// does not half-parse.
auto tRejectsMissingTables = test("Mp4Demuxer/rejectsMissingSampleTables") = []
{
    auto demuxer = Video::Mp4Demuxer {};

    for (auto* name: {"stsd", "stts", "stsc", "stco", "stsz"})
    {
        auto builder = TestMp4Builder {};
        builder.omitBoxes = {name};
        check(!parseBytes(demuxer, builder.build()));
    }

    auto builder = TestMp4Builder {};
    builder.omitConfigBox = true;
    check(!parseBytes(demuxer, builder.build()));
};

// Tables that contradict each other or the file: an offset past the end,
// an stts that covers fewer samples than stsz declares, an stsc that
// cannot be 1-based, and one that leaves samples without a chunk.
auto tRejectsBadTables = test("Mp4Demuxer/rejectsBadTables") = []
{
    auto demuxer = Video::Mp4Demuxer {};

    // Fits in stco's 32 bits but points far past this few-hundred-byte file.
    auto pastEnd = TestMp4Builder {};
    pastEnd.chunkOffsetOverride = 0xFFFF0000;
    check(!parseBytes(demuxer, pastEnd.build()));

    auto shortStts = TestMp4Builder {};
    shortStts.sttsSampleCount = 2;
    check(!parseBytes(demuxer, shortStts.build()));

    auto zeroFirstChunk = TestMp4Builder {};
    zeroFirstChunk.stscFirstChunkZero = true;
    check(!parseBytes(demuxer, zeroFirstChunk.build()));

    auto uncoveredSamples = TestMp4Builder {};
    uncoveredSamples.samplesPerChunkPattern = {2};
    check(!parseBytes(demuxer, uncoveredSamples.build()));
};

// Accessors stay safe on an empty demuxer and off the end of a full one.
auto tOutOfRangeAccess = test("Mp4Demuxer/sampleBytesOutOfRange") = []
{
    auto demuxer = Video::Mp4Demuxer {};
    check(!demuxer.isValid());
    check(demuxer.samples().empty());
    check(demuxer.sampleBytes(0).empty());
    check(demuxer.toSeconds(1000) == 0.0);

    check(parseBytes(demuxer, TestMp4Builder {}.build()));
    check(demuxer.sampleBytes(demuxer.samples().size()).empty());
};
