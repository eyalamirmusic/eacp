#include <eacp/Core/Utils/WinInclude.h>

#include "Backends.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>
#include <string>

// The same FXC call ShaderLibrary-Windows.cpp makes at run time - no file
// name, no defines, no include handler, D3DCOMPILE_ENABLE_STRICTNESS - so the
// bytecode is what the runtime would have compiled from the same text.

namespace eacp::GPU::ShaderCompiler::Detail
{
namespace
{
// FXC's message blob is text with a terminating null inside its size; reading
// by size rather than to the null never runs past a blob that lacks one.
std::string dxbcTextOf(ID3DBlob* blob)
{
    if (blob == nullptr)
        return {};

    auto text = std::string(static_cast<const char*>(blob->GetBufferPointer()),
                            blob->GetBufferSize());

    while (!text.empty() && text.back() == '\0')
        text.pop_back();

    return text;
}
} // namespace

bool isDxbcAvailable()
{
    return true;
}

Blob compileDxbc(const std::string& source,
                 const std::string& entry,
                 const char* profile)
{
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    auto hr = D3DCompile(source.data(),
                         source.size(),
                         nullptr,
                         nullptr,
                         nullptr,
                         entry.c_str(),
                         profile,
                         D3DCOMPILE_ENABLE_STRICTNESS,
                         0,
                         code.GetAddressOf(),
                         errors.GetAddressOf());

    auto blob = Blob {};
    blob.log = dxbcTextOf(errors.Get());

    if (FAILED(hr) || code == nullptr)
        return blob;

    blob.bytes.resize(code->GetBufferSize());
    std::memcpy(blob.bytes.data(), code->GetBufferPointer(), code->GetBufferSize());
    blob.succeeded = true;
    return blob;
}
} // namespace eacp::GPU::ShaderCompiler::Detail
