#include "../D2D-Windows.h"

#include <dwrite_3.h>

#include <mutex>
#include <string>
#include <vector>

// The shared font collection — the Windows stand-in for CoreText's process-wide
// registry. CTFontManagerRegisterGraphicsFont makes an embedded font visible to
// every CoreText call site at once; DirectWrite has no such registry, so the
// one collection lives here and Font, TextMetrics and the eacp-text rasterizer
// all resolve against it. Needs no device, render target or window, so it is
// as happy in a headless test as in a frame.

namespace eacp::Graphics
{
IDWriteFactory* getDWriteFactory();

namespace
{
using Microsoft::WRL::ComPtr;

// Only memory-font registration needs this newer interface, so a machine too
// old to offer it still resolves installed fonts.
IDWriteFactory5* memoryFontFactory()
{
    static auto instance = []
    {
        auto created = ComPtr<IDWriteFactory5>();

        if (auto* factory = getDWriteFactory())
            factory->QueryInterface(
                __uuidof(IDWriteFactory5),
                reinterpret_cast<void**>(created.GetAddressOf()));

        return created;
    }();

    return instance.Get();
}

// The system collection until a font is registered from memory, and a rebuilt
// system-plus-embedded one afterwards.
class FontRegistry
{
public:
    ComPtr<IDWriteFontCollection> collection()
    {
        const auto lock = std::lock_guard {mutex};

        if (!current)
            if (auto* factory = getDWriteFactory())
                factory->GetSystemFontCollection(current.GetAddressOf(), FALSE);

        return current;
    }

    bool add(const void* data, std::size_t size)
    {
        auto* factory = memoryFontFactory();

        if (factory == nullptr || data == nullptr || size == 0)
            return false;

        const auto lock = std::lock_guard {mutex};

        if (!loader)
        {
            if (FAILED(factory->CreateInMemoryFontFileLoader(loader.GetAddressOf()))
                || FAILED(factory->RegisterFontFileLoader(loader.Get())))
                return false;
        }

        auto file = ComPtr<IDWriteFontFile>();

        // A null owner tells DirectWrite to copy the data, so the caller's
        // buffer does not have to outlive the registration.
        if (FAILED(loader->CreateInMemoryFontFileReference(factory,
                                                           data,
                                                           static_cast<UINT32>(size),
                                                           nullptr,
                                                           file.GetAddressOf())))
            return false;

        files.push_back(file);

        return rebuild(factory);
    }

private:
    // Called with the lock held. A font set is immutable once built, so adding a
    // face means building a fresh one over every file plus the system set.
    bool rebuild(IDWriteFactory5* factory)
    {
        auto builder = ComPtr<IDWriteFontSetBuilder1>();

        if (FAILED(factory->CreateFontSetBuilder(builder.GetAddressOf())))
            return false;

        for (const auto& file: files)
            builder->AddFontFile(file.Get());

        auto systemSet = ComPtr<IDWriteFontSet>();

        if (SUCCEEDED(factory->GetSystemFontSet(systemSet.GetAddressOf())))
            builder->AddFontSet(systemSet.Get());

        auto set = ComPtr<IDWriteFontSet>();

        if (FAILED(builder->CreateFontSet(set.GetAddressOf())))
            return false;

        auto built = ComPtr<IDWriteFontCollection1>();

        if (FAILED(factory->CreateFontCollectionFromFontSet(set.Get(),
                                                            built.GetAddressOf())))
            return false;

        current = built;

        return true;
    }

    std::mutex mutex;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    std::vector<ComPtr<IDWriteFontFile>> files;
    ComPtr<IDWriteFontCollection> current;
};

FontRegistry& fontRegistry()
{
    static auto registry = FontRegistry {};
    return registry;
}

bool hasFamily(const ComPtr<IDWriteFontCollection>& collection,
               const std::wstring& name)
{
    auto index = UINT32 {};
    auto exists = BOOL {};

    return collection
           && SUCCEEDED(collection->FindFamilyName(name.c_str(), &index, &exists))
           && exists;
}

// Resolves a PostScript or full face name to its family name through the
// collection's font set, empty when nothing matches (or the collection
// predates IDWriteFontCollection1 — registered memory fonts never do).
std::wstring familyOfNamedFace(const ComPtr<IDWriteFontCollection>& collection,
                               const std::wstring& name)
{
    auto withFontSet = ComPtr<IDWriteFontCollection1>();

    if (!collection || FAILED(collection.As(&withFontSet)) || !withFontSet)
        return {};

    auto set = ComPtr<IDWriteFontSet>();

    if (FAILED(withFontSet->GetFontSet(set.GetAddressOf())))
        return {};

    for (auto property: {DWRITE_FONT_PROPERTY_ID_POSTSCRIPT_NAME,
                         DWRITE_FONT_PROPERTY_ID_FULL_NAME})
    {
        const auto filter = DWRITE_FONT_PROPERTY {property, name.c_str(), L""};
        auto matches = ComPtr<IDWriteFontSet>();

        if (FAILED(set->GetMatchingFonts(&filter, 1, matches.GetAddressOf()))
            || !matches || matches->GetFontCount() == 0)
            continue;

        auto exists = BOOL {};
        auto values = ComPtr<IDWriteLocalizedStrings>();

        if (FAILED(matches->GetPropertyValues(0,
                                              DWRITE_FONT_PROPERTY_ID_FAMILY_NAME,
                                              &exists,
                                              values.GetAddressOf()))
            || !exists || !values)
            continue;

        auto length = UINT32 {};

        if (FAILED(values->GetStringLength(0, &length)) || length == 0)
            continue;

        auto family = std::wstring(length, L'\0');

        if (SUCCEEDED(values->GetString(0, family.data(), length + 1)))
            return family;
    }

    return {};
}
} // namespace

ComPtr<IDWriteFontCollection> getFontCollection()
{
    return fontRegistry().collection();
}

bool registerMemoryFontData(const void* data, std::size_t size)
{
    return fontRegistry().add(data, size);
}

std::wstring resolveFontFamilyName(const std::wstring& name)
{
    const auto collection = getFontCollection();

    if (hasFamily(collection, name))
        return name;

    // CTFontCreateWithName also matches PostScript and full names
    // ("ABCDiatypeMono-Regular"), so a name that works on the Apple side must
    // find the same face here rather than silently substituting.
    return familyOfNamedFace(collection, name);
}

} // namespace eacp::Graphics
