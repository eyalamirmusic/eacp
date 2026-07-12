#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

// Texture::update re-uploads into the existing resource — the per-frame path for
// video and camera streams. Checks the texture stays valid and sized through a
// tightly packed update, a padded-stride update and a null no-op. Pixel content
// is not read back (no texture readback exists), matching the validity-and-size
// fidelity of the other texture tests. Self-skips without a GPU device.
auto tTextureUpdates = test("GPU/textureUpdates") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const std::uint32_t initial[] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};

    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 2;
    descriptor.filter = TextureFilter::Nearest;

    auto texture = device.makeTexture(descriptor, initial);
    check(texture.isValid());

    const std::uint32_t next[] = {0xffffffff, 0xff000000, 0xff0000ff, 0xff00ff00};
    texture.update(next);
    check(texture.isValid());
    check(texture.width() == 2);
    check(texture.height() == 2);

    // A source whose rows are wider than width * 4: two pixels then two padding
    // bytes per row, exercising the bytesPerRow stride path.
    const std::uint8_t padded[] = {
        0,   0, 0, 255, 255, 255, 255, 255, 0xAB, 0xCD, // row 0: 2 px + 2 pad
        255, 0, 0, 255, 0,   255, 0,   255, 0xAB, 0xCD, // row 1: 2 px + 2 pad
    };
    texture.update(padded, 10);
    check(texture.isValid());

    // A null update is a safe no-op.
    texture.update(nullptr);
    check(texture.isValid());
};
