#include <eacp/GPU/GPU.h>

#include <eacp/Core/Platform/Platform.h>

#include <ResEmbed/ResEmbed.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace eacp;
using namespace GPU;

namespace
{
// Per-dispatch constants, matching the Params struct in the shaders.
struct Params
{
    float scale;
    std::uint32_t count;
};

ShaderSource loadComputeShader()
{
    auto fileName = Platform::isWindows() ? "Compute.hlsl" : "Compute.metal";

    auto shader = ResEmbed::get(fileName, "ComputeShaders");

    if (!shader)
        throw std::runtime_error(std::string("Compute: embedded ") + fileName
                                 + " not found");

    auto source = Platform::isWindows() ? ShaderSource::hlsl(shader.toString())
                                        : ShaderSource::msl(shader.toString());

    return source.withCompute("computeMain");
}
} // namespace

// Headless compute: multiplies each element of an input array by a scalar on the
// GPU and reads the result back. No window, no drawable - just a command buffer.
int main()
{
    auto& device = Device::shared();

    if (!device.isValid())
    {
        std::printf("Compute: no GPU device available; skipping.\n");
        return 0;
    }

    const float input[] = {1, 2, 3, 4, 5, 6, 7, 8};
    constexpr auto count = (int) (sizeof(input) / sizeof(input[0]));

    auto inputBuffer = device.makeBuffer(input, BufferUsage::Storage);
    auto outputBuffer = device.makeBuffer(sizeof(input), BufferUsage::Storage);

    auto library = device.makeShaderLibrary(loadComputeShader());
    auto pipeline = device.makeComputePipeline(library);

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.setPipeline(pipeline);
        pass.setInputBuffer(inputBuffer, 0);
        pass.setOutputBuffer(outputBuffer, 1);
        pass.setUniform(Params {2.0f, (std::uint32_t) count});
        pass.dispatch(count);
    }

    commands.commit();

    float result[count] = {};
    outputBuffer.read(result, sizeof(result));

    std::printf("Compute: out[i] = in[i] * 2\n");

    for (auto i = 0; i < count; ++i)
        std::printf("  %g -> %g\n", input[i], result[i]);

    return 0;
}
