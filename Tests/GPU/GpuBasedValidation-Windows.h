#pragma once

#include <eacp/GPU/GPU.h>
#include <eacp/GPU/Windows/D3D12Context.h>

#include <NanoTest/NanoTest.h>

#include <d3d12sdklayers.h>
#include <dxgi1_4.h>

#include <cstdlib>
#include <iostream>
#include <string>

// What a binary that runs under the D3D12 debug layer's GPU-based validation
// needs: switching it on, which has to happen in main before any device exists,
// and reading back what it reported. EACP_REQUIRE_GPU_VALIDATION=1 turns a skip
// into a failure.

inline bool enableGpuBasedValidation()
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

inline bool validationIsRequired()
{
    auto length = std::size_t {0};
    char value[8] = {};

    return getenv_s(&length, value, sizeof(value), "EACP_REQUIRE_GPU_VALIDATION")
               == 0
           && length > 1 && std::string(value) != "0";
}

inline ID3D12Device* nativeDevice()
{
    return eacp::GPU::getD3D12Context(eacp::GPU::Device::shared()).getDevice();
}

inline void waitForTheGpu()
{
    eacp::GPU::getD3D12Context(eacp::GPU::Device::shared()).waitIdle();
}

inline bool isSoftwareAdapter(ID3D12Device* device)
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
inline ValidationErrors takeErrors(ID3D12InfoQueue& queue)
{
    auto errors = ValidationErrors {};
    auto stored = queue.GetNumStoredMessagesAllowedByRetrievalFilter();

    for (auto index = UINT64 {0}; index < stored; ++index)
    {
        auto bytes = SIZE_T {0};

        if (FAILED(queue.GetMessage(index, nullptr, &bytes)))
            continue;

        auto storage = eacp::Vector<std::byte> {};
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

inline bool skipBecause(const std::string& why)
{
    auto message = "GPU-based validation unavailable, skipping: " + why;
    std::cout << message << '\n';

    if (validationIsRequired())
        nano::check(false, message + " (EACP_REQUIRE_GPU_VALIDATION is set)");

    return false;
}
