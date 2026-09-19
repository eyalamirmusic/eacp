#include "Common.h"

#include <eacp/Core/Utils/MemoryMappedFile.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>

// A buffer over memory the caller already owns.
//
// Two things are under test and they are not the same thing. That the bytes
// arrive at all is true on every backend - the ones that cannot adopt host
// memory copy it - so every case here but one runs everywhere. That a host
// write afterwards is seen through the buffer, and a write through the buffer
// seen in the caller's own pages, is true only where the memory was adopted
// rather than copied, and Buffer::canAdoptMemory is what says which happened.
//
// The mapped-file case is the shape this exists for: one mapping of a large
// file becomes one buffer, and every tensor in it a BufferRange into that
// buffer, with nothing copied anywhere.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto elementCount = 1024;
constexpr auto sliceElements = 64;

// The slice a range is taken over starts on the strictest alignment either
// backend asks of a storage bind, so the same offset binds on all three.
constexpr auto sliceOffsetElements = 256;

struct DoubleKernel final : ComputeProgram
{
    DoubleKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * 2.0f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// Page-aligned host memory, which is what an adopted buffer is made over.
// Aligned operator new rather than posix_memalign or _aligned_malloc, because
// it is the one spelling all three platforms have.
std::align_val_t pageAlignment()
{
    return std::align_val_t((std::size_t) Buffer::memoryPageSize());
}

std::uint8_t* allocatePages(std::int64_t bytes)
{
    return static_cast<std::uint8_t*>(
        ::operator new((std::size_t) bytes, pageAlignment()));
}

void freePages(std::uint8_t* pages)
{
    ::operator delete(pages, pageAlignment());
}

// Distinct in every element, so bytes that arrived from the wrong offset are a
// failed comparison rather than filler that matches anyway.
Vector<float> ramp(int count)
{
    auto values = Vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.add((float) i * 0.5f - 3.0f);

    return values;
}

std::int64_t byteCountOf(const Vector<float>& values)
{
    return (std::int64_t) values.size() * (std::int64_t) sizeof(float);
}

std::filesystem::path writeScratchFile(const std::string& name,
                                       const Vector<float>& values)
{
    const auto dir = std::filesystem::temp_directory_path() / "eacp-adopted-memory";

    std::filesystem::create_directories(dir);

    const auto path = dir / name;
    auto out = std::ofstream {path, std::ios::binary | std::ios::trunc};

    out.write((const char*) values.data(), (std::streamsize) byteCountOf(values));

    return path;
}

Vector<float> doubledThrough(Device& device, const Buffer& input, int count)
{
    auto output = device.makeBuffer(
        (std::int64_t) count * (std::int64_t) sizeof(float), BufferUsage::Storage);

    auto kernel = DoubleKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    auto values = Vector<float>(count);
    output.read(values.data(), (std::int64_t) count * (std::int64_t) sizeof(float));

    return values;
}
} // namespace

// The page size is a real number on every platform, and the grid test agrees
// with it - the two being the whole of the contract a caller has to meet.
auto tPageGridIsAnswerable = test("AdoptedMemory/thePageGridIsAnswerable") = []
{
    const auto page = Buffer::memoryPageSize();

    check(page > 0);
    check((page & (page - 1)) == 0);

    auto* pages = allocatePages(page);

    check(Buffer::isPageAligned({pages, page}));
    check(!Buffer::isPageAligned({pages + 1, page - 1}));
    check(!Buffer::isPageAligned({nullptr, page}));
    check(!Buffer::isPageAligned({pages, 0}));

    freePages(pages);
};

// The bytes arrive, whether they were adopted or copied, and a kernel reads
// them like any other storage buffer.
auto tBufferOverPagesReadsThem =
    test("AdoptedMemory/aBufferOverHostPagesReadsThem") = []
{
    auto& device = Device::shared();

    if (!computeIsAvailable())
        return;

    const auto source = ramp(elementCount);
    const auto bytes = byteCountOf(source);

    auto* pages = allocatePages(bytes);
    std::memcpy(pages, source.data(), (std::size_t) bytes);

    auto released = false;

    {
        auto buffer = device.makeBufferOverMemory(
            {pages, bytes, [&] { released = true; }}, BufferUsage::Storage);

        check(buffer.isValid());
        check(buffer.size() == bytes);

        auto values = Vector<float>(elementCount);
        buffer.read(values.data(), bytes);

        for (auto i = 0; i < elementCount; ++i)
            check(values[i] == source[i]);

        const auto doubled = doubledThrough(device, buffer, elementCount);

        for (auto i = 0; i < elementCount; ++i)
            check(doubled[i] == source[i] * 2.0f);
    }

    // The callback is the caller's signal that the pages are its own again, and
    // it has to have fired by the time the Buffer that held them has gone.
    check(released);

    // Freed on that signal rather than merely after checking for it: a backend
    // that had not let go would be holding these pages, and freeing them anyway
    // would turn a failed check into a use-after-free somewhere else.
    if (released)
        freePages(pages);
};

// The whole reason for the feature query: where the memory was adopted, the
// caller and the GPU are looking at the same bytes, in both directions.
auto tAdoptedMemoryIsShared =
    test("AdoptedMemory/adoptedMemoryIsSharedBothWays") = []
{
    auto& device = Device::shared();

    if (!device.isValid() || !Buffer::canAdoptMemory(device))
        return;

    const auto source = ramp(elementCount);
    const auto bytes = byteCountOf(source);

    auto* pages = allocatePages(bytes);
    std::memcpy(pages, source.data(), (std::size_t) bytes);

    {
        auto buffer =
            device.makeBufferOverMemory({pages, bytes}, BufferUsage::Storage);

        check(buffer.isValid());

        // Written by the host after the buffer was made, and seen through it.
        auto* asFloats = reinterpret_cast<float*>(pages);
        asFloats[7] = 1234.5f;

        auto seen = 0.f;
        buffer.read(
            &seen, (std::int64_t) sizeof(float), 7 * (std::int64_t) sizeof(float));
        check(seen == 1234.5f);

        // And the other way round: a write through the buffer lands in the
        // caller's own pages rather than in a copy of them.
        const auto written = -9.75f;
        buffer.update(&written,
                      (std::int64_t) sizeof(float),
                      9 * (std::int64_t) sizeof(float));

        check(asFloats[9] == written);
    }

    freePages(pages);
};

// The shape this exists for: one mapping of a file, one buffer over the whole
// of it, and every piece of it a range. Nothing is copied on a backend that can
// adopt, and the mapping is held by the callback that outlives the call.
auto tMappedFileBecomesOneBuffer =
    test("AdoptedMemory/aMappedFileBecomesOneBufferOfRanges") = []
{
    auto& device = Device::shared();

    if (!computeIsAvailable())
        return;

    const auto source = ramp(elementCount);
    const auto bytes = byteCountOf(source);
    const auto path = writeScratchFile("ramp.bin", source);

    // Behind a shared_ptr because the mapping has to outlive the call that made
    // it and a std::function has to be copyable - which is what the callback in
    // ExternalMemory is for.
    auto mapped = std::make_shared<MemoryMappedFile>(FilePath {path});

    check(mapped->isValid());
    check((std::int64_t) mapped->size() == bytes);

    auto* start = const_cast<std::uint8_t*>(mapped->bytes().data());

    check(Buffer::isPageAligned({start, bytes}));

    auto buffer = device.makeBufferOverMemory({start, bytes, [mapped] {}},
                                              BufferUsage::Storage);

    check(buffer.isValid());
    check(buffer.size() == bytes);

    // One tensor out of the middle of the file, bound as a range rather than
    // copied into a buffer of its own.
    const auto sliceOffset =
        (std::int64_t) sliceOffsetElements * (std::int64_t) sizeof(float);

    const auto slice =
        BufferRange {&buffer,
                     sliceOffset,
                     (std::int64_t) sliceElements * (std::int64_t) sizeof(float)};

    check(slice.isValid());
    check(sliceOffset % device.storageBufferOffsetAlignment() == 0);

    auto output = device.makeBuffer(slice.bytes, BufferUsage::Storage);

    auto kernel = DoubleKernel {};
    kernel.input = slice;
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, sliceElements);
    }

    commands.commit();

    auto values = Vector<float>(sliceElements);
    output.read(values.data(), slice.bytes);

    for (auto i = 0; i < sliceElements; ++i)
        check(values[i] == source[sliceOffsetElements + i] * 2.0f);
};

// Off the grid is refused rather than quietly copied, on every backend, so a
// call site written where the memory is copied is one the adopting backend also
// takes. The caller still gets its memory back.
auto tMemoryOffTheGridIsRefused =
    test("AdoptedMemory/memoryOffThePageGridIsRefused") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const auto page = Buffer::memoryPageSize();

    auto* pages = allocatePages(page * 2);
    auto released = false;

    {
        auto buffer = device.makeBufferOverMemory(
            {pages + 1, page, [&] { released = true; }}, BufferUsage::Storage);

        check(!buffer.isValid());
    }

    check(released);

    if (released)
        freePages(pages);
};
