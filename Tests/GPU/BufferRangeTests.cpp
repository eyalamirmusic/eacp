#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

auto tBufferRangedTransfers = test("GPU/bufferRangedTransfers") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    float initial[16] = {};

    for (auto i = 0; i < 16; ++i)
        initial[i] = (float) i;

    auto buffer = device.makeBuffer(initial, sizeof(initial), BufferUsage::Storage);
    check(buffer.isValid());

    const float patch[] = {100.0f, 101.0f, 102.0f, 103.0f};
    buffer.update(patch, sizeof(patch), 4 * sizeof(float));

    float whole[16] = {};
    buffer.read(whole, sizeof(whole));

    for (auto i = 0; i < 16; ++i)
    {
        auto patched = i >= 4 && i < 8;
        check(whole[i] == (patched ? 96.0f + (float) i : (float) i));
    }

    float slice[4] = {};
    buffer.read(slice, sizeof(slice), 8 * sizeof(float));

    for (auto i = 0; i < 4; ++i)
        check(slice[i] == (float) (8 + i));

    const float tail[] = {200.0f, 201.0f, 202.0f, 203.0f};
    buffer.update(tail, sizeof(tail), 14 * sizeof(float));

    float clamped[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    buffer.read(clamped, sizeof(clamped), 14 * sizeof(float));
    check(clamped[0] == 200.0f);
    check(clamped[1] == 201.0f);
    check(clamped[2] == -1.0f);
    check(clamped[3] == -1.0f);

    float untouched[2] = {-1.0f, -1.0f};
    buffer.read(untouched, sizeof(untouched), sizeof(initial));
    buffer.update(tail, sizeof(tail), sizeof(initial));
    check(untouched[0] == -1.0f);
    check(untouched[1] == -1.0f);
};
