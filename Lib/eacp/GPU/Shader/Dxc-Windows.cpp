#include "DxcCompiler-Windows.h"

#include <eacp/Core/Plugins/DynamicLibrary.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Singleton.h>

#include <d3dcompiler.h>
#include <dxcapi.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// dxcompiler.dll is loaded by path through Plugins::DynamicLibrary and reached
// only through DxcCreateInstance, so nothing links dxcompiler.lib and a machine
// without the compiler builds and runs the same binary. Only <dxcapi.h> comes
// from the SDK.

namespace eacp::GPU::Dxc
{
namespace
{
std::string dxcFailure(std::string_view what, HRESULT hr)
{
    return Strings::format("{} failed (0x{:08X})", what, static_cast<unsigned>(hr));
}

std::string dxcText(IDxcBlobUtf8* blob)
{
    if (blob == nullptr || blob->GetStringLength() == 0)
        return {};

    return {blob->GetStringPointer(), blob->GetStringLength()};
}

// D3D12 refuses DXIL a validator has not signed, and an unsigned container's
// digest - the 16 bytes after "DXBC" - is all zeros.
bool dxcIsSigned(const void* bytes, std::size_t size)
{
    constexpr auto digestBegin = std::size_t {4};
    constexpr auto digestEnd = digestBegin + 16;

    const auto* data = static_cast<const std::uint8_t*>(bytes);

    if (size < digestEnd || std::memcmp(data, "DXBC", 4) != 0)
        return false;

    return std::any_of(data + digestBegin,
                       data + digestEnd,
                       [](std::uint8_t byte) { return byte != 0; });
}

bool dxcSupportsShaderModel6(ID3D12Device* device)
{
    if (device == nullptr)
        return false;

    auto model = D3D12_FEATURE_DATA_SHADER_MODEL {D3D_SHADER_MODEL_6_0};

    if (FAILED(device->CheckFeatureSupport(
            D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))))
        return false;

    return model.HighestShaderModel >= D3D_SHADER_MODEL_6_0;
}

winrt::com_ptr<ID3DBlob> dxcCopyToBlob(IDxcBlob& object)
{
    winrt::com_ptr<ID3DBlob> blob;

    if (FAILED(D3DCreateBlob(object.GetBufferSize(), blob.put())))
        return nullptr;

    std::memcpy(
        blob->GetBufferPointer(), object.GetBufferPointer(), object.GetBufferSize());
    return blob;
}
} // namespace

namespace Detail
{
class Compiler
{
public:
    // Older releases sign their output by loading dxil.dll by name, and a
    // name-only LoadLibrary finds a module already in the process before it
    // searches any directory, so mapping it first by its full path is what
    // lets a compiler outside the exe's folder find it. v1.9 signs without it.
    Compiler(const FilePath& directory, Vector<std::string> arguments)
        : extraArguments(std::move(arguments))
    {
        const auto validatorPath = directory / "dxil.dll";

        if (Files::exists(validatorPath) && !validator.open(validatorPath))
        {
            loadError = "dxil.dll: " + validator.lastError();
            return;
        }

        if (!library.open(directory / "dxcompiler.dll"))
        {
            loadError = "dxcompiler.dll: " + library.lastError();
            return;
        }

        auto create =
            library.findFunction<DxcCreateInstanceProc>("DxcCreateInstance");

        if (create == nullptr)
        {
            loadError = "dxcompiler.dll exports no DxcCreateInstance";
            return;
        }

        auto hr =
            create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), compiler.put_void());

        if (FAILED(hr))
        {
            compiler = nullptr;
            loadError = dxcFailure("DxcCreateInstance", hr);
        }
    }

    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;

    bool isValid() const { return compiler != nullptr; }

    const std::string& error() const { return loadError; }

    std::string version() const
    {
        winrt::com_ptr<IDxcVersionInfo> info;

        if (!isValid()
            || FAILED(compiler->QueryInterface(__uuidof(IDxcVersionInfo),
                                               info.put_void())))
            return {};

        auto major = UINT32 {0};
        auto minor = UINT32 {0};
        info->GetVersion(&major, &minor);

        auto result = Strings::concat(major, ".", minor);

        winrt::com_ptr<IDxcVersionInfo2> commitInfo;

        if (FAILED(info->QueryInterface(__uuidof(IDxcVersionInfo2),
                                        commitInfo.put_void())))
            return result;

        auto commitCount = UINT32 {0};
        char* hash = nullptr;

        if (SUCCEEDED(commitInfo->GetCommitInfo(&commitCount, &hash)))
        {
            result += Strings::concat(".", commitCount, " (", hash, ")");
            CoTaskMemFree(hash);
        }

        return result;
    }

    winrt::com_ptr<ID3DBlob> compile(const std::string& source,
                                     const std::string& entry,
                                     std::string_view profile,
                                     ShaderStage stage,
                                     const Vector<ShaderDefine>& defines,
                                     CompilationInfo& messages) const
    {
        auto arguments = std::vector<std::wstring> {
            L"-E", Strings::widen(entry), L"-T", Strings::widen(profile), L"-Ges"};

        for (const auto& argument: extraArguments)
            arguments.push_back(Strings::widen(argument));

        for (const auto& define: defines)
        {
            arguments.push_back(L"-D");
            arguments.push_back(Strings::widen(define.name + "=" + define.value));
        }

        auto argumentPointers = std::vector<LPCWSTR>(arguments.size());
        std::transform(arguments.begin(),
                       arguments.end(),
                       argumentPointers.begin(),
                       [](const std::wstring& argument)
                       { return argument.c_str(); });

        auto sourceBuffer = DxcBuffer {source.data(), source.size(), DXC_CP_UTF8};
        winrt::com_ptr<IDxcResult> output;

        auto lock = std::lock_guard {compiling};
        auto hr = compiler->Compile(&sourceBuffer,
                                    argumentPointers.data(),
                                    static_cast<UINT32>(argumentPointers.size()),
                                    nullptr,
                                    __uuidof(IDxcResult),
                                    output.put_void());

        if (FAILED(hr))
        {
            const auto failure = dxcFailure("IDxcCompiler3::Compile", hr);
            LOG("DXC: ", failure);
            messages.add(CompilationMessageType::Error, failure, stage);
            return nullptr;
        }

        winrt::com_ptr<IDxcBlobUtf8> errors;
        output->GetOutput(
            DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), errors.put_void(), nullptr);

        auto status = E_FAIL;
        output->GetStatus(&status);

        winrt::com_ptr<IDxcBlob> object;

        const auto failed =
            FAILED(status)
            || FAILED(output->GetOutput(
                DXC_OUT_OBJECT, __uuidof(IDxcBlob), object.put_void(), nullptr))
            || object == nullptr || object->GetBufferSize() == 0;

        const auto log = dxcText(errors.get());
        messages.append(parseDxcLog({log, stage, failed}));

        if (failed)
        {
            LOG("DXC: ", entry, " (", profile, "): ", log);

            if (!messages.hasErrors())
                messages.add(
                    CompilationMessageType::Error, "DXC compiled nothing", stage);

            return nullptr;
        }

        if (!dxcIsSigned(object->GetBufferPointer(), object->GetBufferSize()))
        {
            const auto unsignedOutput =
                std::string {"compiled to unsigned DXIL, which D3D12 refuses; the "
                             "release needs the dxil.dll it shipped beside "
                             "dxcompiler.dll"};
            LOG("DXC: ", entry, " ", unsignedOutput);
            messages.add(CompilationMessageType::Error, unsignedOutput, stage);
            return nullptr;
        }

        return dxcCopyToBlob(*object);
    }

private:
    Plugins::DynamicLibrary validator;
    Plugins::DynamicLibrary library;

    // After the libraries, so it is released before either unmaps.
    winrt::com_ptr<IDxcCompiler3> compiler;
    Vector<std::string> extraArguments;
    std::string loadError;

    // One IDxcCompiler3 is not free-threaded, and libraries are built on
    // several threads for several devices.
    mutable std::mutex compiling;
};
} // namespace Detail

namespace
{
// The process-wide state. Immortal, so the compiler is never unloaded under a
// static destructor that still compiles.
struct DxcRegistry
{
    std::mutex mutex;
    std::condition_variable changed;

    Options options;
    State state = State::disabled;

    // Bumped by every setOptions, so a load the options were replaced under
    // keeps nothing it found.
    std::uint64_t generation = 0;
    std::thread::id loadingThread;

    std::shared_ptr<const Detail::Compiler> compiler;
    FilePath directory;
    std::string version;
    std::string error;
    bool reportedReentry = false;

    std::atomic<int> compiledStages {0};
};

DxcRegistry& dxcRegistry()
{
    return Singleton::getImmortal<DxcRegistry>();
}

Status dxcStatusLocked(const DxcRegistry& registry)
{
    return {.state = registry.state,
            .version = registry.version,
            .directory = registry.directory,
            .error = registry.error,
            .compiledStages = registry.compiledStages.load()};
}

void dxcFailLocked(DxcRegistry& registry, std::string reason)
{
    LOG("DXC: not used, shaders compile through FXC: ", reason);

    registry.state = State::failed;
    registry.error = std::move(reason);
    registry.compiler = nullptr;
}

struct DxcLocated
{
    FilePath directory;
    std::shared_ptr<const Detail::Compiler> compiler;
    std::string error;
};

DxcLocated dxcLocateAndLoad(const Options& options)
{
    auto located = DxcLocated {};

    try
    {
        located.directory = options.locateLibrary();
    }
    catch (const std::exception& e)
    {
        located.error = Strings::concat("locateLibrary threw: ", e.what());
        return located;
    }
    catch (...)
    {
        located.error = "locateLibrary threw";
        return located;
    }

    if (located.directory.empty())
    {
        located.error = "locateLibrary found no compiler";
        return located;
    }

    auto compiler =
        std::make_shared<Detail::Compiler>(located.directory, options.arguments);

    if (compiler->isValid())
        located.compiler = std::move(compiler);
    else
        located.error = compiler->error();

    return located;
}

// Waits out a load running on another thread. A load running on this one is
// locateLibrary pumping the event loop into another compile, which can only
// be answered from where things stand.
bool dxcWaitForLoad(DxcRegistry& registry, std::unique_lock<std::mutex>& lock)
{
    while (registry.state == State::loading)
    {
        if (registry.loadingThread == std::this_thread::get_id())
            return false;

        registry.changed.wait(lock);
    }

    return true;
}

void dxcKeepLoadLocked(DxcRegistry& registry, DxcLocated located)
{
    registry.loadingThread = {};
    registry.directory = located.directory;

    if (located.compiler == nullptr)
        dxcFailLocked(registry, located.error);
    else
    {
        registry.state = State::loaded;
        registry.compiler = std::move(located.compiler);
        registry.version = registry.compiler->version();
        LOG("DXC: ", registry.version, " loaded from ", registry.directory.str());
    }

    registry.changed.notify_all();
}

// A load the options were replaced under is dropped, and the new options are
// loaded or waited for in its place.
void dxcLoadLocked(DxcRegistry& registry, std::unique_lock<std::mutex>& lock)
{
    while (dxcWaitForLoad(registry, lock) && registry.state == State::notLoaded)
    {
        const auto generation = registry.generation;
        const auto options = registry.options;

        registry.state = State::loading;
        registry.loadingThread = std::this_thread::get_id();

        lock.unlock();
        auto located = dxcLocateAndLoad(options);
        lock.lock();

        if (generation == registry.generation)
        {
            dxcKeepLoadLocked(registry, std::move(located));
            return;
        }
    }
}
} // namespace

void setOptions(Options options)
{
    auto& registry = dxcRegistry();
    auto lock = std::lock_guard {registry.mutex};

    registry.state = options.enabled ? State::notLoaded : State::disabled;
    registry.options = std::move(options);
    ++registry.generation;
    registry.loadingThread = {};
    registry.compiler = nullptr;
    registry.directory = {};
    registry.version.clear();
    registry.error.clear();
    registry.reportedReentry = false;
    registry.changed.notify_all();
}

Options getOptions()
{
    auto& registry = dxcRegistry();
    auto lock = std::lock_guard {registry.mutex};
    return registry.options;
}

bool isEnabled()
{
    auto& registry = dxcRegistry();
    auto lock = std::lock_guard {registry.mutex};
    return registry.options.enabled;
}

Status load()
{
    auto& registry = dxcRegistry();
    auto lock = std::unique_lock {registry.mutex};
    dxcLoadLocked(registry, lock);
    return dxcStatusLocked(registry);
}

Status status()
{
    auto& registry = dxcRegistry();
    auto lock = std::lock_guard {registry.mutex};
    return dxcStatusLocked(registry);
}

std::shared_ptr<const Detail::Compiler> Detail::compilerFor(ID3D12Device* device)
{
    auto& registry = dxcRegistry();
    auto lock = std::unique_lock {registry.mutex};

    if (!registry.options.enabled)
        return nullptr;

    dxcLoadLocked(registry, lock);

    if (registry.state == State::loading)
    {
        if (!registry.reportedReentry)
            LOG("DXC: a shader built while locateLibrary runs compiles through FXC");

        registry.reportedReentry = true;
        return nullptr;
    }

    if (registry.state != State::loaded)
        return nullptr;

    if (!dxcSupportsShaderModel6(device))
        dxcFailLocked(registry,
                      "the D3D12 device does not support Shader Model 6.0");

    return registry.compiler;
}

winrt::com_ptr<ID3DBlob> Detail::compile(const Compiler& compiler,
                                         const std::string& source,
                                         const std::string& entry,
                                         std::string_view profile,
                                         ShaderStage stage,
                                         const Vector<ShaderDefine>& defines,
                                         CompilationInfo& messages)
{
    auto blob = compiler.compile(source, entry, profile, stage, defines, messages);

    if (blob != nullptr)
        ++dxcRegistry().compiledStages;

    return blob;
}
} // namespace eacp::GPU::Dxc
