#include "Minidump-macOS.h"

#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/Core/Utils/Time.h>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto minidumpSignature = std::uint32_t {0x504D444D};
constexpr auto minidumpVersion = std::uint32_t {0xA793};
constexpr auto headerSize = std::uint32_t {32};
constexpr auto directoryEntrySize = std::uint32_t {12};

constexpr auto threadListStream = std::uint32_t {3};
constexpr auto moduleListStream = std::uint32_t {4};
constexpr auto memoryListStream = std::uint32_t {5};
constexpr auto exceptionStream = std::uint32_t {6};
constexpr auto systemInfoStream = std::uint32_t {7};
constexpr auto streamCount = std::uint32_t {5};

constexpr auto macOsPlatformId = std::uint32_t {0x8101};
constexpr auto workstationProductType = std::uint8_t {1};
constexpr auto codeViewPdb70Signature = std::uint32_t {0x53445352};
constexpr auto fixedFileInfoSize = std::size_t {52};
constexpr auto cpuInformationSize = std::size_t {24};
constexpr auto maxExceptionParameters = std::size_t {15};

#if defined(__arm64__)
constexpr auto processorArchitecture = std::uint16_t {12};
constexpr auto contextFlags = std::uint32_t {0x00400000 | 0x1 | 0x2};
#else
constexpr auto processorArchitecture = std::uint16_t {9};
constexpr auto contextFlags = std::uint32_t {0x00100000 | 0x1 | 0x2};
#endif

struct Location
{
    std::uint32_t size = 0;
    std::uint32_t rva = 0;
};

// The file as it is built: little-endian fields appended in order, and the
// offsets of earlier fields patched once what they point at is written.
class MinidumpBuffer
{
public:
    std::uint32_t position() const
    {
        return static_cast<std::uint32_t>(bytes.size());
    }

    void appendU8(std::uint8_t value) { bytes.add(value); }

    void appendU16(std::uint16_t value) { appendLittleEndian(value, sizeof(value)); }

    void appendU32(std::uint32_t value) { appendLittleEndian(value, sizeof(value)); }

    void appendU64(std::uint64_t value) { appendLittleEndian(value, sizeof(value)); }

    void appendZeros(std::size_t count)
    {
        for (auto index = std::size_t {0}; index < count; ++index)
            appendU8(0);
    }

    void appendBytes(std::span<const std::byte> data)
    {
        for (auto byte: data)
            bytes.add(static_cast<std::uint8_t>(byte));
    }

    void appendLocation(const Location& location)
    {
        appendU32(location.size);
        appendU32(location.rva);
    }

    void patchU32(std::uint32_t offset, std::uint32_t value)
    {
        for (auto index = 0u; index < sizeof(value); ++index)
            bytes[static_cast<int>(offset + index)] =
                static_cast<std::uint8_t>((value >> (index * 8)) & 0xFF);
    }

    void patchLocation(std::uint32_t offset, const Location& location)
    {
        patchU32(offset, location.size);
        patchU32(offset + sizeof(std::uint32_t), location.rva);
    }

    const Vector<std::uint8_t>& getBytes() const { return bytes; }

private:
    void appendLittleEndian(std::uint64_t value, std::size_t size)
    {
        for (auto index = std::size_t {0}; index < size; ++index)
            appendU8(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFF));
    }

    Vector<std::uint8_t> bytes;
};

constexpr auto firstSurrogateCodePoint = char32_t {0x10000};
constexpr auto highSurrogateBase = char32_t {0xD800};
constexpr auto lowSurrogateBase = char32_t {0xDC00};
constexpr auto surrogateBits = 10;
constexpr auto lowSurrogateMask = char32_t {0x3FF};

Vector<std::uint16_t> encodeUtf16(std::string_view utf8)
{
    auto units = Vector<std::uint16_t> {};

    for (auto character: Strings::widen(utf8))
    {
        auto codePoint = static_cast<char32_t>(character);

        if (codePoint < firstSurrogateCodePoint)
        {
            units.add(static_cast<std::uint16_t>(codePoint));
            continue;
        }

        auto offset = codePoint - firstSurrogateCodePoint;
        units.add(static_cast<std::uint16_t>(highSurrogateBase
                                             + (offset >> surrogateBits)));
        units.add(static_cast<std::uint16_t>(lowSurrogateBase
                                             + (offset & lowSurrogateMask)));
    }

    return units;
}

// MDString: the byte length of the UTF-16 text, the text, then a terminator.
std::uint32_t appendMinidumpString(MinidumpBuffer& buffer, std::string_view utf8)
{
    auto start = buffer.position();
    auto units = encodeUtf16(utf8);
    auto unitCount = static_cast<std::uint32_t>(units.size());
    auto byteLength = unitCount * std::uint32_t {sizeof(std::uint16_t)};
    buffer.appendU32(byteLength);

    for (auto unit: units)
        buffer.appendU16(unit);

    buffer.appendU16(0);
    return start;
}

std::uint32_t readBigEndian(std::span<const std::uint8_t> bytes)
{
    auto value = std::uint32_t {0};

    for (auto byte: bytes)
        value = (value << 8) | byte;

    return value;
}

// MDCVInfoPDB70, the GUID holding the LC_UUID so its debug id reads as the
// UUID does, which is how Breakpad and Crashpad write a Mach-O image.
Location appendCodeViewRecord(MinidumpBuffer& buffer, const AppImage& image)
{
    constexpr auto data2Offset = std::size_t {4};
    constexpr auto data3Offset = std::size_t {6};
    constexpr auto data4Offset = std::size_t {8};
    constexpr auto shortFieldSize = std::size_t {2};
    auto uuid = std::span<const std::uint8_t> {image.uuid};
    auto start = buffer.position();

    buffer.appendU32(codeViewPdb70Signature);
    buffer.appendU32(readBigEndian(uuid.first(data2Offset)));
    buffer.appendU16(static_cast<std::uint16_t>(
        readBigEndian(uuid.subspan(data2Offset, shortFieldSize))));
    buffer.appendU16(static_cast<std::uint16_t>(
        readBigEndian(uuid.subspan(data3Offset, shortFieldSize))));

    for (auto byte: uuid.subspan(data4Offset))
        buffer.appendU8(byte);

    buffer.appendU32(0);
    buffer.appendBytes(std::as_bytes(std::span {image.path}));
    buffer.appendU8(0);
    return {buffer.position() - start, start};
}

// A thread context is the registers in the byte order a reader expects, so
// the order is the format's, not ours: Windows' CONTEXT for that CPU, which
// Breakpad mirrors in src/google_breakpad/common/minidump_cpu_arm64.h
// (MDRawContextARM64) and minidump_cpu_amd64.h (MDRawContextAMD64). The values
// are thread_get_state's ARM_THREAD_STATE64 / x86_THREAD_STATE64, copied one
// register at a time into those slots; the flags say only the control and
// integer registers are filled, so the float and debug slots stay zero.
#if defined(__arm64__)
constexpr auto arm64GeneralRegisterCount = std::size_t {29};
constexpr auto arm64FloatRegisterBytes = std::size_t {32 * 16};
constexpr auto arm64DebugRegisterBytes = std::size_t {8 * 4 + 8 * 8 + 2 * 4 + 2 * 8};

// CONTEXT_ARM64: flags, cpsr, x0-x28, fp, lr, sp, pc, then the float and
// debug registers, left zero since the flags say they are absent.
Location appendThreadContext(MinidumpBuffer& buffer,
                             const TaskThreadRegisters& registers)
{
    const auto& state = registers.state;
    auto start = buffer.position();

    buffer.appendU32(contextFlags);
    buffer.appendU32(state.__cpsr);

    for (auto index = std::size_t {0}; index < arm64GeneralRegisterCount; ++index)
        buffer.appendU64(state.__x[index]);

    buffer.appendU64(registers.framePointer);
    buffer.appendU64(registers.linkRegister);
    buffer.appendU64(registers.stackPointer);
    buffer.appendU64(registers.programCounter);
    buffer.appendZeros(arm64FloatRegisterBytes);
    buffer.appendU32(0);
    buffer.appendU32(0);
    buffer.appendZeros(arm64DebugRegisterBytes);
    return {buffer.position() - start, start};
}
#else
constexpr auto amd64HomeRegisterBytes = std::size_t {6 * 8};
constexpr auto amd64DebugRegisterBytes = std::size_t {6 * 8};
constexpr auto amd64ExtendedStateBytes = std::size_t {512 + 26 * 16 + 6 * 8};

// CONTEXT_AMD64: the home slots, flags, segments, flags register, debug
// registers, the sixteen general registers in its order, rip, then the
// floating-point area, left zero.
Location appendThreadContext(MinidumpBuffer& buffer,
                             const TaskThreadRegisters& registers)
{
    const auto& state = registers.state;
    auto start = buffer.position();

    buffer.appendZeros(amd64HomeRegisterBytes);
    buffer.appendU32(contextFlags);
    buffer.appendU32(0);
    buffer.appendU16(static_cast<std::uint16_t>(state.__cs));
    buffer.appendZeros(3 * sizeof(std::uint16_t));
    buffer.appendU16(static_cast<std::uint16_t>(state.__fs));
    buffer.appendU16(static_cast<std::uint16_t>(state.__gs));
    buffer.appendU16(0);
    buffer.appendU32(static_cast<std::uint32_t>(state.__rflags));
    buffer.appendZeros(amd64DebugRegisterBytes);

    for (auto value: {state.__rax,
                      state.__rcx,
                      state.__rdx,
                      state.__rbx,
                      state.__rsp,
                      state.__rbp,
                      state.__rsi,
                      state.__rdi,
                      state.__r8,
                      state.__r9,
                      state.__r10,
                      state.__r11,
                      state.__r12,
                      state.__r13,
                      state.__r14,
                      state.__r15})
        buffer.appendU64(value);

    buffer.appendU64(state.__rip);
    buffer.appendZeros(amd64ExtendedStateBytes);
    return {buffer.position() - start, start};
}
#endif

struct VersionNumbers
{
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
};

VersionNumbers parseVersionNumbers(std::string_view version)
{
    auto numbers = std::array<std::uint32_t, 3> {};
    auto index = std::size_t {0};

    for (auto character: version)
    {
        if (character == '.' && ++index == numbers.size())
            break;

        if (character >= '0' && character <= '9')
            numbers[index] =
                numbers[index] * 10 + static_cast<std::uint32_t>(character - '0');
    }

    return {numbers[0], numbers[1], numbers[2]};
}

// MDRawSystemInfo; the OS build goes where Windows keeps its service pack.
Location appendSystemInfo(MinidumpBuffer& buffer, const MinidumpContents& contents)
{
    auto version = parseVersionNumbers(contents.operatingSystemVersion);
    auto start = buffer.position();

    buffer.appendU16(processorArchitecture);
    buffer.appendU16(0);
    buffer.appendU16(0);
    buffer.appendU8(static_cast<std::uint8_t>(contents.processorCount));
    buffer.appendU8(workstationProductType);
    buffer.appendU32(version.major);
    buffer.appendU32(version.minor);
    buffer.appendU32(version.patch);
    buffer.appendU32(macOsPlatformId);
    auto buildRvaOffset = buffer.position();
    buffer.appendU32(0);
    buffer.appendU16(0);
    buffer.appendU16(0);
    buffer.appendZeros(cpuInformationSize);

    auto location = Location {buffer.position() - start, start};
    auto buildRva = appendMinidumpString(buffer, contents.operatingSystemBuild);
    buffer.patchU32(buildRvaOffset, buildRva);
    return location;
}

struct ThreadBlobs
{
    Location context;
    Location stack;
};

ThreadBlobs appendThreadBlobs(MinidumpBuffer& buffer, const MinidumpThread& thread)
{
    auto context = appendThreadContext(buffer, thread.registers);
    auto stackStart = buffer.position();
    auto stackBytes = std::span<const std::byte> {
        thread.stackBytes.data(),
        static_cast<std::size_t>(thread.stackBytes.size())};
    buffer.appendBytes(stackBytes);
    return {context, {buffer.position() - stackStart, stackStart}};
}

Vector<ThreadBlobs> appendEveryThreadBlobs(MinidumpBuffer& buffer,
                                           const MinidumpContents& contents)
{
    auto blobs = Vector<ThreadBlobs> {};

    for (const auto& thread: contents.threads)
        blobs.add(appendThreadBlobs(buffer, thread));

    return blobs;
}

std::uint32_t countOf(const auto& items)
{
    return static_cast<std::uint32_t>(items.size());
}

void appendMemoryDescriptor(MinidumpBuffer& buffer,
                            std::uint64_t address,
                            const Location& memory)
{
    buffer.appendU64(address);
    buffer.appendLocation(memory);
}

// MDRawThread: the id, no suspend count, priority or TEB, the stack and the
// context.
void appendThreadEntry(MinidumpBuffer& buffer,
                       const MinidumpThread& thread,
                       const ThreadBlobs& blobs)
{
    buffer.appendU32(static_cast<std::uint32_t>(thread.threadId));
    buffer.appendU32(0);
    buffer.appendU32(0);
    buffer.appendU32(0);
    buffer.appendU64(0);
    appendMemoryDescriptor(buffer, thread.stack.start, blobs.stack);
    buffer.appendLocation(blobs.context);
}

Location appendThreadList(MinidumpBuffer& buffer,
                          const MinidumpContents& contents,
                          const Vector<ThreadBlobs>& blobs)
{
    auto start = buffer.position();
    buffer.appendU32(countOf(contents.threads));

    for (auto index = 0; index < contents.threads.size(); ++index)
        appendThreadEntry(buffer, contents.threads[index], blobs[index]);

    return {buffer.position() - start, start};
}

// The stacks again, for readers such as lldb that look memory up here.
Location appendMemoryList(MinidumpBuffer& buffer,
                          const MinidumpContents& contents,
                          const Vector<ThreadBlobs>& blobs)
{
    auto start = buffer.position();
    buffer.appendU32(countOf(contents.threads));

    for (auto index = 0; index < contents.threads.size(); ++index)
        appendMemoryDescriptor(
            buffer, contents.threads[index].stack.start, blobs[index].stack);

    return {buffer.position() - start, start};
}

// MDRawExceptionStream: the Mach exception as the code, its first code as
// the flags and both codes as the parameters, as Crashpad writes them.
Location appendException(MinidumpBuffer& buffer,
                         const MinidumpContents& contents,
                         const ThreadBlobs& crashingThreadBlobs)
{
    auto start = buffer.position();
    buffer.appendU32(static_cast<std::uint32_t>(contents.threads[0].threadId));
    buffer.appendU32(0);
    buffer.appendU32(static_cast<std::uint32_t>(contents.exceptionType));
    buffer.appendU32(static_cast<std::uint32_t>(contents.exceptionCodes[0]));
    buffer.appendU64(0);
    buffer.appendU64(contents.exceptionAddress);
    buffer.appendU32(static_cast<std::uint32_t>(contents.exceptionCodes.size()));
    buffer.appendU32(0);

    for (auto code: contents.exceptionCodes)
        buffer.appendU64(static_cast<std::uint64_t>(code));

    auto unusedParameters = maxExceptionParameters - contents.exceptionCodes.size();
    buffer.appendZeros(unusedParameters * sizeof(std::uint64_t));
    buffer.appendLocation(crashingThreadBlobs.context);
    return {buffer.position() - start, start};
}

constexpr auto moduleNameRvaOffset = std::uint32_t {20};
constexpr auto moduleCodeViewOffset = std::uint32_t {76};

// MDRawModule's fixed part, its name and CodeView record patched in later.
std::uint32_t appendModuleEntry(MinidumpBuffer& buffer, const AppImage& image)
{
    auto start = buffer.position();
    buffer.appendU64(image.base);
    buffer.appendU32(static_cast<std::uint32_t>(image.size));
    buffer.appendU32(0);
    buffer.appendU32(0);
    buffer.appendU32(0);
    buffer.appendZeros(fixedFileInfoSize);
    buffer.appendLocation({});
    buffer.appendLocation({});
    buffer.appendU64(0);
    buffer.appendU64(0);
    return start;
}

Location appendModuleList(MinidumpBuffer& buffer, const MinidumpContents& contents)
{
    auto start = buffer.position();
    buffer.appendU32(static_cast<std::uint32_t>(contents.images.size()));
    auto entryOffsets = Vector<std::uint32_t> {};

    for (const auto& image: contents.images)
        entryOffsets.add(appendModuleEntry(buffer, image));

    auto location = Location {buffer.position() - start, start};

    for (auto index = 0; index < contents.images.size(); ++index)
    {
        const auto& image = contents.images[index];
        auto nameRva = appendMinidumpString(buffer, image.path);
        auto codeView = appendCodeViewRecord(buffer, image);
        buffer.patchU32(entryOffsets[index] + moduleNameRvaOffset, nameRva);
        buffer.patchLocation(entryOffsets[index] + moduleCodeViewOffset, codeView);
    }

    return location;
}

void appendHeader(MinidumpBuffer& buffer)
{
    buffer.appendU32(minidumpSignature);
    buffer.appendU32(minidumpVersion);
    buffer.appendU32(streamCount);
    buffer.appendU32(headerSize);
    buffer.appendU32(0);
    constexpr auto millisecondsPerSecond = std::int64_t {1000};
    auto secondsSinceEpoch = Time::sinceEpoch().count / millisecondsPerSecond;
    buffer.appendU32(static_cast<std::uint32_t>(secondsSinceEpoch));
    buffer.appendU64(0);
}

struct StreamEntry
{
    std::uint32_t type = 0;
    Location location;
};

void patchDirectory(MinidumpBuffer& buffer, std::span<const StreamEntry> streams)
{
    auto offset = headerSize;

    for (const auto& stream: streams)
    {
        buffer.patchU32(offset, stream.type);
        buffer.patchLocation(offset + sizeof(std::uint32_t), stream.location);
        offset += directoryEntrySize;
    }
}
} // namespace

bool writeMinidump(const FilePath& path, const MinidumpContents& contents)
{
    if (contents.threads.empty())
        return false;

    auto buffer = MinidumpBuffer {};
    appendHeader(buffer);
    buffer.appendZeros(streamCount * directoryEntrySize);

    auto blobs = appendEveryThreadBlobs(buffer, contents);
    auto streams = std::to_array<StreamEntry>({
        {systemInfoStream, appendSystemInfo(buffer, contents)},
        {threadListStream, appendThreadList(buffer, contents, blobs)},
        {memoryListStream, appendMemoryList(buffer, contents, blobs)},
        {moduleListStream, appendModuleList(buffer, contents)},
        {exceptionStream, appendException(buffer, contents, blobs[0])},
    });
    static_assert(streams.size() == streamCount);

    patchDirectory(buffer, streams);

    try
    {
        Files::writeFile(path, buffer.getBytes());
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}
} // namespace eacp::CrashReporting::Detail
