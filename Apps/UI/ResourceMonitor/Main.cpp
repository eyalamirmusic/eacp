#include <eacp/UI/Network/OnlineResourceMonitorWindow.h>

#include <Miro/Json.h>
#include <Miro/Reflect.h>
#include <ResEmbed/ResEmbed.h>

// The library's resource monitor window over the clips DownloadAndPlay
// offers. The app declares them and opens the window; listing, fetching with
// a progress bar, cancelling and clearing the folder are all the window's.
//
// It watches DownloadAndPlay's own resource folder rather than its own, so
// what that app fetched shows up here already on disk, what is fetched here
// plays there without a download, and Clear all empties the cache both use.

using namespace eacp;

namespace
{
struct Clip
{
    OnlineResource::Info resource() const
    {
        auto info = OnlineResource::Info {};
        info.name = name;
        info.url = url;
        info.fileName = fileName;
        return info;
    }

    std::string name;
    std::string detail;
    std::string url;
    std::string fileName;

    MIRO_REFLECT(name, detail, url, fileName)
};

struct Catalogue
{
    Vector<Clip> clips;

    MIRO_REFLECT(clips)
};

Catalogue loadCatalogue()
{
    if (auto resource = ResEmbed::get("Clips.json", "Clips"))
        return Miro::createFromJSONString<Catalogue>(resource.toStringView());

    return {};
}

FilePath sharedResourceDirectory()
{
    return FilePath::appSupportDirectory("eacp", "Download and Play") / "Resources";
}

void declareClips()
{
    auto& registry = OnlineResources::get();
    registry.setDirectory(sharedResourceDirectory());

    for (const auto& clip: loadCatalogue().clips)
        registry.declare(clip.resource());
}

struct App
{
    App() { declareClips(); }

    UI::OnlineResourceMonitorWindow window;
};
} // namespace

int main(int argc, char* argv[])
{
    return eacp::Apps::run<App>(argc, argv);
}
