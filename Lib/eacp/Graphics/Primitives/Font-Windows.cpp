#include "Font.h"
#include "../D2D-Windows.h"
#include "../Helpers/StringUtils-Windows.h"

#include <dwrite_3.h>

#include <cstddef>
#include <vector>

namespace eacp::Graphics
{
IDWriteFactory* getDWriteFactory();
}

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

namespace
{
std::vector<std::vector<unsigned char>>& applicationFontData()
{
    static auto blobs = std::vector<std::vector<unsigned char>> {};
    return blobs;
}

ComPtr<IDWriteFontCollection1>& applicationCollectionCache()
{
    static auto collection = ComPtr<IDWriteFontCollection1> {};
    return collection;
}

bool collectionContains(IDWriteFontCollection* collection,
                        const std::wstring& family)
{
    if (collection == nullptr)
        return false;

    auto index = UINT32 {0};
    auto exists = BOOL {FALSE};
    collection->FindFamilyName(family.c_str(), &index, &exists);
    return exists != FALSE;
}
} // namespace

// Registers TTF/OTF bytes (copied) with the collection Font consults before
// the system's, so embedded fonts resolve by name with no install step — the
// Windows counterpart of CTFontManagerRegisterGraphicsFont.
void registerInMemoryFont(const void* data, std::size_t bytes)
{
    if (data == nullptr || bytes == 0)
        return;

    const auto* begin = static_cast<const unsigned char*>(data);
    applicationFontData().emplace_back(begin, begin + bytes);
    applicationCollectionCache().Reset();
}

// The DirectWrite collection holding every registered in-memory font; null
// until something registers. Rebuilt lazily after each registration.
IDWriteFontCollection1* getApplicationFontCollection()
{
    auto& cache = applicationCollectionCache();

    if (cache)
        return cache.Get();

    if (applicationFontData().empty())
        return nullptr;

    auto* base = getDWriteFactory();
    auto factory = ComPtr<IDWriteFactory5> {};

    if (base == nullptr
        || FAILED(base->QueryInterface(IID_PPV_ARGS(factory.GetAddressOf()))))
        return nullptr;

    // The loader must outlive every font file it backs, so it registers once
    // and stays for the process lifetime.
    static auto loader = ComPtr<IDWriteInMemoryFontFileLoader> {};

    if (!loader)
    {
        if (FAILED(factory->CreateInMemoryFontFileLoader(loader.GetAddressOf())))
            return nullptr;

        factory->RegisterFontFileLoader(loader.Get());
    }

    auto builder = ComPtr<IDWriteFontSetBuilder1> {};

    if (FAILED(factory->CreateFontSetBuilder(builder.GetAddressOf())))
        return nullptr;

    for (const auto& blob: applicationFontData())
    {
        auto file = ComPtr<IDWriteFontFile> {};

        if (SUCCEEDED(loader->CreateInMemoryFontFileReference(factory.Get(),
                                                              blob.data(),
                                                              (UINT32) blob.size(),
                                                              nullptr,
                                                              file.GetAddressOf())))
            builder->AddFontFile(file.Get());
    }

    auto set = ComPtr<IDWriteFontSet> {};

    if (FAILED(builder->CreateFontSet(set.GetAddressOf())))
        return nullptr;

    factory->CreateFontCollectionFromFontSet(set.Get(), cache.GetAddressOf());
    return cache.Get();
}

struct Font::Native
{
    Native() {}

    void update(const FontOptions& options)
    {
        textFormat.Reset();

        auto* factory = getDWriteFactory();
        if (!factory)
            return;

        auto wideName = toWideString(options.name);

        // In-memory fonts win over the system collection, so an embedded
        // face resolves by the same name lookup as an installed one.
        auto* custom = getApplicationFontCollection();
        auto* collection = collectionContains(custom, wideName) ? custom : nullptr;

        factory->CreateTextFormat(wideName.c_str(),
                                  collection,
                                  DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL,
                                  options.size,
                                  L"en-us",
                                  textFormat.GetAddressOf());

        if (!textFormat)
        {
            factory->CreateTextFormat(L"Arial",
                                      nullptr,
                                      DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL,
                                      options.size,
                                      L"en-us",
                                      textFormat.GetAddressOf());
        }

        if (textFormat)
            textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    ComPtr<IDWriteTextFormat> textFormat;
};

Font::Font(const FontOptions& optionsToUse)
    : options(optionsToUse)
    , impl()
{
    updateNativeFont();
}

void Font::setFont(const FontOptions& optionsToUse)
{
    options = optionsToUse;
    updateNativeFont();
}

void* Font::getHandle() const
{
    return impl->textFormat.Get();
}

void Font::updateNativeFont()
{
    impl->update(options);
}

} // namespace eacp::Graphics
