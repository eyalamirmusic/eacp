#pragma once

#include "AppHubPlatform.h"
#include "PrivilegedHelperClient.h"

#include <eacp/Core/App/App.h>
#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/SHA256.h>
#include <eacp/Network/HTTP/Http.h>
#include <eacp/Updater/Updater.h>

#include <Miro/Miro.h>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifndef EACP_APPHUB_VERSION
#define EACP_APPHUB_VERSION "0.0.0"
#endif

#ifndef EACP_APPHUB_DEMO_MANIFEST_URL
#define EACP_APPHUB_DEMO_MANIFEST_URL \
    "https://github.com/Tamber-Inc/eacp/releases/download/remote-demo-v1/manifest.json"
#endif

#ifndef EACP_APPHUB_MANIFEST_URL
#define EACP_APPHUB_MANIFEST_URL \
    "https://github.com/Tamber-Inc/eacp/releases/download/remote-demo-v1/hub-manifest.json"
#endif

#ifndef EACP_APPHUB_CATALOG_URL
#define EACP_APPHUB_CATALOG_URL \
    "https://github.com/Tamber-Inc/eacp/releases/download/remote-demo-v1/apphub-catalog.json"
#endif

namespace Api
{
namespace fs = std::filesystem;
namespace HTTP = eacp::HTTP;
namespace Updater = eacp::Updater;

enum class HubProductKind
{
    App,
    Runtime,
    Model,
    Blob
};

enum class HubInstallState
{
    NotInstalled,
    Installed,
    UpdateAvailable,
    Running
};

enum class HubOperationKind
{
    None,
    Checking,
    DownloadingManifest,
    DownloadingArtifact,
    VerifyingArtifact,
    Installing,
    Launching,
    Updating,
    Resetting
};

enum class HubOperationState
{
    Idle,
    Working,
    Succeeded,
    Failed
};

enum class HubHelperState
{
    Unknown,
    Installed,
    Missing,
    Failed
};

struct HubProduct
{
    std::string id;
    std::string name;
    HubProductKind kind = HubProductKind::App;
    HubInstallState state = HubInstallState::NotInstalled;
    std::string bundleName;
    std::string channel;
    std::string installedVersion;
    std::string latestVersion;
    std::string installPath;
    eacp::Vector<std::string> dependencies;

    MIRO_REFLECT(id,
                 name,
                 kind,
                 state,
                 bundleName,
                 channel,
                 installedVersion,
                 latestVersion,
                 installPath,
                 dependencies)
};

struct RemoteAppStatus
{
    std::string productId;
    std::string name;
    std::string installedVersion;
    std::string latestVersion;
    bool installed = false;
    bool updateAvailable = false;
    std::string message;

    MIRO_REFLECT(productId,
                 name,
                 installedVersion,
                 latestVersion,
                 installed,
                 updateAvailable,
                 message)
};

struct HubOperation
{
    HubOperationKind kind = HubOperationKind::None;
    HubOperationState state = HubOperationState::Idle;
    std::string productId;
    std::string title;
    std::string detail;
    std::int64_t bytesReceived = 0;
    std::int64_t totalBytes = -1;

    MIRO_REFLECT(kind,
                 state,
                 productId,
                 title,
                 detail,
                 bytesReceived,
                 totalBytes)
};

struct HubState
{
    std::string hubVersion = EACP_APPHUB_VERSION;
    std::string root;
    int catalogVersion = 0;
    HubHelperState helperState = HubHelperState::Unknown;
    eacp::Vector<HubProduct> products;
    RemoteAppStatus demoApp;
    RemoteAppStatus hubApp;
    HubOperation operation;

    MIRO_REFLECT(hubVersion,
                 root,
                 catalogVersion,
                 helperState,
                 products,
                 demoApp,
                 hubApp,
                 operation)
};

struct ProductRequest
{
    std::string productId;

    MIRO_REFLECT(productId)
};

struct RemoteInstallRequest
{
    std::string manifestUrl;

    MIRO_REFLECT(manifestUrl)
};

struct CommandResult
{
    bool ok = false;
    std::string message;

    MIRO_REFLECT(ok, message)
};

namespace Detail
{
constexpr std::string_view mazeId = "com.eacp.maze";
constexpr std::string_view teapotId = "com.eacp.teapot";
constexpr std::string_view runtimeId = "shared.onnxruntime";
constexpr std::string_view modelId = "shared.clap";
constexpr std::string_view defaultDemoManifestUrl = EACP_APPHUB_DEMO_MANIFEST_URL;
constexpr std::string_view defaultHubManifestUrl = EACP_APPHUB_MANIFEST_URL;
constexpr std::string_view defaultCatalogUrl = EACP_APPHUB_CATALOG_URL;

inline fs::path defaultRoot()
{
    return AppHub::defaultStateRoot();
}

inline fs::path catalogPath(const fs::path& root)
{
    return root / "catalog.json";
}

inline fs::path stagingRoot(const fs::path& root)
{
    return root / "staging";
}

inline fs::path runningRoot(const fs::path& root)
{
    return root / "running";
}

inline fs::path runningPath(const fs::path& root, const std::string& productId)
{
    return runningRoot(root) / (productId + ".running");
}

inline fs::path remoteDownloadRoot(const fs::path& root)
{
    return root / "remote-downloads";
}

inline fs::path remoteCatalogPath(const fs::path& root)
{
    return remoteDownloadRoot(root) / "apphub-catalog.json";
}

inline void writeFile(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
    out << text;
}

inline std::string readFile(const fs::path& path)
{
    auto in = std::ifstream(path, std::ios::binary);
    if (!in)
        return {};

    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline Updater::Target makeTarget()
{
    return AppHub::currentTarget();
}

inline Updater::MockHelperOptions makeHelperOptions(const fs::path& root)
{
    auto options = Updater::MockHelperOptions();
    options.root = root.string();
    options.stagingRoot = stagingRoot(root).string();
    return options;
}

inline fs::path artifactPath(const fs::path& root, std::string_view productId)
{
    return stagingRoot(root) / (std::string(productId) + ".app.zip");
}

inline std::optional<fs::path> currentExecutablePath()
{
    return AppHub::currentExecutablePath();
}

inline std::optional<fs::path> findBuildAppsRoot()
{
    auto executable = currentExecutablePath();
    if (!executable)
        return std::nullopt;

    for (auto path = executable->parent_path(); !path.empty();
         path = path.parent_path())
    {
        if (fs::exists(path / "Apps" / "GPU" / "Maze" / "Maze.app"))
            return path / "Apps";
        if (path.filename() == "Apps"
            && fs::exists(path / "GPU" / "Maze" / "Maze.app"))
        {
            return path;
        }
        if (path == path.parent_path())
            break;
    }

    return std::nullopt;
}

inline std::optional<fs::path> builtDemoBundle(std::string_view productId)
{
    auto appsRoot = findBuildAppsRoot();
    if (!appsRoot)
        return std::nullopt;

    if (productId == mazeId)
        return *appsRoot / "GPU" / "Maze" / "Maze.app";
    if (productId == teapotId)
        return *appsRoot / "GPU" / "Teapot" / "Teapot.app";
    return std::nullopt;
}

inline bool createAppBundleZip(const fs::path& bundle, const fs::path& output)
{
    return AppHub::createAppBundleZip(bundle, output);
}

inline Updater::ProductArtifact makeArtifact(const fs::path& artifact)
{
    auto out = Updater::ProductArtifact();
    auto target = makeTarget();
    out.platform = target.platform;
    out.architecture = target.architecture;
    out.url = "file://" + artifact.string();
    out.sha256 = eacp::Crypto::sha256File(artifact.string());
    out.signature = "dev-signature-placeholder";
    return out;
}

inline Updater::Product makeProduct(
    const std::string& id,
    const std::string& name,
    Updater::PackageKind kind,
    const std::string& version,
    const fs::path& artifact,
    const eacp::Vector<std::string>& dependencies = {},
    const std::string& bundleName = {})
{
    auto product = Updater::Product();
    product.id = id;
    product.name = name;
    product.kind = kind;
    product.bundleName = bundleName;
    product.channel = "stable";
    product.latestVersion = version;
    product.dependencies = dependencies;
    product.artifacts.add(makeArtifact(artifact));
    return product;
}

inline Updater::ProductCatalog writeDevCatalog(const fs::path& root,
                                               bool updateEditor)
{
    auto runtimeArtifact = artifactPath(root, runtimeId);
    auto modelArtifact = artifactPath(root, modelId);
    auto mazeArtifact = artifactPath(root, mazeId);
    auto teapotArtifact = artifactPath(root, teapotId);

    writeFile(runtimeArtifact, "ONNX Runtime payload v1");
    writeFile(modelArtifact, updateEditor ? "CLAP model payload v2"
                                          : "CLAP model payload v1");

    if (auto bundle = builtDemoBundle(mazeId); bundle && fs::exists(*bundle))
        createAppBundleZip(*bundle, mazeArtifact);
    if (auto bundle = builtDemoBundle(teapotId); bundle && fs::exists(*bundle))
        createAppBundleZip(*bundle, teapotArtifact);

    auto catalog = Updater::ProductCatalog();
    catalog.catalogVersion = updateEditor ? 2 : 1;
    catalog.signature = "dev-catalog-signature-placeholder";

    catalog.products.add(makeProduct(std::string(runtimeId),
                                     "ONNX Runtime",
                                     Updater::PackageKind::Runtime,
                                     "1.0.0",
                                     runtimeArtifact));
    catalog.products.add(makeProduct(std::string(modelId),
                                     "CLAP Model",
                                     Updater::PackageKind::Model,
                                     updateEditor ? "2.0.0" : "1.0.0",
                                     modelArtifact));

    auto appDeps = eacp::Vector<std::string>();
    appDeps.add(std::string(runtimeId));
    appDeps.add(std::string(modelId));

    if (fs::exists(mazeArtifact))
    {
        catalog.products.add(makeProduct(std::string(mazeId),
                                         "Maze",
                                         Updater::PackageKind::App,
                                         updateEditor ? "2.0.0" : "1.0.0",
                                         mazeArtifact,
                                         appDeps,
                                         "Maze.app"));
    }

    if (fs::exists(teapotArtifact))
    {
        catalog.products.add(makeProduct(std::string(teapotId),
                                         "Teapot",
                                         Updater::PackageKind::App,
                                         "1.0.0",
                                         teapotArtifact,
                                         appDeps,
                                         "Teapot.app"));
    }

    writeFile(catalogPath(root), Updater::catalogToJson(catalog));
    return catalog;
}

inline Updater::ProductCatalog loadOrCreateCatalog(const fs::path& root)
{
    auto raw = readFile(catalogPath(root));
    if (!raw.empty())
        return Updater::parseCatalogJson(raw);

    return writeDevCatalog(root, false);
}

inline Updater::MockPrivilegedHelper makeMockHelper(const fs::path& root)
{
    return Updater::MockPrivilegedHelper(makeHelperOptions(root));
}

inline HubProductKind productKindFrom(Updater::PackageKind kind)
{
    switch (kind)
    {
        case Updater::PackageKind::App:
            return HubProductKind::App;
        case Updater::PackageKind::Runtime:
            return HubProductKind::Runtime;
        case Updater::PackageKind::Model:
            return HubProductKind::Model;
        case Updater::PackageKind::Blob:
            return HubProductKind::Blob;
    }
    return HubProductKind::Blob;
}

inline bool isRunning(const fs::path& root, const std::string& productId)
{
    auto ec = std::error_code();
    return fs::exists(runningPath(root, productId), ec);
}

inline std::string executableVersion(std::string_view executable)
{
    auto ec = std::error_code();
    if (!fs::exists(fs::path(executable), ec))
        return {};

    auto result = eacp::Processes::run(std::string(executable), {"--version"});
    if (!result.exited || result.exitCode != 0)
        return {};

    auto out = result.output;
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())))
        out.pop_back();
    return out;
}

inline std::optional<Updater::RemoteAppManifest> fetchRemoteManifest(
    std::string_view url)
{
    auto response = HTTP::Request(std::string(url)).perform();
    if (response.statusCode < 200 || response.statusCode >= 300)
        return std::nullopt;

    auto manifest = Updater::RemoteAppManifest();
    try
    {
        Miro::fromJSONString(manifest, response.content);
    }
    catch (...)
    {
        return std::nullopt;
    }

    return manifest;
}

inline std::optional<Updater::ProductCatalog> fetchRemoteCatalog(
    const fs::path& root)
{
    auto response = HTTP::Request(std::string(defaultCatalogUrl)).perform();
    if (response.statusCode < 200 || response.statusCode >= 300)
        return std::nullopt;

    try
    {
        auto catalog = Updater::parseCatalogJson(response.content);
        writeFile(remoteCatalogPath(root), response.content);
        return catalog;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

inline Updater::ProductCatalog loadCatalog(const fs::path& root,
                                           bool preferRemote = true)
{
    if (preferRemote)
    {
        if (auto remote = fetchRemoteCatalog(root))
            return *remote;
    }

    auto remoteRaw = readFile(remoteCatalogPath(root));
    if (!remoteRaw.empty())
    {
        try
        {
            return Updater::parseCatalogJson(remoteRaw);
        }
        catch (...)
        {
        }
    }

    return loadOrCreateCatalog(root);
}

inline Updater::ProductCatalog loadCatalogContaining(const fs::path& root,
                                                     const std::string& productId)
{
    for (auto attempt = 0; attempt < 12; ++attempt)
    {
        if (auto remote = fetchRemoteCatalog(root))
        {
            if (Updater::findProduct(*remote, productId) != nullptr)
                return *remote;
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    return loadCatalog(root, false);
}

inline std::string nowUtc()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto tm = std::tm {};

    if (auto* utc = std::gmtime(&time))
        tm = *utc;

    auto out = std::ostringstream();
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}
} // namespace Detail

class AppHubApi
{
public:
    AppHubApi()
    {
        refreshState("Ready", true);
    }

    explicit AppHubApi(fs::path rootToUse)
        : root(std::move(rootToUse))
    {
        refreshState("Ready", true);
    }

    void reflect(Miro::ApiReflector& r)
    {
        using T = AppHubApi;
        r.commands<&T::getHubState,
                   &T::refresh,
                   &T::checkUpdates,
                   &T::installProduct,
                   &T::openProduct,
                   &T::closeProduct,
                   &T::updateAll,
                   &T::publishMockUpdate,
                   &T::resetMock,
                   &T::installDemoApp,
                   &T::updateHub,
                   &T::launchDemoApp,
                   &T::launchHub,
                   &T::installPrivilegedHelper>();
        r.events<&T::hubState>();
    }

    HubState getHubState() const
    {
        auto lock = std::lock_guard(stateMutex);
        return state;
    }

    CommandResult refresh()
    {
        refreshState("Ready", true);
        return ok("Refreshed");
    }

    CommandResult checkUpdates()
    {
        beginOperation(HubOperationKind::Checking,
                       "Checking for updates",
                       "Fetching remote catalog");
        refreshState("Fetched remote catalog", true);
        finishOperation(true, "Update check complete");
        updateHubStatus();
        return ok("Update check complete");
    }

    CommandResult installProduct(const ProductRequest& request)
    {
        if (request.productId.empty())
            return fail("Install requires a product id");

        beginOperation(HubOperationKind::Installing,
                       "Installing " + request.productId,
                       "Planning dependency install",
                       request.productId);

        auto catalog = Detail::loadCatalogContaining(root, request.productId);
        auto helper = Detail::makeMockHelper(root);
        auto plan = Updater::planInstallWithDependencies(catalog,
                                                         helper.receipts(),
                                                         request.productId,
                                                         Detail::makeTarget(),
                                                         Detail::stagingRoot(root)
                                                             .string());

        if (plan.operations.empty())
        {
            finishOperation(false, "No install plan for " + request.productId);
            return fail("No install plan for " + request.productId);
        }

        if (auto staged = stagePlanArtifacts(plan, catalog); !staged.ok)
        {
            finishOperation(false, staged.message);
            return staged;
        }

        auto result = executeInstallPlan(plan, catalog);
        refreshState(result.ok ? "Installed " + request.productId
                               : result.message);
        finishOperation(result.ok,
                        result.ok ? "Install complete" : result.message);
        return result.ok ? ok("Installed " + request.productId) : result;
    }

    CommandResult openProduct(const ProductRequest& request)
    {
        auto helper = Detail::makeMockHelper(root);
        auto receipts = helper.receipts();
        auto* receipt = Updater::findReceipt(receipts, request.productId);
        auto catalog = Detail::loadCatalog(root, false);
        auto* product = Updater::findProduct(catalog, request.productId);
        if (receipt == nullptr && product == nullptr)
            return fail(request.productId + " is not installed");

        beginOperation(HubOperationKind::Launching,
                       "Opening " + request.productId,
                       "Launching installed app bundle",
                       request.productId);
        auto app = std::optional<fs::path>();
        if (receipt != nullptr)
            app = installedAppBundle(*receipt);
        if (!app && product != nullptr && !product->bundleName.empty())
            app = AppHub::installedAppBundlePath(product->bundleName);

        if (!app)
        {
            auto message = request.productId + " does not have an installed app bundle";
            finishOperation(false, message);
            return fail(message);
        }

        if (auto launched = AppHub::openAppBundle(app->string()); !launched.ok)
        {
            auto message = launched.error.empty() ? "Launch failed"
                                                  : launched.error;
            finishOperation(false, message);
            return fail(message);
        }

        Detail::writeFile(Detail::runningPath(root, request.productId), "running");
        refreshState("Opened " + request.productId);
        finishOperation(true, "App running");
        return ok("Opened " + request.productId);
    }

    CommandResult closeProduct(const ProductRequest& request)
    {
        std::error_code ec;
        fs::remove(Detail::runningPath(root, request.productId), ec);
        if (ec)
            return fail(ec.message());

        refreshState("Closed " + request.productId);
        return ok("Closed " + request.productId);
    }

    CommandResult updateAll()
    {
        beginOperation(HubOperationKind::Updating,
                       "Updating installed products",
                       "Planning available updates");

        auto catalog = Detail::loadCatalog(root);
        auto helper = Detail::makeMockHelper(root);
        auto receipts = helper.receipts();

        for (const auto& receipt: receipts)
        {
            if (Detail::isRunning(root, receipt.productId))
            {
                auto message = "Close running apps before updating";
                refreshState(message);
                finishOperation(false, message);
                return fail(message);
            }
        }

        auto plan = Updater::planUpdateAll(catalog,
                                           receipts,
                                           Detail::makeTarget(),
                                           Detail::stagingRoot(root).string());
        if (plan.operations.empty())
        {
            finishOperation(true, "No updates available");
            return ok("No updates available");
        }

        if (auto staged = stagePlanArtifacts(plan, catalog); !staged.ok)
        {
            finishOperation(false, staged.message);
            return staged;
        }

        auto result = executeInstallPlan(plan, catalog);
        refreshState(result.ok ? "Updated installed products" : result.message);
        finishOperation(result.ok,
                        result.ok ? "Update complete" : result.message);
        return result.ok ? ok("Updated installed products") : result;
    }

    CommandResult publishMockUpdate()
    {
        beginOperation(HubOperationKind::Updating,
                       "Publishing mock update",
                       "Writing catalog version 2");
        Detail::writeDevCatalog(root, true);
        refreshState("Published mock update", false);
        finishOperation(true, "Mock update published");
        return ok("Mock update published");
    }

    CommandResult resetMock()
    {
        beginOperation(HubOperationKind::Resetting,
                       "Resetting mock products",
                       "Clearing user-space helper root");
        std::error_code ec;
        fs::remove_all(root, ec);
        if (ec)
        {
            finishOperation(false, ec.message());
            return fail(ec.message());
        }

        Detail::writeDevCatalog(root, false);
        auto helper = Detail::makeMockHelper(root);
        if (!helper.isInstalled())
        {
            auto message = "Mock helper root could not be created";
            finishOperation(false, message);
            return fail(message);
        }

        refreshState("Reset complete", true);
        finishOperation(true, "Reset complete");
        return ok("Reset complete");
    }

    CommandResult installDemoApp(const RemoteInstallRequest& request)
    {
        return installRemoteApp(request.manifestUrl.empty()
                                    ? std::string(Detail::defaultDemoManifestUrl)
                                    : request.manifestUrl);
    }

    CommandResult updateHub(const RemoteInstallRequest& request)
    {
        auto manifestUrl = request.manifestUrl.empty()
                               ? std::string(Detail::defaultHubManifestUrl)
                               : request.manifestUrl;
        auto manifest = Detail::fetchRemoteManifest(manifestUrl);
        if (!manifest)
            return fail("Manifest download failed");

        auto installedVersion = Detail::executableVersion(
            AppHub::installedHubAppExecutablePath().string());
        if (!installedVersion.empty()
            && !Updater::isNewerVersion(manifest->version, installedVersion))
        {
            updateHubStatus();
            return ok("AppHub is up to date: " + installedVersion);
        }

        return installRemoteApp(manifestUrl, std::move(manifest));
    }

    CommandResult launchDemoApp()
    {
        beginOperation(HubOperationKind::Launching,
                       "Launching Demo App",
                       AppHub::installedDemoAppBundlePath().string());
        auto launched =
            AppHub::openAppBundle(AppHub::installedDemoAppBundlePath().string());
        finishOperation(launched.ok, launched.ok ? "Demo App launched"
                                                 : launched.error);
        return launched.ok ? ok("Demo App launched") : fail(launched.error);
    }

    CommandResult launchHub()
    {
        beginOperation(HubOperationKind::Launching,
                       "Launching AppHub",
                       AppHub::installedHubAppBundlePath().string());
        auto launched =
            AppHub::openAppBundle(AppHub::installedHubAppBundlePath().string());
        finishOperation(launched.ok, launched.ok ? "AppHub launched"
                                                 : launched.error);
        return launched.ok ? ok("AppHub launched") : fail(launched.error);
    }

    CommandResult installPrivilegedHelper()
    {
        beginOperation(HubOperationKind::Installing,
                       "Installing privileged helper",
                       "Requesting SMJobBless");
        auto result = AppHub::installPrivilegedHelper();
        setHelperState(result.ok ? HubHelperState::Installed : HubHelperState::Failed);
        finishOperation(result.ok, result.ok ? "Privileged helper installed"
                                             : result.error);
        return result.ok ? ok("Privileged helper installed") : fail(result.error);
    }

    Miro::Event<HubState> hubState;

private:
    CommandResult installRemoteApp(
        const std::string& manifestUrl,
        std::optional<Updater::RemoteAppManifest> prefetchedManifest = std::nullopt)
    {
        beginOperation(HubOperationKind::DownloadingManifest,
                       "Downloading manifest",
                       manifestUrl);

        auto manifest = std::move(prefetchedManifest);
        if (!manifest)
            manifest = Detail::fetchRemoteManifest(manifestUrl);
        if (!manifest)
        {
            finishOperation(false, "Manifest download failed");
            return fail("Manifest download failed");
        }

        if (!Updater::isValidProductId(manifest->productId)
            || !Updater::isValidAppBundleName(manifest->bundleName)
            || manifest->artifact.url.empty()
            || manifest->artifact.sha256.empty())
        {
            finishOperation(false, "Manifest failed validation");
            return fail("Manifest failed validation");
        }

        auto downloads = Detail::remoteDownloadRoot(root);
        auto artifactPath = downloads / (manifest->productId + ".app.zip");
        std::error_code ec;
        fs::create_directories(downloads, ec);
        if (ec)
        {
            finishOperation(false, ec.message());
            return fail(ec.message());
        }

        auto progress = HTTP::DownloadProgress();
        auto request = HTTP::Request(manifest->artifact.url);
        request.progress = &progress;
        request.parallelChunks = 4;

        beginOperation(HubOperationKind::DownloadingArtifact,
                       "Downloading " + manifest->name + " " + manifest->version,
                       manifest->artifact.url,
                       manifest->productId);

        auto response = HTTP::Response();
        auto downloadThread = std::thread(
            [&] { response = request.downloadTo(artifactPath.string()); });

        while (!progress.done.load())
        {
            updateProgress(progress.bytesReceived.load(),
                           progress.totalBytes.load());
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }

        downloadThread.join();
        updateProgress(progress.bytesReceived.load(), progress.totalBytes.load());

        if (response.statusCode < 200 || response.statusCode >= 300)
        {
            auto message = response.error.empty() ? "Artifact download failed"
                                                  : response.error;
            finishOperation(false, message);
            return fail(message);
        }

        beginOperation(HubOperationKind::VerifyingArtifact,
                       "Verifying artifact",
                       manifest->artifact.sha256,
                       manifest->productId);
        auto actualHash = eacp::Crypto::sha256File(artifactPath.string());
        if (actualHash != manifest->artifact.sha256)
        {
            finishOperation(false, "Artifact hash mismatch");
            return fail("Artifact hash mismatch");
        }

        auto installRequest = Updater::PrivilegedAppBundleInstallRequest();
        installRequest.productId = manifest->productId;
        installRequest.name = manifest->name;
        installRequest.version = manifest->version;
        installRequest.bundleName = manifest->bundleName;
        installRequest.artifactPath = artifactPath.string();
        installRequest.artifactSha256 = manifest->artifact.sha256;

        beginOperation(HubOperationKind::Installing,
                       "Installing " + manifest->name,
                       manifest->bundleName,
                       manifest->productId);
        auto installResult =
            AppHub::installAppBundleWithPrivilegedHelper(installRequest);
        if (!installResult.ok)
        {
            setHelperState(HubHelperState::Failed);
            finishOperation(false, installResult.error);
            return fail(installResult.error);
        }

        if (manifest->productId == "com.tamber.AppHub")
            updateHubStatus();
        else
            refreshState("Installed " + manifest->name + " " + manifest->version,
                         true);
        finishOperation(true, "Installed " + manifest->name + " "
                                  + manifest->version);
        if (manifest->productId == "com.tamber.AppHub")
            relaunchInstalledHub();
        return ok("Installed " + manifest->name + " " + manifest->version);
    }

    void relaunchInstalledHub()
    {
        beginOperation(HubOperationKind::Launching,
                       "Relaunching AppHub",
                       AppHub::installedHubAppBundlePath().string(),
                       "com.tamber.AppHub");
        auto launched = AppHub::openNewAppBundleInstance(
            AppHub::installedHubAppBundlePath().string());
        if (!launched.ok)
        {
            auto message = launched.error.empty()
                               ? "Installed AppHub, but relaunch failed"
                               : launched.error;
            finishOperation(false, message);
            return;
        }

        eacp::Threads::callAsync([] { eacp::Apps::quit(); });
    }

    CommandResult stagePlanArtifacts(const Updater::InstallPlan& plan,
                                     const Updater::ProductCatalog& catalog)
    {
        for (const auto& operation: plan.operations)
        {
            if (operation.action == Updater::PlanAction::Remove)
                continue;

            auto* product = Updater::findProduct(catalog, operation.productId);
            if (product == nullptr)
                return fail("Product disappeared from catalog: "
                            + operation.productId);

            auto artifact = Updater::artifactForTarget(*product,
                                                       Detail::makeTarget());
            if (artifact.url.empty())
                return fail("No matching artifact for " + operation.productId);

            if (artifact.url.rfind("file://", 0) == 0)
            {
                auto source = fs::path(artifact.url.substr(7));
                if (source == fs::path(operation.artifactPath))
                    continue;

                std::error_code ec;
                fs::create_directories(fs::path(operation.artifactPath)
                                           .parent_path(),
                                       ec);
                fs::copy_file(source,
                              operation.artifactPath,
                              fs::copy_options::overwrite_existing,
                              ec);
                if (ec)
                    return fail("Failed to stage " + operation.productId);
                continue;
            }

            beginOperation(HubOperationKind::DownloadingArtifact,
                           "Downloading " + product->name,
                           artifact.url,
                           operation.productId);

            auto progress = HTTP::DownloadProgress();
            auto request = HTTP::Request(artifact.url);
            request.progress = &progress;
            request.parallelChunks = 4;

            std::error_code ec;
            fs::create_directories(fs::path(operation.artifactPath).parent_path(),
                                   ec);
            if (ec)
                return fail(ec.message());

            auto response = HTTP::Response();
            auto downloadThread = std::thread(
                [&] { response = request.downloadTo(operation.artifactPath); });

            while (!progress.done.load())
            {
                updateProgress(progress.bytesReceived.load(),
                               progress.totalBytes.load());
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
            }

            downloadThread.join();
            updateProgress(progress.bytesReceived.load(),
                           progress.totalBytes.load());

            if (response.statusCode < 200 || response.statusCode >= 300)
            {
                return fail(response.error.empty()
                                ? "Failed to download " + product->name
                                : response.error);
            }
        }

        return ok("Artifacts staged");
    }

    CommandResult executeInstallPlan(const Updater::InstallPlan& plan,
                                     const Updater::ProductCatalog& catalog)
    {
        auto helper = Detail::makeMockHelper(root);

        for (const auto& operation: plan.operations)
        {
            auto* product = Updater::findProduct(catalog, operation.productId);
            if (product == nullptr)
                return fail("Product disappeared from catalog: "
                            + operation.productId);

            if (operation.action == Updater::PlanAction::Remove)
            {
                auto single = Updater::InstallPlan();
                single.operations.add(operation);
                auto result = helper.submit(single);
                if (!result.ok)
                    return fail(result.error);
                continue;
            }

            if (product->kind == Updater::PackageKind::App
                && !product->bundleName.empty())
            {
                auto result = installAppProduct(operation, *product);
                if (!result.ok)
                    return result;
                continue;
            }

            auto single = Updater::InstallPlan();
            single.operations.add(operation);
            auto result = helper.submit(single);
            if (!result.ok)
                return fail(result.error);
        }

        return ok("Install plan complete");
    }

    CommandResult installAppProduct(const Updater::PlanOperation& operation,
                                    const Updater::Product& product)
    {
        beginOperation(HubOperationKind::Installing,
                       "Installing " + product.name,
                       product.bundleName,
                       product.id);

        auto request = Updater::PrivilegedAppBundleInstallRequest();
        request.productId = product.id;
        request.name = product.name;
        request.version = operation.version;
        request.bundleName = product.bundleName;
        request.artifactPath = operation.artifactPath;
        request.artifactSha256 = operation.artifactSha256;

        auto result = AppHub::installAppBundleWithPrivilegedHelper(request);
        if (!result.ok)
        {
            setHelperState(HubHelperState::Failed);
            return fail(result.error);
        }

        writeAppReceipt(operation, product);
        setHelperState(HubHelperState::Installed);
        return ok("Installed " + product.name);
    }

    void writeAppReceipt(const Updater::PlanOperation& operation,
                         const Updater::Product& product)
    {
        auto receipt = Updater::ProductReceipt();
        receipt.productId = product.id;
        receipt.name = product.name;
        receipt.version = operation.version;
        receipt.installPath =
            AppHub::installedAppBundlePath(product.bundleName).string();
        receipt.channel = operation.channel;
        receipt.artifactSha256 = operation.artifactSha256;
        receipt.installedAt = Detail::nowUtc();

        auto helper = Detail::makeMockHelper(root);
        Detail::writeFile(fs::path(helper.receiptsRoot())
                              / (product.id + ".json"),
                          Updater::receiptToJson(receipt));
    }

    std::optional<fs::path> installedAppBundle(
        const Updater::ProductReceipt& receipt) const
    {
        auto rootPath = fs::path(receipt.installPath);
        if (receipt.installPath.size() >= 4
            && receipt.installPath.compare(receipt.installPath.size() - 4,
                                           4,
                                           ".app")
                   == 0)
        {
            return rootPath;
        }

        auto ec = std::error_code();
        if (!fs::exists(rootPath, ec))
            return std::nullopt;

        for (const auto& entry: fs::directory_iterator(rootPath, ec))
        {
            if (!ec && entry.is_directory()
                && entry.path().extension() == ".app")
            {
                return entry.path();
            }
        }

        return std::nullopt;
    }

    void refreshState(const std::string& detail, bool preferRemote = false)
    {
        auto catalog = Detail::loadCatalog(root, preferRemote);
        auto helper = Detail::makeMockHelper(root);
        auto receipts = helper.receipts();

        auto next = HubState();
        next.hubVersion = EACP_APPHUB_VERSION;
        next.root = root.string();
        next.catalogVersion = catalog.catalogVersion;
        next.helperState = helperState;
        next.operation = operationSnapshot();
        next.demoApp = remoteStatusFromLocal(
            AppHub::installedDemoAppExecutablePath().string(),
            "Demo App");
        next.hubApp = remoteStatusFromLocal(
            AppHub::installedHubAppExecutablePath().string(),
            "AppHub");
        next.operation.detail = detail.empty() ? next.operation.detail : detail;

        for (const auto& product: catalog.products)
        {
            auto view = HubProduct();
            view.id = product.id;
            view.name = product.name;
            view.kind = Detail::productKindFrom(product.kind);
            view.bundleName = product.bundleName;
            view.channel = product.channel;
            view.latestVersion = product.latestVersion;
            view.dependencies = product.dependencies;

            if (auto* receipt = Updater::findReceipt(receipts, product.id))
            {
                view.installedVersion = receipt->version;
                view.installPath = receipt->installPath;
                view.state = Updater::isNewerVersion(product.latestVersion,
                                                     receipt->version)
                                 ? HubInstallState::UpdateAvailable
                                 : HubInstallState::Installed;
                if (Detail::isRunning(root, product.id))
                    view.state = HubInstallState::Running;
            }

            next.products.add(std::move(view));
        }

        publish(next);
    }

    void updateHubStatus()
    {
        auto hub = remoteStatusFromManifest(Detail::defaultHubManifestUrl,
                                            AppHub::installedHubAppExecutablePath()
                                                .string(),
                                            "AppHub");

        {
            auto lock = std::lock_guard(stateMutex);
            state.hubApp = std::move(hub);
        }
        publishCurrent();
    }

    RemoteAppStatus remoteStatusFromLocal(const std::string& executable,
                                          std::string_view fallbackName) const
    {
        auto status = RemoteAppStatus();
        status.name = std::string(fallbackName);
        status.installedVersion = Detail::executableVersion(executable);
        status.installed = !status.installedVersion.empty();
        status.message = status.installed ? "Installed " + status.installedVersion
                                          : "Not installed";
        return status;
    }

    RemoteAppStatus remoteStatusFromManifest(std::string_view manifestUrl,
                                             std::string_view executable,
                                             std::string_view fallbackName) const
    {
        auto status = remoteStatusFromLocal(std::string(executable), fallbackName);
        auto manifest = Detail::fetchRemoteManifest(manifestUrl);
        if (!manifest)
        {
            status.message = "Update check failed";
            return status;
        }

        status.productId = manifest->productId;
        status.name = manifest->name;
        status.latestVersion = manifest->version;
        status.updateAvailable =
            status.installed
            && Updater::isNewerVersion(status.latestVersion,
                                       status.installedVersion);
        if (!status.installed)
            status.message = status.name + " is not installed";
        else if (status.updateAvailable)
            status.message = status.name + " " + status.latestVersion
                             + " is available";
        else
            status.message = status.name + " is up to date";
        return status;
    }

    void beginOperation(HubOperationKind kind,
                        const std::string& title,
                        const std::string& detail,
                        const std::string& productId = {})
    {
        {
            auto lock = std::lock_guard(stateMutex);
            state.operation.kind = kind;
            state.operation.state = HubOperationState::Working;
            state.operation.productId = productId;
            state.operation.title = title;
            state.operation.detail = detail;
            state.operation.bytesReceived = 0;
            state.operation.totalBytes = -1;
        }
        publishCurrent();
    }

    void updateProgress(std::int64_t bytesReceived, std::int64_t totalBytes)
    {
        {
            auto lock = std::lock_guard(stateMutex);
            state.operation.bytesReceived = bytesReceived;
            state.operation.totalBytes = totalBytes;
        }
        publishCurrent();
    }

    void finishOperation(bool ok, const std::string& detail)
    {
        {
            auto lock = std::lock_guard(stateMutex);
            state.operation.state =
                ok ? HubOperationState::Succeeded : HubOperationState::Failed;
            state.operation.detail = detail;
        }
        refreshState(detail);
    }

    void setHelperState(HubHelperState next)
    {
        auto lock = std::lock_guard(stateMutex);
        helperState = next;
        state.helperState = next;
    }

    HubOperation operationSnapshot() const
    {
        auto lock = std::lock_guard(stateMutex);
        return state.operation;
    }

    void publishCurrent()
    {
        auto snapshot = HubState();
        {
            auto lock = std::lock_guard(stateMutex);
            snapshot = state;
        }
        publish(std::move(snapshot));
    }

    void publish(HubState next)
    {
        {
            auto lock = std::lock_guard(stateMutex);
            state = next;
        }

        eacp::Threads::callAsync(
            [this, next = std::move(next)]() mutable
            { hubState.publish(std::move(next)); });
    }

    static CommandResult ok(const std::string& message)
    {
        return {.ok = true, .message = message};
    }

    static CommandResult fail(const std::string& message)
    {
        return {.ok = false, .message = message};
    }

    fs::path root = Detail::defaultRoot();
    mutable std::mutex stateMutex;
    HubState state;
    HubHelperState helperState = HubHelperState::Unknown;
};

} // namespace Api
