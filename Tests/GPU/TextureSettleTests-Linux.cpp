#include "Common.h"

#include <eacp/GPU/Vulkan/VulkanContext.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
TextureDescriptor unwritten(int size)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;
    return descriptor;
}
} // namespace

// An image no upload wrote has to leave UNDEFINED before anything binds it. The
// barrier is queued rather than submitted, so a burst of such textures costs
// nothing until the next recording carries all of them at once - what a glyph
// atlas building its pages does, and what used to cost a submission each.
auto tABurstOfUnwrittenTexturesCostsNoSubmission =
    test("TextureSettle/aBurstOfUnwrittenTexturesCostsNoSubmission") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto& context = getVulkanContext(device);
    const auto before = context.submissionCount();

    auto textures = Vector<Texture> {};

    for (auto i = 0; i < 16; ++i)
        textures.add(device.makeTexture(unwritten(8)));

    for (auto& texture: textures)
        check(texture.isValid());

    check(context.submissionCount() == before);

    auto read = Array<unsigned char, 8 * 8 * 4> {};
    textures[0].read(read.data());

    // The read is the one recording, and it settled all sixteen on its way in.
    check(context.submissionCount() == before + 1);
};

// The deferred barrier has to be recorded before whatever else the recording it
// joins does with the image, upload and read alike.
auto tADeferredTextureTakesAnUploadAndAReadBack =
    test("TextureSettle/aDeferredTextureTakesAnUploadAndAReadBack") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto size = 4;

    auto textures = Vector<Texture> {};

    for (auto i = 0; i < 4; ++i)
        textures.add(device.makeTexture(unwritten(size)));

    auto pixels = Array<unsigned char, size * size * 4> {};

    for (auto i = 0; i < pixels.size(); ++i)
        pixels[i] = (unsigned char) (i * 5 + 3);

    // The last of the burst, so three transitions are still owed when it runs.
    textures[3].update(pixels.data());

    auto read = Array<unsigned char, size * size * 4> {};
    textures[3].read(read.data());

    check(read == pixels);
};

// A texture destroyed before the transition it is owed reaches a recording must
// not leave its image behind in the queue.
auto tADeferredTextureCanBeDestroyedFirst =
    test("TextureSettle/aDeferredTextureCanBeDestroyedFirst") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    {
        auto discarded = device.makeTexture(unwritten(8));
        check(discarded.isValid());
    }

    auto survivor = device.makeTexture(unwritten(8));

    auto read = Array<unsigned char, 8 * 8 * 4> {};
    survivor.read(read.data());

    check(survivor.isValid());
};
