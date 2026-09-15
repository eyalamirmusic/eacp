#include "Common.h"

#include <eacp/Core/Utils/Environment.h>

#include <cmath>
#include <cstdint>
#include <vector>

// What a SIMD-group matrix has to answer with on a device: the same product a
// scalar reference computes, out of fragments a whole SIMD group holds between
// its lanes rather than out of anything one thread has.
//
// Three shapes of it. One fragment against one fragment, which is the operation
// itself with no tiling around it; the blocked product a transformer's linear
// is, over a threadgroup tile and several SIMD groups, at a shape whose every
// extent divides the tiling; and the same product at a shape whose extents
// divide none of it, which is what says the clamped loads and the guarded
// copy-out hold the edges.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto fragment = ComputeProgram::simdMatrixWidth;
constexpr auto fragmentElements = fragment * fragment;

// The fill the accumulator starts from, non-zero so that a fragment that was
// zeroed by something other than the fill still shows up.
constexpr auto accumulatorFill = 0.25f;

float scattered(int index, int salt)
{
    return (float) (((index * 37 + salt * 11) % 23) - 11) * 0.125f;
}

std::vector<float> scatteredValues(int count, int salt)
{
    auto values = std::vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.push_back(scattered(i, salt));

    return values;
}

Buffer bufferOf(const std::vector<float>& values)
{
    auto buffer = Device::shared().makeBuffer((int) (values.size() * sizeof(float)),
                                              BufferUsage::Storage);

    buffer.update(values.data(), (int) (values.size() * sizeof(float)));
    return buffer;
}

Buffer outputOf(std::size_t count)
{
    return Device::shared().makeBuffer((int) (count * sizeof(float)),
                                       BufferUsage::Storage);
}

std::vector<float> readBack(Buffer& buffer, std::size_t count)
{
    auto values = std::vector<float>(count);
    buffer.read(values.data(), (int) (count * sizeof(float)));
    return values;
}

// C[m, n] = fill + sum over k of a[m, k] * b[n, k] - the second operand read
// along k, which is the layout an nn.Linear weight already has.
std::vector<float> referenceProduct(const std::vector<float>& a,
                                    const std::vector<float>& b,
                                    int rows,
                                    int columns,
                                    int inner,
                                    float fill)
{
    auto result = std::vector<float>((std::size_t) rows * columns);

    for (auto m = 0; m < rows; ++m)
        for (auto n = 0; n < columns; ++n)
        {
            auto total = (double) fill;

            for (auto k = 0; k < inner; ++k)
                total += (double) a[(std::size_t) m * inner + k]
                         * (double) b[(std::size_t) n * inner + k];

            result[(std::size_t) m * columns + n] = (float) total;
        }

    return result;
}

// One fragment times one fragment, by the one SIMD group the dispatch runs.
// No tile, no loop: the operation on its own.
struct FragmentProduct final : ComputeProgram
{
    FragmentProduct()
        : ComputeProgram({simdWidth, 1, 1})
    {
        compile();
    }

    void define() override
    {
        auto start = unsignedInteger(0u);
        auto rowStride = unsignedInteger((unsigned) fragment);

        auto accumulator = simdMatrix(accumulatorFill);
        auto left = simdMatrix(a, start, rowStride);
        auto right = simdMatrix(b, start, rowStride);

        multiplyAccumulate(accumulator, left, right);
        write(output, start, rowStride, accumulator);
    }

    Uniform<InputBuffer> a;
    Uniform<InputBuffer> b;
    Uniform<OutputBuffer> output;

    EACP_SHADER(a, b, output)
};

// The same operation with the second operand read where it lies: a buffer of
// packed sixteen-bit weights, loaded as a fragment of its own type and
// multiplied into the float accumulator with no staging tile and no widening
// pass under it.
struct PackedFragmentProduct final : ComputeProgram
{
    explicit PackedFragmentProduct(SimdMatrixElement packing)
        : ComputeProgram({simdWidth, 1, 1})
        , element(packing)
    {
        compile();
    }

    void define() override
    {
        auto start = unsignedInteger(0u);
        auto rowStride = unsignedInteger((unsigned) fragment);

        auto accumulator = simdMatrix(accumulatorFill);
        auto left = simdMatrix(a, start, rowStride);

        auto right = element == SimdMatrixElement::Half
                         ? simdMatrixHalf(b, start, rowStride)
                         : simdMatrixBFloat16(b, start, rowStride);

        multiplyAccumulate(accumulator, left, right);
        write(output, start, rowStride, accumulator);
    }

    SimdMatrixElement element;

    Uniform<InputBuffer> a;
    Uniform<InputBuffer> b;
    Uniform<OutputBuffer> output;

    EACP_SHADER(a, b, output)
};

// One sixteen-bit element of each kind, and the value it reads back as. The
// reference multiplies the round trip rather than the original, so the test
// says the load found the right element and not that the format is lossless.
std::uint16_t narrowedTo(SimdMatrixElement element, float value)
{
    return element == SimdMatrixElement::Half ? halfFromFloat(value)
                                              : bfloat16FromFloat(value);
}

float widenedFrom(SimdMatrixElement element, std::uint16_t bits)
{
    return element == SimdMatrixElement::Half ? halfToFloat(bits)
                                              : bfloat16ToFloat(bits);
}

// Values a sixteen-bit format cannot hold exactly - a tenth is not a dyadic
// fraction - so a load that widened the wrong element, or the right one from
// the wrong half of its word, lands somewhere the tolerance does not reach.
std::vector<float> inexactValues(int count, int salt)
{
    auto values = std::vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.push_back((float) (((i * 29 + salt * 13) % 19) - 9) * 0.1f);

    return values;
}

// A bit set below anything eleven significand bits reach, so that a value
// carrying it survives the trip only if the float operand of a mixed product
// stays a float. Eighths alone cannot say: both sixteen-bit formats hold one
// exactly, so a product that narrowed every operand to the packed format would
// agree with the reference to the last bit and the test would pass anyway.
constexpr auto belowSixteenBits = 0x1p-12f;

std::vector<float> finelyScatteredValues(int count, int salt)
{
    auto values = scatteredValues(count, salt);

    for (auto& value: values)
        value *= 1.f + belowSixteenBits;

    return values;
}

// Two elements to a word, the low half first: the layout every packed read in
// the EDSL already assumes, and what makes the offset count in elements.
Buffer packedBufferOf(const std::vector<float>& values, SimdMatrixElement element)
{
    auto words = std::vector<std::uint32_t>((values.size() + 1) / 2);

    for (auto i = std::size_t {}; i < values.size(); ++i)
        words[i / 2] |= (std::uint32_t) narrowedTo(element, values[i])
                        << (16 * (i % 2));

    auto bytes = (int) (words.size() * sizeof(std::uint32_t));
    auto buffer = Device::shared().makeBuffer(bytes, BufferUsage::Storage);

    buffer.update(words.data(), bytes);
    return buffer;
}

std::vector<float> roundTripped(const std::vector<float>& values,
                                SimdMatrixElement element)
{
    auto widened = std::vector<float> {};

    for (auto value: values)
        widened.push_back(widenedFrom(element, narrowedTo(element, value)));

    return widened;
}

// Sets an environment variable for the length of a scope and puts back what was
// there, so a test that takes a device capability away cannot leave it away for
// whatever runs next. A variable that was unset is unset again rather than left
// empty, which is a different state to anything asking whether it is there.
struct ScopedEnv
{
    ScopedEnv(std::string_view nameToUse, std::string_view value)
        : name(nameToUse)
        , previous(getEnv(nameToUse))
    {
        setEnv(name, value);
    }

    ~ScopedEnv()
    {
        if (previous.has_value())
            setEnv(name, *previous);
        else
            unsetEnv(name);
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

    std::string name;
    std::optional<std::string> previous;
};

// The blocked product: a 64 x 64 tile of C per threadgroup of eight SIMD
// groups, each owning 32 x 16 of it as eight accumulator fragments, over a
// 32-deep slab of the inner dimension staged in threadgroup memory.
//
// A is read row-major and B along k, the two layouts an activation and a
// shipped weight already have. The slab is loaded clamped and zero-filled past
// the inner extent, and the tile of C is written back through the same
// threadgroup memory so that the copy-out can be guarded element by element -
// a fragment is stored whole, and a partial tile has no whole patch to store.
struct TiledProduct final : ComputeProgram
{
    static constexpr auto tileRows = 64;
    static constexpr auto tileColumns = 64;
    static constexpr auto slab = 32;
    static constexpr auto threads = 256;

    static constexpr auto rowFragments = 4;
    static constexpr auto columnFragments = 2;
    static constexpr auto tileElements = tileRows * slab + slab * tileColumns;
    static constexpr auto columnTileBase = tileRows * slab;

    TiledProduct()
        : ComputeProgram({threads, 1, 1})
    {
        compile();
    }

    void dispatch(ComputePass& pass, int rows, int columns, int inner)
    {
        rowCount = (std::uint32_t) rows;
        columnCount = (std::uint32_t) columns;
        innerCount = (std::uint32_t) inner;

        const auto rowTiles = (rows + tileRows - 1) / tileRows;
        const auto columnTiles = (columns + tileColumns - 1) / tileColumns;

        pass.dispatch(*this, columnTiles * threads, rowTiles);
    }

    void define() override
    {
        constexpr auto rows = (unsigned) tileRows;
        constexpr auto columns = (unsigned) tileColumns;
        constexpr auto depth = (unsigned) slab;
        constexpr auto side = (unsigned) fragment;

        auto lane = localPosition().x;
        auto group = groupPosition();
        auto simd = simdGroupIndex();

        auto m0 = group.y * rows;
        auto n0 = group.x * columns;

        auto rowOffset = (simd % 2u) * 32u;
        auto columnOffset = (simd / 2u) * 16u;

        auto tile = shared<Float>(tileElements);
        auto slabStride = unsignedInteger(depth);
        auto columnStride = unsignedInteger(columns);

        // Eight consecutive elements per thread of each slab, which is 256
        // threads covering both of them exactly.
        auto loadRow = lane / 4u;
        auto loadDepth = (lane % 4u) * 8u;

        auto aRow = min(m0 + loadRow, rowCount - 1u) * aRowStride;
        auto bRow = min(n0 + loadRow, columnCount - 1u) * bStride;

        SimdMatrix accumulators[rowFragments * columnFragments];

        for (auto& accumulator: accumulators)
            accumulator = simdMatrix();

        auto k0 = var(0u);

        loop(k0.get() < innerCount,
             [&]
             {
                 for (auto i = 0u; i < 8u; ++i)
                 {
                     auto k = k0.get() + loadDepth + i;
                     auto inside = k < innerCount;
                     auto at = min(k, innerCount - 1u);

                     write(tile,
                           loadRow * depth + loadDepth + i,
                           select(inside, a[aRow + at], 0.f));

                     write(tile,
                           columnTileBase + (loadDepth + i) * columns + loadRow,
                           select(inside, b[bRow + at], 0.f));
                 }

                 barrier();

                 for (auto kk = 0u; kk < depth; kk += side)
                 {
                     SimdMatrix left[rowFragments];
                     SimdMatrix right[columnFragments];

                     for (auto i = 0u; i < (unsigned) rowFragments; ++i)
                         left[i] = simdMatrix(
                             tile, (rowOffset + i * side) * depth + kk, slabStride);

                     for (auto j = 0u; j < (unsigned) columnFragments; ++j)
                         right[j] = simdMatrix(tile,
                                               columnTileBase + kk * columns
                                                   + columnOffset + j * side,
                                               columnStride);

                     for (auto i = 0u; i < (unsigned) rowFragments; ++i)
                         for (auto j = 0u; j < (unsigned) columnFragments; ++j)
                             multiplyAccumulate(
                                 accumulators[i * columnFragments + j],
                                 left[i],
                                 right[j]);
                 }

                 barrier();
                 k0 += depth;
             });

        barrier();

        for (auto i = 0u; i < (unsigned) rowFragments; ++i)
            for (auto j = 0u; j < (unsigned) columnFragments; ++j)
                write(tile,
                      (rowOffset + i * side) * columns + columnOffset + j * side,
                      columnStride,
                      accumulators[i * columnFragments + j]);

        barrier();

        for (auto i = 0u; i < (unsigned) (tileRows * tileColumns / threads); ++i)
        {
            auto index = lane + i * (unsigned) threads;
            auto m = m0 + index / columns;
            auto n = n0 + index % columns;

            ifThen(m < rowCount && n < columnCount,
                   [&] { write(output, m * cRowStride + n, tile[index]); });
        }
    }

    Uniform<InputBuffer> a;
    Uniform<InputBuffer> b;
    Uniform<OutputBuffer> output;
    Uniform<UInt> rowCount;
    Uniform<UInt> columnCount;
    Uniform<UInt> innerCount;
    Uniform<UInt> aRowStride;
    Uniform<UInt> bStride;
    Uniform<UInt> cRowStride;

    EACP_SHADER(a,
                b,
                output,
                rowCount,
                columnCount,
                innerCount,
                aRowStride,
                bStride,
                cRowStride)
};

// The blocked product run once over one shape, and what it wrote read back.
std::vector<float> tiledProduct(const std::vector<float>& a,
                                const std::vector<float>& b,
                                int rows,
                                int columns,
                                int inner)
{
    auto& device = Device::shared();

    auto left = bufferOf(a);
    auto right = bufferOf(b);
    auto result = outputOf((std::size_t) rows * columns);

    auto kernel = TiledProduct {};
    kernel.a = left;
    kernel.b = right;
    kernel.output = result;
    kernel.aRowStride = (std::uint32_t) inner;
    kernel.bStride = (std::uint32_t) inner;
    kernel.cRowStride = (std::uint32_t) columns;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        kernel.dispatch(pass, rows, columns, inner);
    }

    commands.commit();

    return readBack(result, (std::size_t) rows * columns);
}

// The tolerance a sum of `inner` products of values under 1.5 deserves in
// single precision, which is what both sides compute in.
float toleranceFor(int inner)
{
    return 2.0e-4f * (float) inner;
}

void checkMatches(const std::vector<float>& values,
                  const std::vector<float>& expected,
                  float tolerance)
{
    check(values.size() == expected.size());

    auto worst = 0.f;

    for (auto i = std::size_t {}; i < expected.size(); ++i)
        worst = std::max(worst, std::abs(values[i] - expected[i]));

    check(worst <= tolerance);
}
} // namespace

// The operation itself: one 8x8 fragment multiplied into another, by the
// thirty-two lanes that hold them between them.
auto tOneFragmentProduct =
    test("SimdMatrix/oneFragmentProductMatchesTheReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto a = scatteredValues(fragmentElements, 1);
    auto b = scatteredValues(fragmentElements, 2);

    auto left = bufferOf(a);
    auto right = bufferOf(b);
    auto result = outputOf(fragmentElements);

    auto kernel = FragmentProduct {};
    kernel.a = left;
    kernel.b = right;
    kernel.output = result;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, ComputeProgram::simdWidth);
    }

    commands.commit();

    // The kernel's fragments are read row-major, so the second operand's rows
    // are the product's inner index - a plain row-by-column product, unlike the
    // blocked one below, whose second operand is read along k.
    auto expected = std::vector<float>(fragmentElements, accumulatorFill);

    for (auto m = 0; m < fragment; ++m)
        for (auto n = 0; n < fragment; ++n)
            for (auto k = 0; k < fragment; ++k)
                expected[(std::size_t) m * fragment + n] +=
                    a[(std::size_t) m * fragment + k]
                    * b[(std::size_t) k * fragment + n];

    checkMatches(
        readBack(result, fragmentElements), expected, toleranceFor(fragment));
};

// The blocked product at a shape every extent of the tiling divides: 128 rows
// is two row tiles, 128 columns two column tiles, and 64 inner two whole slabs.
auto tTiledProductOnWholeTiles =
    test("SimdMatrix/blockedProductMatchesTheReference") = []
{
    if (!Device::shared().isValid())
        return;

    constexpr auto rows = 128;
    constexpr auto columns = 128;
    constexpr auto inner = 64;

    auto a = scatteredValues(rows * inner, 3);
    auto b = scatteredValues(columns * inner, 5);

    checkMatches(tiledProduct(a, b, rows, columns, inner),
                 referenceProduct(a, b, rows, columns, inner, 0.f),
                 toleranceFor(inner));
};

// And at a shape none of them divides: 100 rows leaves 36 of the second row
// tile past the data, 76 columns leaves 52 of the second column tile, and 45
// inner leaves 19 of the second slab. A clamped load that failed to zero-fill,
// or a copy-out that failed to guard, is a wrong number or a corrupted
// neighbour here and is neither above.
auto tTiledProductOnRaggedShape = test("SimdMatrix/blockedProductHoldsTheEdges") = []
{
    if (!Device::shared().isValid())
        return;

    constexpr auto rows = 100;
    constexpr auto columns = 76;
    constexpr auto inner = 45;

    auto a = scatteredValues(rows * inner, 7);
    auto b = scatteredValues(columns * inner, 11);

    checkMatches(tiledProduct(a, b, rows, columns, inner),
                 referenceProduct(a, b, rows, columns, inner, 0.f),
                 toleranceFor(inner));
};

// The shape a Whisper encoder's feed-forward is, which is what the primitive
// was added for. Checked against the scalar reference on a strip of the rows,
// the whole product being 2.3 million dot products of 384 terms and this being
// a test rather than a benchmark.
auto tTiledProductAtEncoderShape =
    test("SimdMatrix/blockedProductAtAnEncoderShape") = []
{
    if (!Device::shared().isValid())
        return;

    constexpr auto rows = 1500;
    constexpr auto columns = 1536;
    constexpr auto inner = 384;
    constexpr auto checkedRows = 8;

    auto a = scatteredValues(rows * inner, 13);
    auto b = scatteredValues(columns * inner, 17);

    auto values = tiledProduct(a, b, rows, columns, inner);

    auto strip = std::vector<float>(a.begin(), a.begin() + checkedRows * inner);
    auto expected = referenceProduct(strip, b, checkedRows, columns, inner, 0.f);

    values.resize((std::size_t) checkedRows * columns);
    checkMatches(values, expected, toleranceFor(inner));
};

namespace
{
// A float fragment against a packed one, run once and read back.
//
// The left operand carries a bit below what either sixteen-bit format holds and
// the reference multiplies it unnarrowed, so this checks two things at once:
// that the packed load found the right elements, and that the float operand of
// a mixed product stays a float. Eighths would have checked only the first -
// both formats hold one exactly, so a product that narrowed everything would
// have agreed to the last bit.
void checkPackedFragmentProduct(SimdMatrixElement element)
{
    auto& device = Device::shared();

    auto a = finelyScatteredValues(fragmentElements, 19);
    auto b = inexactValues(fragmentElements, 23);

    auto left = bufferOf(a);
    auto right = packedBufferOf(b, element);
    auto result = outputOf(fragmentElements);

    auto kernel = PackedFragmentProduct {element};
    kernel.a = left;
    kernel.b = right;
    kernel.output = result;
    kernel.prepare();

    check(kernel.fitsPackedSimdMatrix(device));
    check(kernel.isValid());

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, ComputeProgram::simdWidth);
    }

    commands.commit();

    auto stored = roundTripped(b, element);
    auto expected = std::vector<float>(fragmentElements, accumulatorFill);

    for (auto m = 0; m < fragment; ++m)
        for (auto n = 0; n < fragment; ++n)
            for (auto k = 0; k < fragment; ++k)
                expected[(std::size_t) m * fragment + n] +=
                    a[(std::size_t) m * fragment + k]
                    * stored[(std::size_t) k * fragment + n];

    checkMatches(
        readBack(result, fragmentElements), expected, toleranceFor(fragment));
}

// Whether prepare() has a refusal to make here at all. A packed fragment is a
// Metal type the dialect either has or does not, so only that backend can be
// handed a shader it would not compile; D3D12 and Vulkan lower the load to the
// same two-floats-per-lane emulation every other fragment operation lowers to
// and build the kernel whatever the device answered. See
// ComputeProgram::fitsPackedSimdMatrix, which is where that split lives.
bool packedFragmentsCanBeRefused(const ComputeProgram& kernel)
{
    return kernel.source().backend == ShaderBackend::Metal;
}
} // namespace

// A bf16 weight multiplied where it lies. The fragment is loaded straight out
// of the packed buffer - no threadgroup tile of widened floats under it, and so
// neither of the two barriers staging one would need - and the product is the
// mixed-precision instruction into a float accumulator.
auto tPackedBFloat16Product =
    test("SimdMatrix/aPackedBFloat16FragmentMultipliesWithoutStaging") = []
{
    auto& device = Device::shared();

    if (!device.isValid() || !device.supportsBFloat16SimdMatrix())
        return;

    checkPackedFragmentProduct(SimdMatrixElement::BFloat16);
};

// The fp16 sibling, which is the one of the two on eacp's macOS floor.
auto tPackedHalfProduct =
    test("SimdMatrix/aPackedHalfFragmentMultipliesWithoutStaging") = []
{
    auto& device = Device::shared();

    if (!device.isValid() || !device.supportsHalfSimdMatrix())
        return;

    checkPackedFragmentProduct(SimdMatrixElement::Half);
};

// And what happens to a kernel built against the wrong answer: prepare()
// refuses it rather than handing the backend a shader naming a type it has
// never heard of. The refusal is a pipeline that is not valid, and the log
// beside it names the query to ask before recording the load.
//
// Exercised by taking the capability away from a device that has it, which is
// the only way to reach this path on hardware that answers yes - and on Metal
// alone, because it is the only backend with a refusal to make.
auto tPackedLoadRefusedWithoutTheFeature =
    test("SimdMatrix/aPackedLoadIsRefusedWhereTheDeviceSaysNo") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto withoutPackedFragments = ScopedEnv {"EACP_NO_PACKED_SIMD_MATRIX", "1"};

    check(!device.supportsHalfSimdMatrix());
    check(!device.supportsBFloat16SimdMatrix());

    auto kernel = PackedFragmentProduct {SimdMatrixElement::BFloat16};

    // The other half of the same contract, and the only place the emulating
    // backends run it: where the load lowers rather than naming a type, the no
    // above costs the kernel nothing and it builds.
    if (!packedFragmentsCanBeRefused(kernel))
    {
        check(kernel.fitsPackedSimdMatrix(device));

        kernel.prepare();

        check(kernel.isValid());
        return;
    }

    check(!kernel.fitsPackedSimdMatrix(device));

    kernel.prepare();

    check(!kernel.isValid());
    check(!kernel.pipeline().isValid());

    // A kernel that loads no packed fragment is unaffected: the refusal is
    // about what this one asked for, not about the device having said no once.
    auto plain = FragmentProduct {};

    check(plain.fitsPackedSimdMatrix(device));

    plain.prepare();

    check(plain.isValid());
};

// And dispatching the refused kernel anyway, which is what a caller that never
// checked will do. It has to be a no-op: an encoder recorded against with no
// pipeline state bound aborts the process on Metal, and reports nothing at all
// with the validation layer off, so the drop belongs in ComputePass rather than
// in every caller's discipline.
//
// The output buffer is filled first and checked afterwards, so "nothing
// happened" is a claim about the buffer rather than about not having crashed.
auto tRefusedKernelDispatchesNothing =
    test("SimdMatrix/aRefusedKernelDispatchesNothing") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto withoutPackedFragments = ScopedEnv {"EACP_NO_PACKED_SIMD_MATRIX", "1"};

    auto kernel = PackedFragmentProduct {SimdMatrixElement::BFloat16};

    // Nothing is refused where the load lowers to the emulation, so there is no
    // dropped dispatch to watch for: the kernel is the one the packed product
    // tests above already build.
    if (!packedFragmentsCanBeRefused(kernel))
        return;

    auto a = bufferOf(scatteredValues(fragmentElements, 31));
    auto b = packedBufferOf(inexactValues(fragmentElements, 37),
                            SimdMatrixElement::BFloat16);

    auto untouched = std::vector<float>(fragmentElements, 7.5f);
    auto result = bufferOf(untouched);

    kernel.a = a;
    kernel.b = b;
    kernel.output = result;
    kernel.prepare();

    check(!kernel.isValid());

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, ComputeProgram::simdWidth);
    }

    commands.commit();

    checkMatches(readBack(result, fragmentElements), untouched, 0.f);
};

// Whether the float operand of a mixed product is narrowed to the packed
// operand's format, which is the question a tiling decision rests on: it is the
// activation, and losing it to eight significand bits would cost more than the
// staging ever did.
//
// Measured rather than assumed. One operand is exactly one in both formats and
// the other is 1 + 2^-12, which neither holds; eight terms of that come to
// 8.001953125 if the float side survives and to a flat 8 if it does not, and
// the two are a thousand tolerances apart. On the hardware this runs on it
// survives - see the README, where the number is recorded as a measurement and
// not as something the language promises.
auto tMixedProductKeepsTheFloatOperand =
    test("SimdMatrix/aMixedProductDoesNotNarrowTheFloatOperand") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    for (auto element: {SimdMatrixElement::Half, SimdMatrixElement::BFloat16})
    {
        auto native = element == SimdMatrixElement::Half
                          ? device.supportsHalfSimdMatrix()
                          : device.supportsBFloat16SimdMatrix();

        if (!native)
            continue;

        auto a = std::vector<float>(fragmentElements, 1.f + belowSixteenBits);
        auto b = std::vector<float>(fragmentElements, 1.f);

        auto left = bufferOf(a);
        auto right = packedBufferOf(b, element);
        auto result = outputOf(fragmentElements);

        auto kernel = PackedFragmentProduct {element};
        kernel.a = left;
        kernel.b = right;
        kernel.output = result;
        kernel.prepare();

        check(kernel.isValid());

        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, ComputeProgram::simdWidth);
        }

        commands.commit();

        auto kept = accumulatorFill + (float) fragment * (1.f + belowSixteenBits);

        auto narrowed = accumulatorFill + (float) fragment;
        auto expected = std::vector<float>(fragmentElements, kept);

        checkMatches(readBack(result, fragmentElements), expected, 1.0e-5f);
        check(std::abs(kept - narrowed) > 1.0e-3f);
    }
};
