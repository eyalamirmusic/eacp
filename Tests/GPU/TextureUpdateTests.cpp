#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

// No texture readback exists, so only validity and size can be checked.
auto tTextureUpdates = test("GPU/textureUpdates") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const std::uint32_t initial[] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};

    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 2;

    auto texture = device.makeTexture(descriptor, initial);
    check(texture.isValid());

    const std::uint32_t next[] = {0xffffffff, 0xff000000, 0xff0000ff, 0xff00ff00};
    texture.update(next);
    check(texture.isValid());
    check(texture.width() == 2);
    check(texture.height() == 2);

    // Rows deliberately padded past width * 4 to exercise the bytesPerRow stride.
    const std::uint8_t padded[] = {
        0,   0, 0, 255, 255, 255, 255, 255, 0xAB, 0xCD,
        255, 0, 0, 255, 0,   255, 0,   255, 0xAB, 0xCD,
    };
    texture.update(padded, 10);
    check(texture.isValid());

    texture.update(nullptr);
    check(texture.isValid());
};
