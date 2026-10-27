#pragma once

#include "HotReload.h"
#include "HotReloadLoop.h"
#include "ReloadableEntry.h"
#include "../Utils/Logging.h"

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Included directly, never through the Core.h umbrella: the constructor reads
// the mode and the module from the app's own compile definitions.

namespace eacp::Plugins
{
struct ReloadableOptions
{
    std::function<void(std::string_view line)> log = [](std::string_view line)
    { LOG(line); };

    std::function<void()> onReloaded = [] {};
    bool callOnReloadedAtCreation = true;

    std::function<void()> onStateChanged = [] {};
    std::function<void(const BuildFailure& failure)> onBuildFailed =
        [](const BuildFailure&) {};
    std::function<void(const std::string& error)> onLoadFailed =
        [](const std::string&) {};
    std::function<void(std::string_view chunk)> onBuildOutput =
        [](std::string_view) {};

    Files::SourceWatcher::Options sources;
    std::optional<HotReloadLoop::Options> loop;
};

namespace Detail
{
// What the app's call site decides: the mode, the compiled-in table static mode
// uses, and the module dev mode loads. Tests build one by hand.
struct ReloadableSetup
{
    ReloadableTable compiledIn;
    bool hotReload = false;
    HotReloadTarget target;
};

// The type a signatureOf() string names, as this compiler spells it.
std::string readableSignature(std::string_view signature);
} // namespace Detail

// The functions EACP_RELOADABLE lists, each found by its type: create<T>()
// calls the T*() factory and function<F>() returns the F. In static mode they
// are the app's own; under EACP_HOT_RELOAD they come from the module, loaded
// at construction as generation 0 and swapped on every save. See
// Docs/HotReload.md.
//
// Lifetime: onReloaded runs once at the end of construction and again after
// every reload, so declare the Reloadable after everything onReloaded uses.
// On a reload, every object create<T>() made must be replaced or destroyed
// inside onReloaded, while the previous image is still mapped. Destroying the
// Reloadable leaves its image mapped for the rest of the loop turn, so members
// destroyed after it may still hold its objects.
//
// No value: create<T>() returns an empty pointer when the factory returns null
// or throws, or when no module is loaded; function<F>() returns nullptr when no
// module is loaded, and call<F>() then returns a value-initialised result. A
// type EACP_RELOADABLE does not list throws std::logic_error.
class Reloadable
{
public:
    template <bool hotReload = isHotReload()>
    explicit Reloadable(const ReloadableOptions& optionsToUse = {})
        : Reloadable(setupFor<hotReload>(), optionsToUse)
    {
    }

    Reloadable(const Detail::ReloadableSetup& setup,
               const ReloadableOptions& optionsToUse);
    ~Reloadable();

    Reloadable(const Reloadable&) = delete;
    Reloadable& operator=(const Reloadable&) = delete;
    Reloadable(Reloadable&&) = delete;
    Reloadable& operator=(Reloadable&&) = delete;

    template <typename T>
    OwningPointer<T> create()
    {
        using Factory = T* (*)();
        auto factory =
            reinterpret_cast<Factory>(lookup(Detail::signatureOf<T*()>()));

        if (factory == nullptr)
            return {};

        try
        {
            if (auto* made = factory())
                return OwningPointer<T> {made};

            logFactoryFailure("factory returned nothing");
        }
        catch (const std::exception& e)
        {
            logFactoryFailure(std::string {"factory threw: "} + e.what());
        }
        catch (...)
        {
            logFactoryFailure("factory threw");
        }

        return {};
    }

    template <typename F>
    F* function()
    {
        static_assert(std::is_function_v<F>, "function<F>() takes a function type");
        return reinterpret_cast<F*>(lookup(Detail::signatureOf<F>()));
    }

    template <typename F, typename... Args>
    std::invoke_result_t<F&, Args...> call(Args&&... args)
    {
        using Result = std::invoke_result_t<F&, Args...>;

        if (auto* current = function<F>())
            return current(std::forward<Args>(args)...);

        if constexpr (!std::is_void_v<Result>)
            return Result {};
    }

    [[nodiscard]] std::string describeState() const;
    [[nodiscard]] int getGeneration() const;

private:
    template <bool hotReload>
    static Detail::ReloadableSetup setupFor()
    {
        if constexpr (hotReload)
            return {{}, true, hotReloadTarget()};
        else
            return {eacpReloadableTable(), false, {}};
    }

    ReloadableEntry::Erased lookup(std::string_view signature) const;
    void logFactoryFailure(const std::string& reason) const;

    HotReloadLoop::Options loopOptions(const HotReloadTarget& target) const;
    std::string refusalOf(const DynamicLibrary& image) const;
    void startLoop(const HotReloadTarget& target);
    void loadFirstImage();
    void useLoadedImage();
    void reloaded();

    ReloadableOptions options;
    ReloadableTable compiledIn;
    ReloadableTable loaded;
    Vector<std::string> firstSignatures;
    bool imageLoaded = false;
    int firstImageGeneration = 0;
    std::string firstLoadFailure;
    std::optional<HotReloadLoop> loop;
};
} // namespace eacp::Plugins
