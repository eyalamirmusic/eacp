#include "Zip.h"
#include "Files.h"
#include "MemoryMappedFile.h"
#include "StdPath.h"

#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

namespace eacp::Zip
{
namespace
{
int toMinizLevel(Level level)
{
    switch (level)
    {
        case Level::none:
            return MZ_NO_COMPRESSION;
        case Level::fastest:
            return MZ_BEST_SPEED;
        case Level::normal:
            return MZ_DEFAULT_LEVEL;
        case Level::best:
            return MZ_BEST_COMPRESSION;
    }

    return MZ_DEFAULT_LEVEL;
}

std::string describe(mz_zip_archive& zip)
{
    const auto error = mz_zip_get_last_error(&zip);

    if (error == MZ_ZIP_NO_ERROR)
        return {};

    return mz_zip_get_error_string(error);
}

Entry toEntry(const mz_zip_archive_file_stat& stat)
{
    auto entry = Entry {};
    entry.name = stat.m_filename;
    entry.size = stat.m_uncomp_size;
    entry.compressedSize = stat.m_comp_size;
    entry.modificationTime = static_cast<std::int64_t>(stat.m_time);
    entry.isDirectory = stat.m_is_directory != 0;

    return entry;
}

Span<const std::uint8_t> asBytes(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::string asText(const Bytes& bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.getSize()};
}

// Every buffer here grows by appending a range of non-const std::uint8_t
// pointers, and never by resize() or by inserting anything else: libc++'s
// memcpy specialisation for the insert matches only when the source element
// type is exactly the destination's, const included, and every other route
// is a byte-at-a-time loop in a Debug build - seconds per GiB. So each
// producer fills a Scratch and appends from it, and one that is handed a
// const pointer copies through the Scratch first.
using Scratch = std::vector<std::uint8_t>;

void appendFrom(Bytes& bytes, Scratch& scratch, std::size_t count)
{
    auto* begin = scratch.data();
    bytes.getVector().insert(bytes.getVector().end(), begin, begin + count);
}

void appendVia(Bytes& bytes, Scratch& scratch, const void* data, std::size_t count)
{
    if (scratch.size() < count)
        scratch.resize(count);

    std::memcpy(scratch.data(), data, count);
    appendFrom(bytes, scratch, count);
}

constexpr auto chunkLimit = std::size_t {1} << 30;
constexpr auto scratchSize = std::size_t {1} << 20;

// mz_stream counts in unsigned int, and the stream-shaped miniz calls refuse
// anything past 4 GiB outright, so the deflate and inflate loops below feed
// chunks that fit and collect their output through a scratch buffer.
unsigned int chunkOf(std::size_t remaining)
{
    return static_cast<unsigned int>(std::min(remaining, chunkLimit));
}

// Where an entry may land under an extraction root: a relative path that,
// once normalized, never climbs above its start. Nullopt for anything else,
// and for a name that normalizes to nothing.
std::optional<std::filesystem::path> relativeEntryPath(std::string_view name)
{
    auto text = std::string {name};
    std::replace(text.begin(), text.end(), '\\', '/');

    const auto path = std::filesystem::path {text}.lexically_normal();

    if (path.empty() || path.has_root_name() || path.has_root_directory())
        return std::nullopt;

    for (const auto& part: path)
        if (part == "..")
            return std::nullopt;

    return path;
}
} // namespace

struct Reader::Impl
{
    explicit Impl(const FilePath& path)
        : mapping(std::in_place, path)
    {
        if (mapping->isValid())
            open(mapping->bytes().data(), mapping->size());
    }

    explicit Impl(Bytes bytesToUse)
        : bytes(std::move(bytesToUse))
    {
        open(bytes.data(), bytes.getSize());
    }

    ~Impl()
    {
        if (valid)
            mz_zip_reader_end(&zip);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void open(const std::uint8_t* data, std::size_t size)
    {
        mz_zip_zero_struct(&zip);
        valid = mz_zip_reader_init_mem(&zip, data, size, 0) != 0;
    }

    int locate(std::string_view name) const
    {
        if (!valid)
            return -1;

        const auto text = std::string {name};

        return mz_zip_reader_locate_file(
            &zip, text.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
    }

    std::optional<Entry> stat(int index) const
    {
        auto info = mz_zip_archive_file_stat {};

        if (index < 0 || !mz_zip_reader_file_stat(&zip, (mz_uint) index, &info))
            return std::nullopt;

        return toEntry(info);
    }

    std::optional<Bytes> extract(int index) const
    {
        const auto entry = stat(index);

        if (!entry || entry->isDirectory)
            return std::nullopt;

        auto* state = mz_zip_reader_extract_iter_new(&zip, (mz_uint) index, 0);

        if (state == nullptr)
            return std::nullopt;

        auto out = Bytes {};
        out.reserve(static_cast<std::size_t>(entry->size));
        auto scratch = Scratch(scratchSize);

        while (true)
        {
            const auto n = mz_zip_reader_extract_iter_read(
                state, scratch.data(), scratch.size());

            if (n == 0)
                break;

            appendFrom(out, scratch, n);
        }

        // False unless the whole entry came through and its CRC matched.
        if (!mz_zip_reader_extract_iter_free(state))
            return std::nullopt;

        return out;
    }

    std::optional<MemoryMappedFile> mapping;
    Bytes bytes;
    mutable mz_zip_archive zip {};
    bool valid = false;
};

Reader::Reader(const FilePath& path)
    : impl(path)
{
}

Reader::Reader(Bytes bytes)
    : impl(std::move(bytes))
{
}

bool Reader::isValid() const
{
    return impl->valid;
}

std::string Reader::errorMessage() const
{
    return describe(impl->zip);
}

int Reader::numEntries() const
{
    if (!impl->valid)
        return 0;

    return static_cast<int>(mz_zip_reader_get_num_files(&impl->zip));
}

Vector<Entry> Reader::entries() const
{
    auto result = Vector<Entry> {};

    for (auto index = 0; index < numEntries(); ++index)
        if (auto entry = impl->stat(index))
            result.add(std::move(*entry));

    return result;
}

bool Reader::contains(std::string_view name) const
{
    return impl->locate(name) >= 0;
}

std::optional<Entry> Reader::find(std::string_view name) const
{
    return impl->stat(impl->locate(name));
}

std::optional<Bytes> Reader::read(std::string_view name) const
{
    return impl->extract(impl->locate(name));
}

std::optional<std::string> Reader::readText(std::string_view name) const
{
    auto bytes = read(name);

    if (!bytes)
        return std::nullopt;

    return asText(*bytes);
}

bool Reader::extractAll(const FilePath& directory) const
{
    if (!impl->valid)
        return false;

    const auto root = toStdPath(directory);
    auto ok = true;

    for (auto index = 0; index < numEntries(); ++index)
    {
        const auto entry = impl->stat(index);

        if (!entry)
        {
            ok = false;
            continue;
        }

        const auto relative = relativeEntryPath(entry->name);

        if (!relative)
        {
            ok = false;
            continue;
        }

        const auto target = root / *relative;
        auto ec = std::error_code {};

        if (entry->isDirectory)
        {
            std::filesystem::create_directories(target, ec);
            ok = ok && !ec;
            continue;
        }

        const auto bytes = impl->extract(index);

        if (!bytes)
        {
            ok = false;
            continue;
        }

        try
        {
            Files::writeFile(FilePath {target}, *bytes);
        }
        catch (const std::exception&)
        {
            ok = false;
        }
    }

    return ok;
}

struct Writer::Impl
{
    Impl()
    {
        mz_zip_zero_struct(&zip);
        zip.m_pWrite = &Impl::write;
        zip.m_pIO_opaque = this;
        open = mz_zip_writer_init(&zip, 0) != 0;
    }

    ~Impl()
    {
        if (open)
            mz_zip_writer_end(&zip);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    static size_t write(void* opaque, mz_uint64 offset, const void* data, size_t n)
    {
        auto& out = static_cast<Impl*>(opaque)->out;
        const auto start = static_cast<std::size_t>(offset);

        if (start == out.getSize())
        {
            appendVia(out, static_cast<Impl*>(opaque)->scratch, data, n);
            return n;
        }

        if (start + n > out.getSize())
            out.resize(start + n);

        std::memcpy(out.data() + start, data, n);

        return n;
    }

    bool add(std::string_view name, Span<const std::uint8_t> bytes, Level level)
    {
        if (!open)
        {
            error = "the archive is already finished";
            return false;
        }

        const auto text = std::string {name};
        const auto added = mz_zip_writer_add_mem(&zip,
                                                 text.c_str(),
                                                 bytes.data(),
                                                 bytes.getSize(),
                                                 (mz_uint) toMinizLevel(level));

        error = added ? std::string {} : describe(zip);

        return added != 0;
    }

    Bytes finish()
    {
        if (!open)
        {
            error = "the archive is already finished";
            return {};
        }

        const auto finalized = mz_zip_writer_finalize_archive(&zip);
        error = finalized ? std::string {} : describe(zip);

        mz_zip_writer_end(&zip);
        open = false;

        if (!finalized)
            return {};

        return std::exchange(out, Bytes {});
    }

    Bytes out;
    Scratch scratch;
    mz_zip_archive zip {};
    std::string error;
    bool open = false;
};

Writer::Writer()
    : impl()
{
}

std::string Writer::errorMessage() const
{
    return impl->error;
}

bool Writer::add(std::string_view name, Span<const std::uint8_t> bytes, Level level)
{
    return impl->add(name, bytes, level);
}

bool Writer::add(std::string_view name, std::string_view text, Level level)
{
    return impl->add(name, asBytes(text), level);
}

bool Writer::addFile(std::string_view name, const FilePath& path, Level level)
{
    const auto file = MemoryMappedFile {path};

    if (!file.isValid())
    {
        impl->error = "cannot read '" + path.str() + "'";
        return false;
    }

    return impl->add(name, file.bytes(), level);
}

bool Writer::addDirectory(const FilePath& directory,
                          std::string_view prefix,
                          Level level)
{
    const auto root = toStdPath(directory);
    auto ec = std::error_code {};
    auto files = std::vector<std::filesystem::path> {};

    for (const auto& item: std::filesystem::recursive_directory_iterator(root, ec))
        if (item.is_regular_file(ec))
            files.push_back(item.path());

    if (ec)
    {
        impl->error = "cannot list '" + directory.str() + "'";
        return false;
    }

    std::sort(files.begin(), files.end());

    auto base = std::string {prefix};

    while (!base.empty() && base.back() == '/')
        base.pop_back();

    auto ok = true;

    for (const auto& file: files)
    {
        const auto relative = file.lexically_relative(root).generic_string();
        const auto name = base.empty() ? relative : base + "/" + relative;

        ok = addFile(name, FilePath {file}, level) && ok;
    }

    return ok;
}

Bytes Writer::finish()
{
    return impl->finish();
}

bool Writer::writeTo(const FilePath& path)
{
    const auto bytes = finish();

    if (bytes.empty())
        return false;

    try
    {
        Files::writeFile(path, bytes);
    }
    catch (const std::exception& e)
    {
        impl->error = e.what();
        return false;
    }

    return true;
}

Bytes compress(Span<const std::uint8_t> bytes, Level level)
{
    auto deflater = mz_stream {};

    if (mz_deflateInit(&deflater, toMinizLevel(level)) != MZ_OK)
        return {};

    auto scratch = Scratch(scratchSize);
    auto out = Bytes {};
    auto consumed = std::size_t {0};
    auto result = Bytes {};

    while (true)
    {
        const auto in = chunkOf(bytes.getSize() - consumed);
        const auto last = consumed + in == bytes.getSize();

        deflater.next_in = bytes.data() + consumed;
        deflater.avail_in = in;
        deflater.next_out = scratch.data();
        deflater.avail_out = static_cast<unsigned int>(scratchSize);

        const auto status = mz_deflate(&deflater, last ? MZ_FINISH : MZ_NO_FLUSH);
        consumed += in - deflater.avail_in;
        appendFrom(out, scratch, scratchSize - deflater.avail_out);

        if (status == MZ_STREAM_END)
        {
            result = std::move(out);
            break;
        }

        if (status != MZ_OK)
            break;
    }

    mz_deflateEnd(&deflater);

    return result;
}

Bytes compress(std::string_view text, Level level)
{
    return compress(asBytes(text), level);
}

std::optional<Bytes> decompress(Span<const std::uint8_t> stream)
{
    auto inflater = mz_stream {};

    if (mz_inflateInit(&inflater) != MZ_OK)
        return std::nullopt;

    auto scratch = Scratch(scratchSize);
    auto out = Bytes {};
    auto consumed = std::size_t {0};
    auto result = std::optional<Bytes> {};

    while (true)
    {
        const auto in = chunkOf(stream.getSize() - consumed);

        inflater.next_in = stream.data() + consumed;
        inflater.avail_in = in;
        inflater.next_out = scratch.data();
        inflater.avail_out = static_cast<unsigned int>(scratchSize);

        const auto status = mz_inflate(&inflater, MZ_SYNC_FLUSH);
        consumed += in - inflater.avail_in;
        appendFrom(out, scratch, scratchSize - inflater.avail_out);

        if (status == MZ_STREAM_END)
        {
            result = std::move(out);
            break;
        }

        if (status != MZ_OK)
            break;
    }

    mz_inflateEnd(&inflater);

    return result;
}

std::optional<std::string> decompressText(Span<const std::uint8_t> stream)
{
    auto bytes = decompress(stream);

    if (!bytes)
        return std::nullopt;

    return asText(*bytes);
}
} // namespace eacp::Zip
