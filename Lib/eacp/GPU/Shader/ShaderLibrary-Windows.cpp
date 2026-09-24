#include <eacp/Core/Utils/WinInclude.h>

#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"
#include "ShaderBinaryCache.h"
#include "ShaderSource.h"

#include <d3dcompiler.h>
#include <dxcapi.h>

#include <cstring>
#include <iterator>
#include <string>
#include <string_view>

#include <winrt/base.h>

// Windows/D3D12 backend. Compiles the HLSL source with FXC into vertex/pixel
// (or compute) bytecode; D3D12 consumes the blobs directly at pipeline
// creation, so no shader objects exist at this level. SM 5.0 DXBC remains
// valid input for D3D12 pipelines, which keeps the hand-written HLSL in tests
// and examples working unchanged.
//
// FXC is the slow half of building a pipeline, so what it produces is kept on
// disk (ShaderBinaryCache) and a later launch reads the bytecode back instead of
// compiling the same source again.

namespace eacp::GPU
{
namespace
{
constexpr auto compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;

std::string fxcIdentity()
{
    return "fxc-" + std::to_string(D3D_COMPILER_VERSION) + "-flags"
           + std::to_string(compileFlags);
}

std::string
    cacheKey(const std::string& source, const std::string& entry, const char* target)
{
    return entry + '\n' + target + '\n' + source;
}

// DXC, when the machine has it. dxcompiler.dll ships in the Windows SDK and in
// its redistributable, but neither is on PATH by default, so this asks for it
// by name and takes no for an answer: a machine without it compiles with FXC
// exactly as before rather than failing to start.
// Asked for, not merely present. Which compiler built a shader decides the
// last bit of every float it computes, and gating that on whether a DLL
// happens to be installed would make the same binary produce different audio
// on two machines - the one thing a backend must not do quietly.
bool dxcRequested()
{
    auto length = std::size_t {0};
    char value[8] = {};

    return getenv_s(&length, value, sizeof(value), "EACP_D3D12_DXC") == 0
        && length > 1 && value[0] != '0';
}

struct DxcLibrary
{
    DxcLibrary()
    {
        if (!dxcRequested())
            return;

        module = LoadLibraryW(L"dxcompiler.dll");

        if (module == nullptr)
            return;

        create = reinterpret_cast<DxcCreateInstanceProc>(
            GetProcAddress(module, "DxcCreateInstance"));
    }

    bool isAvailable() const { return create != nullptr; }

    HMODULE module = nullptr;
    DxcCreateInstanceProc create = nullptr;
};

const DxcLibrary& dxc()
{
    static const auto library = DxcLibrary {};
    return library;
}

// Said once a process, not once a kernel: which compiler built the shaders is
// worth knowing, and worth knowing quietly.
void reportShaderCompilerOnce()
{
    static const auto reported = []
    {
        if (dxc().isAvailable())
            LOG("eacp: EACP_D3D12_DXC is set, so shaders compile with DXC at "
                "shader model 6. Results move by rounding against an FXC "
                "build.");
        else if (dxcRequested())
            LOG("eacp: EACP_D3D12_DXC is set but dxcompiler.dll was not found, "
                "so shaders compile with FXC at shader model 5.");
        return true;
    }();

    (void) reported;
}

std::string dxcIdentity()
{
    return "dxc-sm6";
}

// The shader model 6 target matching a shader model 5 one: cs_5_0 to cs_6_0.
std::wstring dxcTargetFor(const char* target)
{
    auto sm5 = std::string {target};
    auto sm6 = sm5.substr(0, sm5.size() - 3) + "6_0";

    return std::wstring {sm6.begin(), sm6.end()};
}

std::string dxcCompile(const std::string& source,
                       const std::string& entry,
                       const char* target)
{
    auto utils = winrt::com_ptr<IDxcUtils> {};
    auto compiler = winrt::com_ptr<IDxcCompiler3> {};

    if (FAILED(dxc().create(CLSID_DxcUtils, IID_PPV_ARGS(utils.put())))
        || FAILED(dxc().create(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put()))))
        return {};

    auto wideEntry = std::wstring {entry.begin(), entry.end()};
    auto wideTarget = dxcTargetFor(target);

    const wchar_t* arguments[] = {
        L"-E", wideEntry.c_str(), L"-T", wideTarget.c_str(), L"-O3"};

    auto buffer = DxcBuffer {};
    buffer.Ptr = source.data();
    buffer.Size = source.size();
    buffer.Encoding = DXC_CP_UTF8;

    auto result = winrt::com_ptr<IDxcResult> {};

    if (FAILED(compiler->Compile(&buffer,
                                 arguments,
                                 (UINT32) std::size(arguments),
                                 nullptr,
                                 IID_PPV_ARGS(result.put()))))
        return {};

    auto status = HRESULT {};

    if (FAILED(result->GetStatus(&status)) || FAILED(status))
    {
        auto errors = winrt::com_ptr<IDxcBlobUtf8> {};

        if (SUCCEEDED(
                result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.put()), nullptr))
            && errors != nullptr && errors->GetStringLength() > 0)
            LOG(errors->GetStringPointer());

        return {};
    }

    auto object = winrt::com_ptr<IDxcBlob> {};

    if (FAILED(
            result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object.put()), nullptr))
        || object == nullptr)
        return {};

    return std::string {static_cast<const char*>(object->GetBufferPointer()),
                        object->GetBufferSize()};
}

winrt::com_ptr<ID3DBlob> blobHolding(const std::string& bytes)
{
    winrt::com_ptr<ID3DBlob> blob;

    if (FAILED(D3DCreateBlob(bytes.size(), blob.put())))
        return nullptr;

    std::memcpy(blob->GetBufferPointer(), bytes.data(), bytes.size());
    return blob;
}

winrt::com_ptr<ID3DBlob> compileStage(const std::string& source,
                                      const std::string& entry,
                                      const char* target)
{
    reportShaderCompilerOnce();

    auto key = cacheKey(source, entry, target);
    auto identity = dxc().isAvailable() ? dxcIdentity() : fxcIdentity();

    if (auto cached = ShaderBinaryCache::load(identity, key))
        if (auto blob = blobHolding(*cached))
            return blob;

    // DXC where it is there, and FXC where it is not or where it refused -
    // hand-written shader model 5 HLSL in the tests and examples is not all
    // valid shader model 6, and it compiled yesterday.
    if (dxc().isAvailable())
    {
        if (auto bytes = dxcCompile(source, entry, target); !bytes.empty())
        {
            ShaderBinaryCache::store(identity, key, bytes);

            if (auto blob = blobHolding(bytes))
                return blob;
        }
    }

    winrt::com_ptr<ID3DBlob> code;
    winrt::com_ptr<ID3DBlob> errors;

    auto hr = D3DCompile(source.data(),
                         source.size(),
                         nullptr,
                         nullptr,
                         nullptr,
                         entry.c_str(),
                         target,
                         compileFlags,
                         0,
                         code.put(),
                         errors.put());

    if (FAILED(hr))
    {
        if (errors)
            LOG(static_cast<const char*>(errors->GetBufferPointer()));

        return nullptr;
    }

    ShaderBinaryCache::store(
        identity,
        key,
        std::string_view {static_cast<const char*>(code->GetBufferPointer()),
                          code->GetBufferSize()});

    return code;
}
} // namespace

struct ShaderLibrary::Native
{
    Native(Device& device, const ShaderSource& source)
    {
        // An empty source is a build something declined to make, and whatever
        // declined it has already said why - see ComputeProgram::prepare. There
        // is nothing here to compile and nothing for FXC to complain about.
        if (!device.isValid() || source.source.empty())
            return;

        if (source.isCompute())
        {
            program.computeBytecode =
                compileStage(source.source, source.computeEntry, "cs_5_0");
        }
        else
        {
            program.vertexBytecode =
                compileStage(source.source, source.vertexEntry, "vs_5_0");
            program.pixelBytecode =
                compileStage(source.source, source.fragmentEntry, "ps_5_0");
        }
    }

    D3D12ShaderProgram program;
};

ShaderLibrary::ShaderLibrary(Device& device, const ShaderSource& source)
    : vertexEntryName(source.vertexEntry)
    , fragmentEntryName(source.fragmentEntry)
    , computeEntryName(source.computeEntry)
    , groupShape(source.threadGroup)
    , impl(device, source)
{
}

bool ShaderLibrary::isValid() const
{
    if (impl->program.computeBytecode != nullptr)
        return true;

    return impl->program.vertexBytecode != nullptr
           && impl->program.pixelBytecode != nullptr;
}

void* ShaderLibrary::nativeLibrary() const
{
    return const_cast<D3D12ShaderProgram*>(&impl->program);
}
} // namespace eacp::GPU
