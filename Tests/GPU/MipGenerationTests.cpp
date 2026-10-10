#include "Common.h"

#include <functional>

// Frame::generateMipmaps and CommandBuffer::generateMipmaps: every level below
// 0 of every slice filled on the GPU from level 0. Each image starts in a
// colour of its own and level 0 of each slice in another, so a chain built
// from the wrong slice, or a level left alone, reads back as the wrong colour.
// A volume's level 0 is two colours along z, so only a filter that also runs
// in z gives the mean at the bottom of its chain.
//
// Self-skips without a device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
bool canTest()
{
    return Device::shared().isValid()
           && Device::shared().supportsMipGeneration(TextureFormat::RGBA8Unorm);
}

struct FrameRunner final : GPUView
{
    FrameRunner() { setSampleCount(1); }

    void render(Frame& frame) override
    {
        draw(frame);
        frame.beginPass({Graphics::Color::black()});
    }

    std::function<void(Frame&)> draw = [](Frame&) {};
};

void runFrame(std::function<void(Frame&)> draw)
{
    auto view = FrameRunner {};
    view.draw = std::move(draw);
    view.setBounds({0.f, 0.f, 4.f, 4.f});
    view.renderToImage(1.f);
}

int extent(int size, int level)
{
    return std::max(1, size >> level);
}

int imagesAt(const Texture& texture, int level)
{
    return texture.kind() == TextureType::ThreeD
               ? extent(texture.volumeDepth(), level)
               : texture.slices();
}

Graphics::Color asColor(std::uint32_t rgba)
{
    const auto channel = [&](int shift)
    { return (float) ((rgba >> shift) & 0xffu) / 255.f; };

    return {channel(0), channel(8), channel(16), channel(24)};
}

// What a level 0 slice is drawn in, and what every lower level starts as.
std::uint32_t sliceColor(int slice)
{
    return 0xff000000u | (std::uint32_t) ((slice * 40 + 20) & 0xff)
           | ((std::uint32_t) ((200 - slice * 30) & 0xff) << 8);
}

constexpr auto staleColor = 0xffff00ffu;

void clearImage(
    Frame& frame, const Texture& texture, int level, int slice, std::uint32_t color)
{
    auto target = ColorTarget {&texture};
    target.clearColor = asColor(color);
    target.level = level;
    target.slice = slice;

    frame.beginMultiTargetPass(std::span<const ColorTarget> {&target, 1});
}

// Level 0 of slice s in sliceColor(s) (of z, on a volume, in `zColor`), and
// every level below it in the stale colour.
void fillLevelZero(Frame& frame,
                   const Texture& texture,
                   std::function<std::uint32_t(int)> zColor = sliceColor)
{
    for (auto level = 0; level < texture.mipLevels(); ++level)
        for (auto slice = 0; slice < imagesAt(texture, level); ++slice)
            clearImage(frame,
                       texture,
                       level,
                       slice,
                       level == 0 ? zColor(slice) : staleColor);
}

Vector<std::uint32_t> readImage(const Texture& texture, int level, int slice)
{
    const auto volume = texture.kind() == TextureType::ThreeD;

    auto region = TextureRegion::wholeLevel(level, volume ? 0 : slice);

    if (volume)
    {
        region.origin.z = slice;
        region.size.depth = 1;
    }

    auto values = Vector<std::uint32_t> {};
    values.resize((std::size_t) (extent(texture.width(), level)
                                 * extent(texture.height(), level)));
    texture.readSubresource(region, values.data());
    return values;
}

bool isFilledWith(const Vector<std::uint32_t>& values, std::uint32_t expected)
{
    const auto close = [](std::uint32_t a, std::uint32_t b)
    {
        for (auto shift = 0; shift < 32; shift += 8)
        {
            const auto x = (int) ((a >> shift) & 0xffu);
            const auto y = (int) ((b >> shift) & 0xffu);

            if (x < y - 2 || x > y + 2)
                return false;
        }

        return true;
    };

    if (values.empty())
        return false;

    for (auto value: values)
        if (!close(value, expected))
            return false;

    return true;
}

TextureDescriptor describeTarget(TextureType kind, int size, int layers = 1)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;
    descriptor.kind = kind;
    descriptor.arrayLength = layers;
    descriptor.mipLevels = mipLevelCount(size, size);
    descriptor.renderTarget = true;
    return descriptor;
}

// Every level of every slice holds that slice's level 0 colour.
bool everyLevelHoldsItsSlice(const Texture& texture)
{
    for (auto level = 1; level < texture.mipLevels(); ++level)
        for (auto slice = 0; slice < texture.slices(); ++slice)
            if (!isFilledWith(readImage(texture, level, slice), sliceColor(slice)))
                return false;

    return true;
}

void generateFromLevelZero(const Texture& texture)
{
    runFrame(
        [&](Frame& frame)
        {
            fillLevelZero(frame, texture);
            frame.generateMipmaps(texture);
        });
}
} // namespace

auto tMipGenerationDeviceAnswers =
    test("MipGeneration/theDeviceSaysWhichFormatsItFilters") = []
{
    if (!Device::shared().isValid())
        return;

    auto& device = Device::shared();

    check(!device.supportsMipGeneration(TextureFormat::BC1RGBA));
    check(!device.supportsMipGeneration(TextureFormat::BC7RGBA));
    check(!device.supportsMipGeneration(TextureFormat::R32Uint));

    if constexpr (Platform::isApple())
    {
        check(device.supportsMipGeneration(TextureFormat::RGBA8Unorm));
        check(device.supportsMipGeneration(TextureFormat::BGRA8Unorm));
        check(device.supportsMipGeneration(TextureFormat::RGBA16Float));
        check(device.supportsMipGeneration(TextureFormat::R8Unorm));
    }
};

auto tMipGeneration2D = test("MipGeneration/aTargetsChainIsFilledFromLevelZero") = []
{
    if (!canTest())
        return;

    auto texture =
        Device::shared().makeTexture(describeTarget(TextureType::TwoD, 16));

    check(texture.isValid() && texture.mipLevels() == 5);

    if (!texture.isValid())
        return;

    generateFromLevelZero(texture);

    check(isFilledWith(readImage(texture, 0, 0), sliceColor(0)));
    check(everyLevelHoldsItsSlice(texture));
};

// Level 0 half one colour and half another, columns apart: level 1 keeps the
// halves, and the 1x1 bottom of the chain is their mean.
auto tMipGenerationFilters =
    test("MipGeneration/eachLevelIsTheMeanOfTheOneAbove") = []
{
    if (!canTest())
        return;

    constexpr auto size = 8;
    constexpr auto left = 0xff0000c8u;
    constexpr auto right = 0xffc80000u;

    auto pixels = Vector<std::uint32_t> {};

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            pixels.add(x < size / 2 ? left : right);

    auto descriptor = describeTarget(TextureType::TwoD, size);
    auto texture = Device::shared().makeTexture(descriptor);

    check(texture.isValid());

    if (!texture.isValid())
        return;

    texture.updateSubresource(TextureRegion::wholeLevel(0), pixels.data());

    runFrame([&](Frame& frame) { frame.generateMipmaps(texture); });

    const auto levelOne = readImage(texture, 1, 0);

    check(levelOne.size() == 16);

    if (levelOne.size() == 16)
    {
        check(isFilledWith({levelOne[0], levelOne[5], levelOne[9]}, left));
        check(isFilledWith({levelOne[2], levelOne[7], levelOne[15]}, right));
    }

    check(isFilledWith(readImage(texture, 3, 0), 0xff640064u));
};

auto tMipGenerationCube = test("MipGeneration/aCubesFacesEachGetTheirOwnChain") = []
{
    if (!canTest())
        return;

    auto texture =
        Device::shared().makeTexture(describeTarget(TextureType::Cube, 8));

    check(texture.isValid() && texture.slices() == 6 && texture.mipLevels() == 4);

    if (!texture.isValid())
        return;

    generateFromLevelZero(texture);

    check(everyLevelHoldsItsSlice(texture));
};

auto tMipGenerationArray =
    test("MipGeneration/anArraysLayersEachGetTheirOwnChain") = []
{
    if (!canTest())
        return;

    auto texture =
        Device::shared().makeTexture(describeTarget(TextureType::TwoDArray, 8, 3));

    check(texture.isValid() && texture.slices() == 3);

    if (!texture.isValid())
        return;

    generateFromLevelZero(texture);

    check(everyLevelHoldsItsSlice(texture));
};

auto tMipGenerationCubeArray =
    test("MipGeneration/aCubeArraysFacesEachGetTheirOwnChain") = []
{
    if (!canTest() || !Device::shared().supportsCubeArrays())
        return;

    auto texture =
        Device::shared().makeTexture(describeTarget(TextureType::CubeArray, 4, 2));

    check(texture.isValid() && texture.slices() == 12);

    if (!texture.isValid())
        return;

    generateFromLevelZero(texture);

    check(everyLevelHoldsItsSlice(texture));
};

// z 0 and 1 in one colour, 2 and 3 in another: level 1 keeps the two apart
// along z, and level 2, one image, is their mean.
auto tMipGenerationVolume = test("MipGeneration/aVolumeIsFilteredAlongZToo") = []
{
    if (!canTest())
        return;

    constexpr auto front = 0xff0000c8u;
    constexpr auto back = 0xffc80000u;

    auto descriptor = describeTarget(TextureType::ThreeD, 4);
    descriptor.volumeDepth = 4;
    descriptor.mipLevels = 3;

    auto texture = Device::shared().makeTexture(descriptor);

    check(texture.isValid() && texture.volumeDepth() == 4);

    if (!texture.isValid())
        return;

    runFrame(
        [&](Frame& frame)
        {
            fillLevelZero(
                frame, texture, [](int z) { return z < 2 ? front : back; });
            frame.generateMipmaps(texture);
        });

    check(isFilledWith(readImage(texture, 1, 0), front));
    check(isFilledWith(readImage(texture, 1, 1), back));
    check(isFilledWith(readImage(texture, 2, 0), 0xff640064u));
};

// A sampled texture whose CPU-built chain goes stale when level 0 is
// replaced, and is rebuilt on the GPU - here on a CommandBuffer of its own.
auto tMipGenerationAfterUpdate =
    test("MipGeneration/anUpdatedLevelZeroIsRegenerated") = []
{
    if (!canTest())
        return;

    constexpr auto size = 8;
    constexpr auto before = 0xff204060u;
    constexpr auto after = 0xff6040c0u;

    auto pixels = Vector<std::uint32_t> {};
    pixels.resize((std::size_t) (size * size), before);

    auto descriptor = TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;
    descriptor.mipmapped = true;

    auto texture = Device::shared().makeTexture(descriptor, pixels.data());

    check(texture.isValid() && texture.mipLevels() == 4);

    if (!texture.isValid())
        return;

    for (auto& pixel: pixels)
        pixel = after;

    texture.updateSubresource(TextureRegion::wholeLevel(0), pixels.data());

    check(isFilledWith(readImage(texture, 2, 0), before));

    auto commands = Device::shared().makeCommandBuffer();
    commands.generateMipmaps(texture);
    commands.commit();

    for (auto level = 0; level < texture.mipLevels(); ++level)
        check(isFilledWith(readImage(texture, level, 0), after));
};

// Levels the GPU has never written, asked for with no pixels, filled from a
// level 0 a subresource upload supplied.
auto tMipGenerationFromSubresources =
    test("MipGeneration/aChainNamedOnlyByLevelZeroIsFilled") = []
{
    if (!canTest())
        return;

    constexpr auto size = 8;
    constexpr auto color = 0xff30a050u;

    auto pixels = Vector<std::uint32_t> {};
    pixels.resize((std::size_t) (size * size * 2), color);

    auto descriptor = TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;
    descriptor.kind = TextureType::TwoDArray;
    descriptor.arrayLength = 2;
    descriptor.mipLevels = mipLevelCount(size, size);

    const TextureSubresource levelZero[] = {
        {0, 0, pixels.data(), size * 4, 0},
        {0, 1, pixels.data(), size * 4, 0},
    };

    auto texture =
        Device::shared().makeTextureFromSubresources(descriptor, levelZero);

    check(texture.isValid() && texture.mipLevels() == 4);

    if (!texture.isValid())
        return;

    runFrame([&](Frame& frame) { frame.generateMipmaps(texture); });

    for (auto slice = 0; slice < 2; ++slice)
        for (auto level = 1; level < texture.mipLevels(); ++level)
            check(isFilledWith(readImage(texture, level, slice), color));
};

// An integer target is refused with a trace and nothing recorded, which under
// the API validation layers is the difference between a clean frame and an
// assert.
auto tMipGenerationRefusesIntegers =
    test("MipGeneration/anIntegerFormatIsRefused") = []
{
    if (!canTest())
        return;

    auto descriptor = describeTarget(TextureType::TwoD, 4);
    descriptor.format = TextureFormat::R32Uint;

    auto texture = Device::shared().makeTexture(descriptor);

    check(texture.isValid() && texture.mipLevels() == 3);
    check(!Device::shared().supportsMipGeneration(texture.format()));

    if (!texture.isValid())
        return;

    runFrame([&](Frame& frame) { frame.generateMipmaps(texture); });
};
