#include <eacp/GPU/Windows/D3D12Context.h>
#include <eacp/GPUWidgets/GPUWidgets.h>
#include <eacp/GPUWidgets/Path/CoverageKernel.h>
#include <eacp/GPUWidgets/Path/PrefixSum.h>
#include <eacp/GPUWidgets/Path/PrefixSumKernels.h>

#include <NanoTest/NanoTest.h>

#include <d3d12sdklayers.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

// The prefix sum under the D3D12 debug layer's GPU-based validation, which is
// the only thing that sees what this file is about: a read whose value is
// thrown away, from an address past the end of its buffer.
//
// ScanBlockKernel guards every read of its counts with at < elementCount, and
// the numbers it leaves are right at every size. But FXC compiles a guarded
// read under cs_5_0 as an unconditional load whose value the guard then masks,
// which Shader Model 5 allows because an out-of-range structured load through a
// descriptor reads zero. The backend binds compute buffers as root UAVs, and a
// root descriptor carries no size: the load goes wherever the address says. So
// every lane of a partial group reads past the counts, and only validation, a
// fault or a device removal ever says so.
//
// Validation has to be on before the device exists, so this is a binary of its
// own whose main switches it on before anything asks for Device::shared().
// Where it cannot run - no Graphics Tools, WARP (which faults inside its own
// shader JIT running the validated scan kernel, fixed or not), or an adapter
// whose validation does not report even a deliberate overread - the case skips
// and says why. EACP_REQUIRE_GPU_VALIDATION=1 turns that skip into a failure.

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;

namespace
{
constexpr auto perGroup = ScanBlockKernel::perGroup;

auto validationLayerEnabled = false;

bool enableGpuBasedValidation()
{
    auto debug = winrt::com_ptr<ID3D12Debug>();

    if (FAILED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), debug.put_void())))
        return false;

    auto gpuBased = debug.try_as<ID3D12Debug1>();

    if (!gpuBased)
        return false;

    debug->EnableDebugLayer();
    gpuBased->SetEnableGPUBasedValidation(TRUE);
    return true;
}

bool validationIsRequired()
{
    auto length = std::size_t {0};
    char value[8] = {};

    return getenv_s(&length, value, sizeof(value), "EACP_REQUIRE_GPU_VALIDATION")
               == 0
           && length > 1 && std::string(value) != "0";
}

ID3D12Device* nativeDevice()
{
    return GPU::getD3D12Context(GPU::Device::shared()).getDevice();
}

void waitForTheGpu()
{
    GPU::getD3D12Context(GPU::Device::shared()).waitIdle();
}

bool isSoftwareAdapter(ID3D12Device* device)
{
    auto factory = winrt::com_ptr<IDXGIFactory4>();

    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), factory.put_void())))
        return false;

    auto adapter = winrt::com_ptr<IDXGIAdapter1>();

    if (FAILED(factory->EnumAdapterByLuid(
            device->GetAdapterLuid(), __uuidof(IDXGIAdapter1), adapter.put_void())))
        return false;

    auto description = DXGI_ADAPTER_DESC1 {};
    adapter->GetDesc1(&description);
    return (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
}

struct ValidationErrors
{
    int count = 0;
    int outOfBounds = 0;
    std::string first;
};

// Everything the queue holds at error severity or worse, which it then forgets,
// so each call sees only what happened since the last.
ValidationErrors takeErrors(ID3D12InfoQueue& queue)
{
    auto errors = ValidationErrors {};
    auto stored = queue.GetNumStoredMessagesAllowedByRetrievalFilter();

    for (auto index = UINT64 {0}; index < stored; ++index)
    {
        auto bytes = SIZE_T {0};

        if (FAILED(queue.GetMessage(index, nullptr, &bytes)))
            continue;

        auto storage = Vector<std::byte> {};
        storage.resize((int) bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());

        if (FAILED(queue.GetMessage(index, message, &bytes))
            || message->Severity > D3D12_MESSAGE_SEVERITY_ERROR)
            continue;

        if (errors.count++ == 0)
            errors.first = message->pDescription;

        if (message->ID
            == D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_ROOT_DESCRIPTOR_ACCESS_OUT_OF_BOUNDS)
            ++errors.outOfBounds;
    }

    queue.ClearStoredMessages();
    return errors;
}

// One thread reading 64 elements past a one-element buffer, bound the way the
// scan's counts are: the overread validation has to report before a clean scan
// can be believed.
struct OverreadKernel final : GPU::ComputeProgram
{
    static constexpr auto distance = 64u;

    OverreadKernel() { compile(); }

    void define() override
    {
        auto at = threadId();
        write(output, at, source.load(at + distance));
    }

    GPU::Uniform<GPU::AtomicBuffer> source;
    GPU::Uniform<GPU::AtomicBuffer> output;

    EACP_SHADER(source, output)
};

bool validationReportsAnOverread(ID3D12InfoQueue& queue)
{
    auto one = std::uint32_t {1};
    auto bytes = (int) sizeof(one);
    auto& device = GPU::Device::shared();
    auto source = GPU::Buffer {device, &one, bytes, GPU::BufferUsage::Storage};
    auto output = GPU::Buffer {device, nullptr, bytes, GPU::BufferUsage::Storage};

    auto& kernel = sharedKernel<OverreadKernel>();
    kernel.source = source;
    kernel.output = output;

    takeErrors(queue);
    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, 1);
    }

    commands.commit();
    output.read(&one, bytes);
    waitForTheGpu();

    return takeErrors(queue).outOfBounds > 0;
}

bool skipBecause(const std::string& why)
{
    auto message = "GPU-based validation unavailable, skipping: " + why;
    std::cout << message << '\n';

    if (validationIsRequired())
        check(false, message + " (EACP_REQUIRE_GPU_VALIDATION is set)");

    return false;
}

// The info queue of a device whose validation is known to report a root
// descriptor overread, or null after saying why there is none.
winrt::com_ptr<ID3D12InfoQueue> validatedQueue()
{
    if (!validationLayerEnabled)
        return skipBecause("no D3D12 debug layer; install the Windows Graphics Tools"),
               nullptr;

    if (!GPU::Device::shared().isValid())
        return skipBecause("no device"), nullptr;

    auto* device = nativeDevice();
    auto queue = winrt::com_ptr<ID3D12InfoQueue>();

    if (FAILED(device->QueryInterface(__uuidof(ID3D12InfoQueue), queue.put_void())))
        return skipBecause("the device has no info queue"), nullptr;

    queue->SetMessageCountLimit(4096);
    auto name = GPU::Device::shared().name();

    if (!validationReportsAnOverread(*queue))
        return skipBecause(name + " does not report a deliberate root UAV overread"),
               nullptr;

    if (isSoftwareAdapter(device))
        return skipBecause(name + " faults running the validated scan kernel"), nullptr;

    std::cout << "GPU-based validation is reporting on " << name << '\n';
    return queue;
}

Vector<std::uint32_t> countsOfLength(int count)
{
    auto values = Vector<std::uint32_t> {};

    for (auto i = 0; i < count; ++i)
        values.add((std::uint32_t) ((i * 7 + (i % 13)) % 31));

    return values;
}

// The whole of PrefixSum over exact-sized buffers, its offsets held to the
// running sum and its counts to zero, as PrefixSumTests holds them.
void expectScans(int count)
{
    auto counts = countsOfLength(count);
    auto bytes = (int) sizeof(std::uint32_t) * count;
    auto& device = GPU::Device::shared();

    auto source = GPU::Buffer {device, counts.data(), bytes, GPU::BufferUsage::Storage};
    auto destination =
        GPU::Buffer {device, nullptr, bytes, GPU::BufferUsage::Storage};

    auto sum = PrefixSum {};
    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        sum.run(pass, source, destination, count);
    }

    commands.commit();

    auto offsets = Vector<std::uint32_t> {};
    auto left = Vector<std::uint32_t> {};
    offsets.resize(count);
    left.resize(count);
    destination.read(offsets.data(), bytes);
    source.read(left.data(), bytes);
    waitForTheGpu();

    auto running = std::uint32_t {0};
    auto wrong = 0;

    for (auto i = 0; i < count; ++i)
    {
        wrong += offsets[i] != running || left[i] != 0 ? 1 : 0;
        running += counts[i];
    }

    check(wrong == 0,
          std::to_string(wrong) + " of " + std::to_string(count) + " elements wrong");
}
} // namespace

// Every size PrefixSumTests scans: inside one group, across groups and three
// levels deep. All but the exact multiples of a group leave a partial group,
// whose lanes past the end are the ones that read past the counts.
auto tScansReadInsideTheirBuffers =
    test("PrefixSumValidation/scansReadOnlyInsideTheirBuffers") = []
{
    auto queue = validatedQueue();

    if (!queue)
        return;

    for (auto count: {1,
                      2,
                      63,
                      64,
                      65,
                      255,
                      perGroup - 1,
                      perGroup,
                      perGroup + 1,
                      perGroup * 2,
                      perGroup * 7 + 3,
                      perGroup * perGroup + 17})
    {
        expectScans(count);

        auto errors = takeErrors(*queue);

        check(errors.count == 0,
              "count " + std::to_string(count) + ": " + std::to_string(errors.count)
                  + " GPU validation errors (" + std::to_string(errors.outOfBounds)
                  + " root descriptor out of bounds), first: " + errors.first);
    }

    check(SUCCEEDED(nativeDevice()->GetDeviceRemovedReason()),
          "the device survives the scans");
};

int main(int argc, char* argv[])
{
    validationLayerEnabled = enableGpuBasedValidation();
    return nano::run(argc, argv);
}
