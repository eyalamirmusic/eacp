#include "Common.h"

#include <eacp/GPU/OpenGL/GLBackend-Linux.h>
#include <eacp/GPU/OpenGL/GLContext-Linux.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

// The one GL-specific test the plan asks for (D12): everything else is the
// existing suite run again. What it pins is not a number - a driver is allowed
// to offer whatever it offers - but the shape of the answer: what the context
// says it can do, printed so a lane's log names the profile it actually ran,
// and the handful of implications that would make a capability set incoherent.
namespace
{
const GLCapabilities* glTestCapabilities()
{
    auto& device = Device::shared();

    if (!device.isValid() || device.backendName() != "OpenGL")
        return nullptr;

    return &getGLContext(device).getCapabilities();
}

void glLogFlag(const char* name, bool value)
{
    LOG("  ", name, ": ", value ? "yes" : "no");
}
} // namespace

auto tGLCapabilitiesAreReported = test("GLCapability/theContextSaysWhatItCanDo") = []
{
    const auto* caps = glTestCapabilities();

    if (caps == nullptr)
        return;

    LOG("OpenGL context: ",
        caps->isES ? "ES " : "core ",
        caps->version,
        " - ",
        caps->renderer);
    LOG("  vendor: ", caps->vendor);
    LOG("  version string: ", caps->versionString);
    LOG("  shading language: ", caps->shadingLanguageVersion);
    LOG("  GLSL target: ",
        caps->glslTarget().version,
        caps->glslTarget().isES() ? " es" : " core");

    glLogFlag("computeShaders", caps->computeShaders);
    glLogFlag("storageBuffers", caps->storageBuffers);
    glLogFlag("imageLoadStore", caps->imageLoadStore);
    glLogFlag("bufferStorage", caps->bufferStorage);
    glLogFlag("bufferStorageIsEXT", caps->bufferStorageIsEXT);
    glLogFlag("textureStorage", caps->textureStorage);
    glLogFlag("timerQuery", caps->timerQuery);
    glLogFlag("timerQueryIsEXT", caps->timerQueryIsEXT);
    glLogFlag("explicitBindings", caps->explicitBindings);
    glLogFlag("separateShaderObjects", caps->separateShaderObjects);
    glLogFlag("bptc", caps->bptc);
    glLogFlag("s3tc", caps->s3tc);
    glLogFlag("clipControl", caps->clipControl);
    glLogFlag("debugOutput", caps->debugOutput);
    glLogFlag("bgraFormat", caps->bgraFormat);
    glLogFlag("getTexImage", caps->getTexImage);
    glLogFlag("getBufferSubData", caps->getBufferSubData);
    glLogFlag("floatRenderTargets", caps->floatRenderTargets);
    glLogFlag("halfFloatRenderTargets", caps->halfFloatRenderTargets);

    LOG("  maxSamples: ", caps->maxSamples);
    LOG("  maxTextureSize: ", caps->maxTextureSize);
    LOG("  maxThreadgroupMemory: ", caps->maxThreadgroupMemory);
    LOG("  uniformBufferOffsetAlignment: ", caps->uniformBufferOffsetAlignment);
    LOG("  storageBufferOffsetAlignment: ", caps->storageBufferOffsetAlignment);
};

// The floor is GL 3.3 core and ES 3.0, because everything below it needs a
// different uniform model from the one every eacp shader assumes.
auto tGLIsAtOrAboveTheFloor = test("GLCapability/theContextIsAtOrAboveTheFloor") = []
{
    const auto* caps = glTestCapabilities();

    if (caps == nullptr)
        return;

    check(caps->version >= (caps->isES ? 300 : 330),
          "a context below the floor should not have been taken at all");

    const auto target = caps->glslTarget();

    check(target.isES() == caps->isES, "the target is this context's profile");
    check(target.version <= caps->version,
          "the lowering is asked for a version this context accepts");
    check(!caps->renderer.empty(), "a valid context names its renderer");
};

// The compute tier is one thing, not three: a context with a compute stage has
// the storage buffers and the image stores every kernel eacp emits writes
// through, since all three arrive together at GL 4.3 and ES 3.1.
auto tGLComputeImpliesItsResources =
    test("GLCapability/computeImpliesStorageBuffersAndImageStore") = []
{
    const auto* caps = glTestCapabilities();

    if (caps == nullptr)
        return;

    if (!caps->computeShaders)
        return;

    check(caps->storageBuffers, "a compute stage comes with std430 buffers");
    check(caps->imageLoadStore, "a compute stage comes with image stores");
    check(caps->explicitBindings,
          "layout(binding = N) is older than the compute stage on both profiles");
    check(caps->maxThreadgroupMemory > 0,
          "a compute stage has shared memory to declare arrays in");
    check(caps->glslTarget().allowsCompute(),
          "the target picked for a compute context can spell a kernel");
};

// What Device answers has to be the same answer, since that is the only one
// anything above the backend ever sees.
auto tGLDeviceAgreesWithTheContext =
    test("GLCapability/theDeviceReportsWhatTheContextSaid") = []
{
    const auto* caps = glTestCapabilities();

    if (caps == nullptr)
        return;

    auto& device = Device::shared();

    // Not the context's compute stage, which llvmpipe has: the GL kernel tier
    // is a later stage and the Device says so until it is built (plan.md D9).
    check(!device.supportsCompute(),
          "no kernel runs on the GL backend until its compute tier is built");

    check(caps->maxSamples >= 1, "a valid context takes at least one sample");
    check(device.supportsSampleCount(1), "one sample is never refused");
    check(device.supportsSampleCount(caps->maxSamples),
          "the count the context named is one the device takes");
    check(!device.supportsSampleCount(caps->maxSamples * 2),
          "past what the context named is refused rather than rounded down");

    check(device.supportsBlockCompression() == (caps->s3tc && caps->bptc),
          "block compression is both families or neither");

    // One number for both binds, which is the larger of the two grids.
    const auto alignment = device.storageBufferOffsetAlignment();

    check(alignment >= caps->uniformBufferOffsetAlignment,
          "the reported alignment satisfies a uniform-block bind");

    if (caps->storageBuffers)
        check(alignment >= caps->storageBufferOffsetAlignment,
              "and a storage-buffer bind");

    // Not the extension flag alone: a std430 block is a thing the language has,
    // so the target the lowering would be asked for decides it - which is what
    // makes a driver capped to the floor answer no while its extension string
    // still says yes.
    check(device.supportsStorageBuffers()
              == (caps->storageBuffers && caps->glslTarget().allowsStorageBuffers()),
          "storage buffers are what this context's GLSL target can spell");

    if (device.supportsCompute())
        check(device.supportsStorageBuffers(),
              "a compute tier comes with the buffers its kernels write");

    check(device.supportsZeroToOneDepth() == caps->clipControl,
          "glClipControl is the whole of the [0, 1] depth range on GL");

    check(!device.supportsHalfSimdMatrix() && !device.supportsBFloat16SimdMatrix(),
          "no GL version has a cooperative-matrix fragment");
};
