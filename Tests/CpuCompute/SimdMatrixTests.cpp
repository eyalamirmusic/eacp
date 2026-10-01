#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <array>
#include <bit>
#include <cstdint>
#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

// The SIMD-group matrix: a fragment held whole per SIMD group, filled, loaded,
// stored and multiplied. Every case carries its C++ twin; the product's twin
// sums in the fallback's order - the accumulator, then k = 0 to 7 - so it
// compares exactly.

namespace
{
constexpr auto side = simdMatrixSize;
constexpr auto elements = side * side;
constexpr auto unset = -1.f;

using Fragment = std::array<float, elements>;

Vector<float> filledFloats(int count, float value)
{
    auto values = Vector<float> {};
    values.resize(count, value);
    return values;
}

float inexact(int index, int salt)
{
    return (float) (((index * 37 + salt * 11) % 23) - 11) * 0.1f;
}

Vector<float> inexactValues(int count, int salt)
{
    auto values = Vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.add(inexact(i, salt));

    return values;
}

Fragment patchOf(const Vector<float>& values, int offset, int stride)
{
    auto patch = Fragment {};

    for (auto row = 0; row < side; ++row)
        for (auto column = 0; column < side; ++column)
        {
            auto at = offset + row * stride + column;
            patch[row * side + column] = at < values.size() ? values[at] : 0.f;
        }

    return patch;
}

Fragment multiplyAdd(const Fragment& accumulator,
                     const Fragment& left,
                     const Fragment& right)
{
    auto result = Fragment {};

    for (auto row = 0; row < side; ++row)
        for (auto column = 0; column < side; ++column)
        {
            auto sum = accumulator[row * side + column];

            for (auto k = 0; k < side; ++k)
                sum += left[row * side + k] * right[k * side + column];

            result[row * side + column] = sum;
        }

    return result;
}

bool holdsPatch(const Vector<float>& values,
                int offset,
                int stride,
                const Fragment& expected)
{
    auto matches = true;

    for (auto row = 0; row < side; ++row)
        for (auto column = 0; column < side; ++column)
            matches = matches
                      && values[offset + row * stride + column]
                             == expected[row * side + column];

    return matches;
}

bool isInPatch(int index, int offset, int stride)
{
    if (index < offset)
        return false;

    auto row = (index - offset) / stride;
    auto column = (index - offset) % stride;
    return row < side && column < side;
}

int countOutsidePatchesThatChanged(const Vector<float>& values,
                                   std::initializer_list<int> offsets,
                                   int stride)
{
    auto changed = 0;

    for (auto i = 0; i < values.size(); ++i)
    {
        auto inside = false;

        for (auto offset: offsets)
            inside = inside || isInPatch(i, offset, stride);

        if (!inside && values[i] != unset)
            ++changed;
    }

    return changed;
}

bool runsOver(ComputeKernel& kernel, const Bindings& bindings, int count)
{
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());
    return executor.isValid() && executor.dispatch(bindings, count);
}
} // namespace

// ---------------------------------------------------------------------------
// A fill: every element of each SIMD group's fragment, and nothing else.

namespace
{
constexpr auto fillValue = 2.75f;

struct FillKernel final : ComputeKernel
{
    FillKernel()
        : ComputeKernel({2 * simdGroupWidth})
    {
        compile();
    }

    void define() override
    {
        auto filled = simdMatrix(fillValue);
        write(output, simdGroupIndex() * 100u, unsignedInteger(10u), filled);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};
} // namespace

auto tSimdMatrixFill = test("SimdMatrix/aFillSetsEveryElement") = []
{
    auto kernel = FillKernel {};
    auto output = filledFloats(200, unset);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(runsOver(kernel, bindings, 2 * simdGroupWidth));

    auto expected = Fragment {};
    expected.fill(fillValue);

    check(holdsPatch(output, 0, 10, expected));
    check(holdsPatch(output, 100, 10, expected));
    check(countOutsidePatchesThatChanged(output, {0, 100}, 10) == 0);
};

// ---------------------------------------------------------------------------
// A load and a store, each at its own offset and row stride.

namespace
{
constexpr auto sourceStride = 16;
constexpr auto sourceOffset = 3 * sourceStride + 5;
constexpr auto targetStride = 11;
constexpr auto targetOffset = 7;

struct TransferKernel final : ComputeKernel
{
    TransferKernel()
        : ComputeKernel({simdGroupWidth})
    {
        compile();
    }

    void define() override
    {
        auto patch = simdMatrix(input,
                                unsignedInteger((unsigned) sourceOffset),
                                unsignedInteger((unsigned) sourceStride));

        write(output,
              unsignedInteger((unsigned) targetOffset),
              unsignedInteger((unsigned) targetStride),
              patch);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};
} // namespace

auto tSimdMatrixTransfer =
    test("SimdMatrix/aLoadAndAStoreFollowTheirRowStrides") = []
{
    auto kernel = TransferKernel {};
    auto input = inexactValues(sourceStride * sourceStride, 1);
    auto output = filledFloats(targetOffset + targetStride * side, unset);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);
    check(runsOver(kernel, bindings, simdGroupWidth));

    check(holdsPatch(output,
                     targetOffset,
                     targetStride,
                     patchOf(input, sourceOffset, sourceStride)));
    check(countOutsidePatchesThatChanged(output, {targetOffset}, targetStride) == 0);
};

// ---------------------------------------------------------------------------
// The product, D = A * B + C, and the accumulator as its own operand.

namespace
{
constexpr auto productFill = 0.25f;

struct ProductKernel final : ComputeKernel
{
    ProductKernel()
        : ComputeKernel({simdGroupWidth})
    {
        compile();
    }

    void define() override
    {
        auto start = unsignedInteger(0u);
        auto stride = unsignedInteger((unsigned) side);

        auto accumulator = simdMatrix(productFill);
        auto left = simdMatrix(a, start, stride);
        auto right = simdMatrix(b, start, stride);

        multiplyAccumulate(accumulator, left, right);
        write(product, start, stride, accumulator);

        multiplyAccumulate(accumulator, accumulator, right);
        write(squared, start, stride, accumulator);
    }

    Uniform<InputBuffer> a;
    Uniform<InputBuffer> b;
    Uniform<OutputBuffer> product;
    Uniform<OutputBuffer> squared;

    EACP_SHADER(a, b, product, squared)
};
} // namespace

auto tSimdMatrixProduct = test("SimdMatrix/multiplyAddMatchesThe8x8Reference") = []
{
    auto kernel = ProductKernel {};
    auto a = inexactValues(elements, 2);
    auto b = inexactValues(elements, 3);
    auto product = filledFloats(elements, unset);
    auto squared = filledFloats(elements, unset);

    auto bindings = Bindings {};
    bindings.set(kernel.a, a);
    bindings.set(kernel.b, b);
    bindings.set(kernel.product, product);
    bindings.set(kernel.squared, squared);
    check(runsOver(kernel, bindings, simdGroupWidth));

    auto filled = Fragment {};
    filled.fill(productFill);

    auto once = multiplyAdd(filled, patchOf(a, 0, side), patchOf(b, 0, side));
    auto twice = multiplyAdd(once, once, patchOf(b, 0, side));

    check(holdsPatch(product, 0, side, once));
    check(holdsPatch(squared, 0, side, twice));
};

// ---------------------------------------------------------------------------
// Two SIMD groups in one 64-lane group, over two groups: four fragments that
// share nothing.

namespace
{
constexpr auto pairGroupWidth = 2 * simdGroupWidth;
constexpr auto pairGroups = 2;
constexpr auto pairBlocks = pairGroups * 2;

struct SimdGroupPairKernel final : ComputeKernel
{
    SimdGroupPairKernel()
        : ComputeKernel({pairGroupWidth})
    {
        compile();
    }

    void define() override
    {
        auto block = (groupId() * 2u + simdGroupIndex()) * (unsigned) elements;
        auto stride = unsignedInteger((unsigned) side);

        auto accumulator = simdMatrix();
        multiplyAccumulate(
            accumulator, simdMatrix(a, block, stride), simdMatrix(b, block, stride));
        write(output, block, stride, accumulator);
    }

    Uniform<InputBuffer> a;
    Uniform<InputBuffer> b;
    Uniform<OutputBuffer> output;

    EACP_SHADER(a, b, output)
};
} // namespace

auto tSimdMatrixPairs = test("SimdMatrix/eachSimdGroupHoldsItsOwnFragments") = []
{
    auto kernel = SimdGroupPairKernel {};
    auto a = inexactValues(pairBlocks * elements, 4);
    auto b = inexactValues(pairBlocks * elements, 5);
    auto output = filledFloats(pairBlocks * elements, unset);

    auto bindings = Bindings {};
    bindings.set(kernel.a, a);
    bindings.set(kernel.b, b);
    bindings.set(kernel.output, output);
    check(runsOver(kernel, bindings, pairGroups * pairGroupWidth));

    for (auto block = 0; block < pairBlocks; ++block)
    {
        auto at = block * elements;
        auto expected =
            multiplyAdd(Fragment {}, patchOf(a, at, side), patchOf(b, at, side));
        check(holdsPatch(output, at, side, expected), std::to_string(block));
    }
};

// ---------------------------------------------------------------------------
// Divergence, which Metal leaves undefined: a SIMD group with any active lane
// acts whole on its first active lane's offset and stride, and one with none
// does nothing.

namespace
{
constexpr auto divergentWidth = 3 * simdGroupWidth;
constexpr auto firstActiveLane = 37;
constexpr auto divergentSpacing = 300;

struct DivergentKernel final : ComputeKernel
{
    DivergentKernel()
        : ComputeKernel({divergentWidth})
    {
        compile();
    }

    void define() override
    {
        auto lane = localId();
        auto stride = unsignedInteger((unsigned) side);

        ifThen(lane >= (unsigned) firstActiveLane,
               [&]
               {
                   auto patch = simdMatrix(input, lane, stride);
                   auto at = simdGroupIndex() * (unsigned) divergentSpacing
                             + lane % (unsigned) simdGroupWidth;
                   write(output, at, stride, patch);
               });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};
} // namespace

auto tSimdMatrixDivergence =
    test("SimdMatrix/aDivergentLoadTakesTheFirstActiveLanesOffset") = []
{
    auto kernel = DivergentKernel {};
    auto input = inexactValues(200, 6);
    auto output = filledFloats(3 * divergentSpacing, unset);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);
    check(runsOver(kernel, bindings, divergentWidth));

    auto secondStore = divergentSpacing + firstActiveLane % simdGroupWidth;
    auto thirdStore = 2 * divergentSpacing;

    check(holdsPatch(
        output, secondStore, side, patchOf(input, firstActiveLane, side)));
    check(holdsPatch(output, thirdStore, side, patchOf(input, 64, side)));
    check(countOutsidePatchesThatChanged(output, {secondStore, thirdStore}, side)
          == 0);
};

// ---------------------------------------------------------------------------
// D7: a patch reaching past its array reads 0 there, and its store is dropped
// there - in a buffer and in shared memory alike.

namespace
{
constexpr auto shortInput = 40;
constexpr auto shortOutput = 50;
constexpr auto shortTile = 30;
constexpr auto tileFill = 3.5f;

struct OutOfRangeKernel final : ComputeKernel
{
    OutOfRangeKernel()
        : ComputeKernel({simdGroupWidth})
    {
        compile();
    }

    void define() override
    {
        auto lane = localId();
        auto start = unsignedInteger(0u);
        auto stride = unsignedInteger((unsigned) side);
        auto tile = shared<Float>(shortTile);

        write(output, start, stride, simdMatrix(input, start, stride));

        write(tile, unsignedInteger(20u), stride, simdMatrix(tileFill));
        write(staged, start, stride, simdMatrix(tile, unsignedInteger(16u), stride));
        write(tileBack, lane, tile[lane]);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<OutputBuffer> staged;
    Uniform<OutputBuffer> tileBack;

    EACP_SHADER(input, output, staged, tileBack)
};
} // namespace

auto tSimdMatrixOutOfRange =
    test("SimdMatrix/anOutOfRangeLoadReadsZeroAndItsStoreIsDropped") = []
{
    auto kernel = OutOfRangeKernel {};
    auto input = inexactValues(shortInput, 7);
    auto output = filledFloats(shortOutput, unset);
    auto staged = filledFloats(elements, unset);
    auto tileBack = filledFloats(simdGroupWidth, unset);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);
    bindings.set(kernel.staged, staged);
    bindings.set(kernel.tileBack, tileBack);
    check(runsOver(kernel, bindings, simdGroupWidth));

    auto bufferReads = 0;

    for (auto i = 0; i < shortOutput; ++i)
        bufferReads += output[i] == (i < shortInput ? input[i] : 0.f) ? 1 : 0;

    check(bufferReads == shortOutput);

    auto tileValue = [](int element)
    { return element >= 20 && element < shortTile ? tileFill : 0.f; };

    auto tileReads = 0;

    for (auto i = 0; i < elements; ++i)
        tileReads += staged[i] == tileValue(16 + i) ? 1 : 0;

    check(tileReads == elements);

    for (auto lane = 0; lane < simdGroupWidth; ++lane)
        check(tileBack[lane] == tileValue(lane), std::to_string(lane));
};

// ---------------------------------------------------------------------------
// A blocked product staged through a threadgroup tile with barrier() inside a
// recorded loop, as SimdMatrixTests' TiledProduct does: C (8 x 16) = A (8 x K)
// times B (K x 16), K walked in slabs of 8, one SIMD group per 8 columns.

namespace
{
constexpr auto stagedInner = 24;
constexpr auto stagedColumns = 2 * side;
constexpr auto stagedWidth = 2 * simdGroupWidth;
constexpr auto slabA = side * side;

struct StagedProductKernel final : ComputeKernel
{
    StagedProductKernel()
        : ComputeKernel({stagedWidth})
    {
        compile();
    }

    void define() override
    {
        auto lane = localId();
        auto simd = simdGroupIndex();
        auto tile = shared<Float>(slabA + side * stagedColumns);
        auto accumulator = simdMatrix();
        auto k0 = var(0u);

        loop(k0.get() < inner,
             [&]
             {
                 auto row = lane / (unsigned) side;
                 auto depth = lane % (unsigned) side;
                 write(tile, lane, a[row * inner + k0.get() + depth]);

                 for (auto half = 0u; half < 2u; ++half)
                 {
                     auto at = lane + half * (unsigned) stagedWidth;
                     auto bRow = at / (unsigned) stagedColumns;
                     auto bColumn = at % (unsigned) stagedColumns;
                     write(
                         tile,
                         (unsigned) slabA + at,
                         b[(k0.get() + bRow) * (unsigned) stagedColumns + bColumn]);
                 }

                 barrier();

                 auto left = simdMatrix(
                     tile, unsignedInteger(0u), unsignedInteger((unsigned) side));
                 auto right = simdMatrix(tile,
                                         simd * (unsigned) side + (unsigned) slabA,
                                         unsignedInteger((unsigned) stagedColumns));
                 multiplyAccumulate(accumulator, left, right);

                 barrier();
                 k0 += (unsigned) side;
             });

        write(output,
              simd * (unsigned) side,
              unsignedInteger((unsigned) stagedColumns),
              accumulator);
    }

    Uniform<InputBuffer> a;
    Uniform<InputBuffer> b;
    Uniform<OutputBuffer> output;
    Uniform<UInt> inner;

    EACP_SHADER(a, b, output, inner)
};
} // namespace

auto tSimdMatrixStaged = test("SimdMatrix/aTileStagedInsideALoopWithBarriers") = []
{
    auto kernel = StagedProductKernel {};
    kernel.inner = (std::uint32_t) stagedInner;

    auto a = inexactValues(side * stagedInner, 8);
    auto b = inexactValues(stagedInner * stagedColumns, 9);
    auto output = filledFloats(side * stagedColumns, unset);

    auto bindings = Bindings {};
    bindings.set(kernel.a, a);
    bindings.set(kernel.b, b);
    bindings.set(kernel.output, output);
    check(runsOver(kernel, bindings, stagedWidth));

    auto matching = 0;

    for (auto m = 0; m < side; ++m)
        for (auto n = 0; n < stagedColumns; ++n)
        {
            auto sum = 0.f;

            for (auto k = 0; k < stagedInner; ++k)
                sum += a[m * stagedInner + k] * b[k * stagedColumns + n];

            matching += output[m * stagedColumns + n] == sum ? 1 : 0;
        }

    check(matching == side * stagedColumns);
};

// ---------------------------------------------------------------------------
// A packed half or bf16 load: the offset and the row stride count in sixteen-bit
// elements, element i is half i % 2 of word i / 2, widened through the helper
// the shader's own packed reads use. Odd offset and stride so the parity moves
// along every row, and a buffer that ends inside the patch so its last row
// reads 0. The packed fragment is the right operand of a product with the
// identity, which hands it back exactly.

namespace
{
constexpr auto packedOffset = 5;
constexpr auto packedStride = 11;
constexpr auto packedWords = 40;

struct PackedSource
{
    float value = 0.f;
    bool exact = false;
};

std::array<PackedSource, 10> packedSources(SimdMatrixElement element)
{
    if (element == SimdMatrixElement::Half)
        return {{{1.5f, true},
                 {-2.25f, true},
                 {65504.f, true},
                 {0x1p-14f, true},
                 {0x1p-24f, true},
                 {-7.75f, true},
                 {0.1f, false},
                 {1.f / 3.f, false},
                 {-2.7182817f, false},
                 {1000.1f, false}}};

    return {{{1.5f, true},
             {-2.25f, true},
             {0x1p100f, true},
             {0x1p-126f, true},
             {-384.f, true},
             {0.5f, true},
             {0.1f, false},
             {1.f / 3.f, false},
             {-3.1415927f, false},
             {1000.1f, false}}};
}

std::uint32_t packedPair(SimdMatrixElement element, float low, float high)
{
    return element == SimdMatrixElement::Half ? packHalf2({low, high})
                                              : packBFloat16x2({low, high});
}

float widenedElement(SimdMatrixElement element,
                     const Vector<float>& words,
                     int index)
{
    auto word = index / 2;
    auto bits = word < words.size() ? std::bit_cast<std::uint32_t>(words[word]) : 0u;
    auto parity = (std::uint32_t) (index % 2);

    return element == SimdMatrixElement::Half ? readHalf(bits, parity)
                                              : readBFloat16(bits, parity);
}

struct PackedLoadKernel final : ComputeKernel
{
    explicit PackedLoadKernel(SimdMatrixElement elementToUse)
        : ComputeKernel({simdGroupWidth})
        , element(elementToUse)
    {
        compile();
    }

    void define() override
    {
        auto start = unsignedInteger(0u);
        auto stride = unsignedInteger((unsigned) side);
        auto offset = unsignedInteger((unsigned) packedOffset);
        auto rowStride = unsignedInteger((unsigned) packedStride);

        auto packed = element == SimdMatrixElement::Half
                          ? simdMatrixHalf(weights, offset, rowStride)
                          : simdMatrixBFloat16(weights, offset, rowStride);

        auto accumulator = simdMatrix();
        multiplyAccumulate(accumulator, simdMatrix(identity, start, stride), packed);
        write(output, start, stride, accumulator);
    }

    SimdMatrixElement element;

    Uniform<InputBuffer> identity;
    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(identity, weights, output)
};
} // namespace

auto tSimdMatrixPackedLoad =
    test("SimdMatrix/aPackedLoadWidensThroughTheHelpers") = []
{
    for (auto element: {SimdMatrixElement::Half, SimdMatrixElement::BFloat16})
    {
        auto name = element == SimdMatrixElement::Half ? "half" : "bf16";
        auto sources = packedSources(element);
        auto sourceAt = [&](int index) { return sources[index % sources.size()]; };

        auto weights = Vector<float> {};

        for (auto word = 0; word < packedWords; ++word)
            weights.add(std::bit_cast<float>(packedPair(
                element, sourceAt(2 * word).value, sourceAt(2 * word + 1).value)));

        auto identity = filledFloats(elements, 0.f);

        for (auto i = 0; i < side; ++i)
            identity[i * side + i] = 1.f;

        auto kernel = PackedLoadKernel {element};
        auto output = filledFloats(elements, unset);

        auto bindings = Bindings {};
        bindings.set(kernel.identity, identity);
        bindings.set(kernel.weights, weights);
        bindings.set(kernel.output, output);
        check(runsOver(kernel, bindings, simdGroupWidth), name);

        auto matching = 0;
        auto pastTheEnd = 0, zeroPastTheEnd = 0;
        auto exact = 0, exactAsWritten = 0;
        auto inexact = 0, narrowed = 0;

        for (auto row = 0; row < side; ++row)
            for (auto column = 0; column < side; ++column)
            {
                auto index = packedOffset + row * packedStride + column;
                auto value = output[row * side + column];
                matching += value == widenedElement(element, weights, index) ? 1 : 0;

                if (index / 2 >= packedWords)
                {
                    ++pastTheEnd;
                    zeroPastTheEnd += value == 0.f ? 1 : 0;
                    continue;
                }

                auto source = sourceAt(index);

                if (source.exact)
                {
                    ++exact;
                    exactAsWritten += value == source.value ? 1 : 0;
                }
                else
                {
                    ++inexact;
                    narrowed += value != source.value ? 1 : 0;
                }
            }

        check(matching == elements, name);
        check(pastTheEnd == side && zeroPastTheEnd == pastTheEnd, name);
        check(exact > 0 && exactAsWritten == exact, name);
        check(inexact > 0 && narrowed == inexact, name);
    }
};

// ---------------------------------------------------------------------------
// A fragment needs whole SIMD groups: a group of 48 leaves a partial second
// one, and a group of 16 is less than one. Both are refused at plan time,
// naming the thread count. Recorded on a bare builder, since emitting either
// trips the emitter's own assertion.

auto tSimdMatrixPartialGroup =
    test("SimdMatrix/aGroupOfPartialSimdGroupsIsRefused") = []
{
    for (auto threads: {48, 16})
    {
        auto builder = ShaderBuilder {};
        builder.setThreadGroupShape({threads});

        auto input = builder.inputBuffer();
        auto output = builder.outputBuffer();
        auto start = builder.unsignedInteger(0u);
        auto stride = builder.unsignedInteger((unsigned) side);

        auto accumulator = builder.simdMatrix();
        auto patch = builder.simdMatrix(input, start, stride);
        builder.multiplyAccumulate(accumulator, patch, patch);
        builder.write(output, start, stride, accumulator);

        auto executor = Executor {builder.graph()};
        const auto& reason = executor.reason();

        check(!executor.isValid(), std::to_string(threads));
        check(reason.find("whole SIMD groups") != std::string::npos, reason);
        check(reason.find(std::to_string(threads)) != std::string::npos, reason);
    }

    auto whole = ShaderBuilder {};
    whole.setThreadGroupShape({3 * simdGroupWidth});

    auto output = whole.outputBuffer();
    whole.write(output,
                whole.unsignedInteger(0u),
                whole.unsignedInteger((unsigned) side),
                whole.simdMatrix());

    auto executor = Executor {whole.graph()};
    check(executor.isValid(), executor.reason());
};
