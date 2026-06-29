#include <eacp/Updater/Updater.h>

#include <eacp/Core/Utils/SHA256.h>

#include "PrivilegedHelperClient.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
namespace Updater = eacp::Updater;

#if !defined(_WIN32)
extern char** environ;
#endif

namespace
{
constexpr auto platform = Updater::Platform::MacOS;
constexpr auto architecture = Updater::Architecture::Universal;
constexpr std::string_view editorId = "tamber.editor";
constexpr std::string_view captureId = "tamber.capture";
constexpr std::string_view runtimeId = "shared.onnxruntime";
constexpr std::string_view modelId = "shared.clap";

struct CliOptions
{
    fs::path root = fs::temp_directory_path() / "eacp-apphub-demo";
    std::string command = "tui";
    std::string productId;
    std::string manifestUrl;
};

struct RemoteAppArtifact
{
    std::string url;
    std::string sha256;

    MIRO_REFLECT(url, sha256)
};

struct RemoteAppManifest
{
    std::string productId;
    std::string name;
    std::string version;
    std::string bundleName;
    RemoteAppArtifact artifact;

    MIRO_REFLECT(productId, name, version, bundleName, artifact)
};

void writeFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string stringFrom(std::string_view value)
{
    return std::string(value);
}

Updater::Target makeTarget()
{
    auto target = Updater::Target();
    target.platform = platform;
    target.architecture = architecture;
    return target;
}

Updater::ProductArtifact makeArtifact(const fs::path& artifact)
{
    auto out = Updater::ProductArtifact();
    out.platform = platform;
    out.architecture = architecture;
    out.url = "file://" + artifact.string();
    out.sha256 = eacp::Crypto::sha256File(artifact.string());
    out.signature = "dev-signature-placeholder";
    return out;
}

std::string readFile(const fs::path& path)
{
    auto in = std::ifstream(path, std::ios::binary);
    if (!in)
        return {};

    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

fs::path catalogPath(const fs::path& root)
{
    return root / "catalog.json";
}

fs::path stagingRoot(const fs::path& root)
{
    return root / "staging";
}

fs::path runningRoot(const fs::path& root)
{
    return root / "running";
}

fs::path runningPath(const fs::path& root, const std::string& productId)
{
    return runningRoot(root) / (productId + ".running");
}

fs::path remoteDownloadRoot(const fs::path& root)
{
    return root / "remote-downloads";
}

fs::path remoteUnpackRoot(const fs::path& root)
{
    return root / "remote-unpack";
}

Updater::MockHelperOptions makeHelperOptions(const fs::path& root)
{
    auto options = Updater::MockHelperOptions();
    options.root = root.string();
    options.stagingRoot = stagingRoot(root).string();
    return options;
}

fs::path artifactPath(const fs::path& root, std::string_view productId)
{
    return stagingRoot(root) / (stringFrom(productId) + ".artifact");
}

Updater::Product makeProduct(const std::string& id,
                             const std::string& name,
                             Updater::PackageKind kind,
                             const std::string& version,
                             const fs::path& artifact,
                             const eacp::Vector<std::string>& dependencies = {})
{
    auto product = Updater::Product();
    product.id = id;
    product.name = name;
    product.kind = kind;
    product.channel = "stable";
    product.latestVersion = version;
    product.dependencies = dependencies;
    product.artifacts.add(makeArtifact(artifact));
    return product;
}

Updater::ProductCatalog writeDevCatalog(const fs::path& root, bool updateEditor)
{
    auto editorArtifact = artifactPath(root, editorId);
    auto captureArtifact = artifactPath(root, captureId);
    auto runtimeArtifact = artifactPath(root, runtimeId);
    auto modelArtifact = artifactPath(root, modelId);

    writeFile(editorArtifact,
              updateEditor ? "Example Editor payload v2"
                           : "Example Editor payload v1");
    writeFile(captureArtifact, "Example Capture payload v1");
    writeFile(runtimeArtifact, "ONNX Runtime payload v1");
    writeFile(modelArtifact, updateEditor ? "CLAP model payload v2"
                                          : "CLAP model payload v1");

    auto catalog = Updater::ProductCatalog();
    catalog.catalogVersion = updateEditor ? 2 : 1;
    catalog.signature = "dev-catalog-signature-placeholder";

    catalog.products.add(makeProduct(stringFrom(runtimeId),
                                     "ONNX Runtime",
                                     Updater::PackageKind::Runtime,
                                     "1.0.0",
                                     runtimeArtifact));
    catalog.products.add(makeProduct(stringFrom(modelId),
                                     "CLAP Model",
                                     Updater::PackageKind::Model,
                                     updateEditor ? "2.0.0" : "1.0.0",
                                     modelArtifact));

    auto appDeps = eacp::Vector<std::string>();
    appDeps.add(stringFrom(runtimeId));
    appDeps.add(stringFrom(modelId));

    catalog.products.add(makeProduct(stringFrom(editorId),
                                     "Example Editor",
                                     Updater::PackageKind::App,
                                     updateEditor ? "2.0.0" : "1.0.0",
                                     editorArtifact,
                                     appDeps));
    catalog.products.add(makeProduct(stringFrom(captureId),
                                     "Example Capture",
                                     Updater::PackageKind::App,
                                     "1.0.0",
                                     captureArtifact,
                                     appDeps));

    writeFile(catalogPath(root), Updater::catalogToJson(catalog));
    return catalog;
}

Updater::ProductCatalog loadOrCreateCatalog(const fs::path& root)
{
    auto raw = readFile(catalogPath(root));
    if (!raw.empty())
        return Updater::parseCatalogJson(raw);

    return writeDevCatalog(root, false);
}

Updater::MockPrivilegedHelper makeHelper(const fs::path& root)
{
    return Updater::MockPrivilegedHelper(makeHelperOptions(root));
}

bool isRunning(const fs::path& root, const std::string& productId)
{
    auto ec = std::error_code();
    return fs::exists(runningPath(root, productId), ec);
}

eacp::Vector<std::string> runningProducts(const fs::path& root)
{
    auto out = eacp::Vector<std::string>();
    auto ec = std::error_code();
    auto dir = runningRoot(root);
    if (!fs::exists(dir, ec))
        return out;

    for (const auto& entry: fs::directory_iterator(dir, ec))
    {
        if (ec || !entry.is_regular_file())
            continue;

        auto name = entry.path().filename().string();
        constexpr auto suffix = std::string_view(".running");
        if (name.size() <= suffix.size()
            || name.compare(name.size() - suffix.size(),
                            suffix.size(),
                            suffix) != 0)
            continue;

        out.add(name.substr(0, name.size() - suffix.size()));
    }

    return out;
}

std::optional<CliOptions> parseArgs(int argc, char* argv[])
{
    auto options = CliOptions();

    for (auto i = 1; i < argc; ++i)
    {
        auto arg = std::string(argv[i]);
        if (arg == "--root")
        {
            if (i + 1 >= argc)
                return std::nullopt;
            options.root = argv[++i];
        }
        else if (arg == "--help" || arg == "-h")
        {
            options.command = "help";
        }
        else if (options.command == "tui")
        {
            options.command = arg;
        }
        else if (arg == "--manifest-url")
        {
            if (i + 1 >= argc)
                return std::nullopt;
            options.manifestUrl = argv[++i];
        }
        else if (options.productId.empty())
        {
            options.productId = arg;
        }
        else
        {
            return std::nullopt;
        }
    }

    return options;
}

bool runProcess(const std::vector<std::string>& args)
{
    if (args.empty())
        return false;

#if defined(_WIN32)
    return false;
#else
    auto argv = std::vector<char*>();
    argv.reserve(args.size() + 1);
    for (const auto& arg: args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    auto pid = pid_t {};
    auto status = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
    if (status != 0)
        return false;

    auto waitStatus = 0;
    if (waitpid(pid, &waitStatus, 0) < 0)
        return false;

    return WIFEXITED(waitStatus) && WEXITSTATUS(waitStatus) == 0;
#endif
}

bool runInstallProcess(std::vector<std::string> args)
{
#if defined(_WIN32)
    return false;
#else
    if (::access("/Applications", W_OK) == 0)
        return runProcess(args);

    args.insert(args.begin(), "sudo");
    return runProcess(args);
#endif
}

bool validBundleName(const std::string& bundleName)
{
    constexpr auto suffix = std::string_view(".app");
    if (bundleName.size() <= suffix.size()
        || bundleName.compare(bundleName.size() - suffix.size(),
                              suffix.size(),
                              suffix) != 0)
        return false;

    for (auto c: bundleName)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' '
            && c != '.' && c != '-' && c != '_')
            return false;
    }

    return true;
}

void printUsage()
{
    std::cout
        << "AppHub mock updater CLI\n\n"
        << "Usage:\n"
        << "  AppHub [--root <path>] tui\n"
        << "  AppHub [--root <path>] demo\n"
        << "  AppHub [--root <path>] reset\n"
        << "  AppHub [--root <path>] list\n"
        << "  AppHub [--root <path>] status\n"
        << "  AppHub [--root <path>] install <product-id>\n"
        << "  AppHub [--root <path>] open <product-id>\n"
        << "  AppHub [--root <path>] close <product-id>\n"
        << "  AppHub [--root <path>] publish-update\n"
        << "  AppHub [--root <path>] bless-helper\n"
        << "  AppHub [--root <path>] remote-install --manifest-url <url>\n"
        << "  AppHub [--root <path>] update\n"
        << "  AppHub [--root <path>] remove <product-id>\n\n"
        << "Products:\n"
        << "  " << editorId << "\n"
        << "  " << captureId << "\n"
        << "  " << runtimeId << "\n"
        << "  " << modelId << "\n";
}

void printStatus(const fs::path& root,
                 const Updater::ProductCatalog& catalog,
                 const eacp::Vector<Updater::ProductReceipt>& receipts)
{
    std::cout << "Root: " << root << "\n";
    std::cout << "Catalog version: " << catalog.catalogVersion << "\n";
    std::cout << "Products:\n";

    for (const auto& product: catalog.products)
    {
        auto* receipt = Updater::findReceipt(receipts, product.id);
        auto installed = receipt != nullptr;
        auto updateAvailable =
            installed && Updater::isNewerVersion(product.latestVersion,
                                                 receipt->version);

        std::cout << "  " << product.id << " | " << product.name
                  << " | latest " << product.latestVersion << " | ";

        if (!installed)
        {
            std::cout << "not installed\n";
        }
        else
        {
            std::cout << "installed " << receipt->version;
            if (updateAvailable)
                std::cout << " | update available";
            if (isRunning(root, product.id))
                std::cout << " | running";
            std::cout << "\n";
        }
    }
}

void printReceipts(const eacp::Vector<Updater::ProductReceipt>& receipts)
{
    std::cout << "Receipts:\n";
    if (receipts.empty())
    {
        std::cout << "  none\n";
        return;
    }

    for (const auto& receipt: receipts)
    {
        std::cout << "  " << receipt.productId << " " << receipt.version
                  << " -> " << receipt.installPath << "\n";
    }
}

int printResult(const std::string& label, const Updater::InstallResult& result)
{
    std::cout << label << ": " << (result.ok ? "ok" : result.error) << "\n";
    return result.ok ? 0 : 1;
}

int resetRoot(const fs::path& root)
{
    std::error_code ec;
    fs::remove_all(root, ec);
    if (ec)
    {
        std::cout << "Reset failed: " << ec.message() << "\n";
        return 1;
    }

    writeDevCatalog(root, false);
    auto helper = makeHelper(root);
    if (!helper.isInstalled())
    {
        std::cout << "Reset failed: helper root could not be created\n";
        return 1;
    }

    std::cout << "Reset mock AppHub root: " << root << "\n";
    return 0;
}

int installProduct(const fs::path& root, const std::string& productId)
{
    auto catalog = loadOrCreateCatalog(root);
    auto helper = makeHelper(root);
    auto plan = Updater::planInstallWithDependencies(catalog,
                                                     helper.receipts(),
                                                     productId,
                                                     makeTarget(),
                                                     stagingRoot(root).string());

    if (plan.operations.empty())
    {
        std::cout << "No install plan for product: " << productId << "\n";
        return 1;
    }

    return printResult("Install " + productId, helper.submit(plan));
}

int updateAll(const fs::path& root)
{
    auto catalog = loadOrCreateCatalog(root);
    auto helper = makeHelper(root);
    auto plan =
        Updater::planUpdateAll(catalog,
                               helper.receipts(),
                               makeTarget(),
                               stagingRoot(root).string());

    if (plan.operations.empty())
    {
        std::cout << "Update all: no updates available\n";
        return 0;
    }

    auto running = runningProducts(root);
    if (!running.empty())
    {
        std::cout << "Update all: waiting for apps to close";
        for (const auto& productId: running)
            std::cout << " " << productId;
        std::cout << "\n";
        return 0;
    }

    return printResult("Update all", helper.submit(plan));
}

int publishUpdate(const fs::path& root)
{
    writeDevCatalog(root, true);
    std::cout << "Published catalog version 2 with updates for "
              << modelId << " and " << editorId << "\n";
    return 0;
}

int blessHelper()
{
    auto result = AppHub::installPrivilegedHelper();
    if (!result.ok)
    {
        std::cout << "Bless helper: " << result.error << "\n";
        return 1;
    }

    std::cout << "Bless helper: ok\n";
    return 0;
}

int remoteInstall(const fs::path& root, const std::string& manifestUrl)
{
    if (manifestUrl.empty())
    {
        std::cout << "remote-install requires --manifest-url <url>\n";
        return 2;
    }

    auto downloads = remoteDownloadRoot(root);
    auto unpack = remoteUnpackRoot(root);
    auto manifestPath = downloads / "manifest.json";
    auto artifactPath = downloads / "artifact.app.zip";

    std::error_code ec;
    fs::create_directories(downloads, ec);
    if (ec)
    {
        std::cout << "Remote install: failed to create download directory\n";
        return 1;
    }

    std::cout << "Remote install: downloading manifest\n";
    if (!runProcess({"/usr/bin/curl",
                     "--fail",
                     "--location",
                     "--silent",
                     "--show-error",
                     manifestUrl,
                     "--output",
                     manifestPath.string()}))
    {
        std::cout << "Remote install: manifest download failed\n";
        return 1;
    }

    auto manifest = RemoteAppManifest();
    try
    {
        Miro::fromJSONString(manifest, readFile(manifestPath));
    }
    catch (...)
    {
        std::cout << "Remote install: invalid manifest\n";
        return 1;
    }

    if (!Updater::isValidProductId(manifest.productId)
        || !validBundleName(manifest.bundleName)
        || manifest.artifact.url.empty()
        || manifest.artifact.sha256.empty())
    {
        std::cout << "Remote install: manifest failed validation\n";
        return 1;
    }

    std::cout << "Remote install: downloading " << manifest.name << " "
              << manifest.version << "\n";
    if (!runProcess({"/usr/bin/curl",
                     "--fail",
                     "--location",
                     "--silent",
                     "--show-error",
                     manifest.artifact.url,
                     "--output",
                     artifactPath.string()}))
    {
        std::cout << "Remote install: artifact download failed\n";
        return 1;
    }

    auto actualHash = eacp::Crypto::sha256File(artifactPath.string());
    if (actualHash != manifest.artifact.sha256)
    {
        std::cout << "Remote install: artifact hash mismatch\n";
        return 1;
    }

    fs::remove_all(unpack, ec);
    fs::create_directories(unpack, ec);
    if (ec)
    {
        std::cout << "Remote install: failed to create unpack directory\n";
        return 1;
    }

    if (!runProcess({"/usr/bin/ditto",
                     "-x",
                     "-k",
                     artifactPath.string(),
                     unpack.string()}))
    {
        std::cout << "Remote install: failed to unpack artifact\n";
        return 1;
    }

    auto unpackedApp = unpack / manifest.bundleName;
    if (!fs::is_directory(unpackedApp, ec))
    {
        std::cout << "Remote install: artifact did not contain "
                  << manifest.bundleName << "\n";
        return 1;
    }

    auto installPath = fs::path("/Applications") / manifest.bundleName;
    auto rollbackPath = fs::path("/Applications") / (manifest.bundleName + ".rollback");

    std::cout << "Remote install: installing to " << installPath << "\n";
    if (!runInstallProcess({"/bin/rm", "-rf", rollbackPath.string()}))
    {
        std::cout << "Remote install: failed to remove old rollback\n";
        return 1;
    }

    if (fs::exists(installPath, ec)
        && !runInstallProcess({"/bin/mv",
                               installPath.string(),
                               rollbackPath.string()}))
    {
        std::cout << "Remote install: failed to create rollback\n";
        return 1;
    }

    if (!runInstallProcess({"/usr/bin/ditto",
                            unpackedApp.string(),
                            installPath.string()}))
    {
        std::cout << "Remote install: failed to install app\n";
        return 1;
    }

    auto executable =
        installPath / "Contents" / "MacOS" / manifest.name;
    std::cout << "Remote install: installed " << manifest.name << " "
              << manifest.version << "\n";
    if (fs::exists(executable, ec))
        runProcess({executable.string(), "--version"});

    return 0;
}

int openProduct(const fs::path& root, const std::string& productId)
{
    auto helper = makeHelper(root);
    auto* receipt = Updater::findReceipt(helper.receipts(), productId);
    if (receipt == nullptr)
    {
        std::cout << "Open " << productId << ": not installed\n";
        return 1;
    }

    writeFile(runningPath(root, productId), "running");
    std::cout << "Open " << productId << ": ok\n";
    return 0;
}

int closeProduct(const fs::path& root, const std::string& productId)
{
    auto ec = std::error_code();
    fs::remove(runningPath(root, productId), ec);
    if (ec)
    {
        std::cout << "Close " << productId << ": " << ec.message() << "\n";
        return 1;
    }

    std::cout << "Close " << productId << ": ok\n";
    return 0;
}

int removeProduct(const fs::path& root, const std::string& productId)
{
    auto helper = makeHelper(root);
    return printResult("Remove " + productId,
                       helper.submit(Updater::planRemove(productId)));
}

int runDemo(const fs::path& root)
{
    auto status = resetRoot(root);
    if (status != 0)
        return status;

    status = installProduct(root, stringFrom(editorId));
    if (status != 0)
        return status;

    status = installProduct(root, stringFrom(captureId));
    if (status != 0)
        return status;

    status = publishUpdate(root);
    if (status != 0)
        return status;

    status = updateAll(root);
    if (status != 0)
        return status;

    status = removeProduct(root, stringFrom(captureId));
    if (status != 0)
        return status;

    auto helper = makeHelper(root);
    printReceipts(helper.receipts());
    std::cout << "Demo complete. All writes were constrained to the mock helper root.\n";
    return 0;
}

int showList(const fs::path& root)
{
    auto catalog = loadOrCreateCatalog(root);
    auto helper = makeHelper(root);
    printStatus(root, catalog, helper.receipts());
    return 0;
}

int showReceipts(const fs::path& root)
{
    auto helper = makeHelper(root);
    std::cout << "Root: " << root << "\n";
    printReceipts(helper.receipts());
    return 0;
}

int runTui(const fs::path& root)
{
    loadOrCreateCatalog(root);

    while (true)
    {
        std::cout << "\nAppHub mock updater\n"
                  << "Root: " << root << "\n"
                  << "1. List products\n"
                  << "2. Install Example Editor\n"
                  << "3. Install Example Capture\n"
                  << "4. Update all\n"
                  << "5. Open Example Editor\n"
                  << "6. Close Example Editor\n"
                  << "7. Publish update\n"
                  << "8. Remove Example Editor\n"
                  << "9. Remove Example Capture\n"
                  << "10. Show receipts\n"
                  << "11. Reset\n"
                  << "12. Quit\n"
                  << "> ";

        auto choice = std::string();
        if (!std::getline(std::cin, choice))
            return 0;

        if (choice == "1")
            showList(root);
        else if (choice == "2")
            installProduct(root, stringFrom(editorId));
        else if (choice == "3")
            installProduct(root, stringFrom(captureId));
        else if (choice == "4")
            updateAll(root);
        else if (choice == "5")
            openProduct(root, stringFrom(editorId));
        else if (choice == "6")
            closeProduct(root, stringFrom(editorId));
        else if (choice == "7")
            publishUpdate(root);
        else if (choice == "8")
            removeProduct(root, stringFrom(editorId));
        else if (choice == "9")
            removeProduct(root, stringFrom(captureId));
        else if (choice == "10")
            showReceipts(root);
        else if (choice == "11")
            resetRoot(root);
        else if (choice == "12" || choice == "q" || choice == "quit")
            return 0;
        else
            std::cout << "Unknown choice\n";
    }
}
} // namespace

int main(int argc, char* argv[])
{
    auto parsed = parseArgs(argc, argv);
    if (!parsed)
    {
        printUsage();
        return 2;
    }

    const auto& options = *parsed;
    const auto& command = options.command;

    if (command == "help")
    {
        printUsage();
        return 0;
    }
    if (command == "tui")
        return runTui(options.root);
    if (command == "demo")
        return runDemo(options.root);
    if (command == "reset")
        return resetRoot(options.root);
    if (command == "list")
        return showList(options.root);
    if (command == "status")
        return showReceipts(options.root);
    if (command == "install")
    {
        if (options.productId.empty())
        {
            std::cout << "install requires a product id\n";
            return 2;
        }
        return installProduct(options.root, options.productId);
    }
    if (command == "open")
    {
        if (options.productId.empty())
        {
            std::cout << "open requires a product id\n";
            return 2;
        }
        return openProduct(options.root, options.productId);
    }
    if (command == "close")
    {
        if (options.productId.empty())
        {
            std::cout << "close requires a product id\n";
            return 2;
        }
        return closeProduct(options.root, options.productId);
    }
    if (command == "publish-update")
        return publishUpdate(options.root);
    if (command == "bless-helper")
        return blessHelper();
    if (command == "remote-install")
        return remoteInstall(options.root, options.manifestUrl);
    if (command == "update")
        return updateAll(options.root);
    if (command == "remove")
    {
        if (options.productId.empty())
        {
            std::cout << "remove requires a product id\n";
            return 2;
        }
        return removeProduct(options.root, options.productId);
    }

    printUsage();
    return 2;
}
