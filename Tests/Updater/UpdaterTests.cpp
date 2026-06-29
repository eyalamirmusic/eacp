#include <eacp/Updater/Updater.h>

#include <eacp/Core/Utils/SHA256.h>

#include <Miro/Miro.h>
#include <NanoTest/NanoTest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace nano;
namespace fs = std::filesystem;
namespace Updater = eacp::Updater;
using Platform = Updater::Platform;
using Architecture = Updater::Architecture;

enum class TestPlatform
{
    Any,
    Workstation,
    Cluster
};

enum class TestArchitecture
{
    Any,
    X64,
    NeuralEngine,
    Universal
};

namespace eacp::Updater
{
template <>
struct PlatformTraits<TestPlatform>
{
    static int specificity(TestPlatform artifact, TestPlatform target)
    {
        if (artifact == TestPlatform::Any)
            return 0;
        if (target == TestPlatform::Any)
            return 0;
        return artifact == target ? 2 : -1;
    }
};

template <>
struct ArchitectureTraits<TestArchitecture>
{
    static int specificity(TestArchitecture artifact, TestArchitecture target)
    {
        if (artifact == TestArchitecture::Any)
            return 0;
        if (artifact == TestArchitecture::Universal)
            return 1;
        if (target == TestArchitecture::Any)
            return 0;
        return artifact == target ? 2 : -1;
    }
};
} // namespace eacp::Updater

namespace
{
fs::path testRoot(const std::string& name)
{
    auto root = fs::temp_directory_path() / ("eacp-updater-tests-" + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

void writeFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
    out << text;
}

Updater::ProductCatalog makeEmptyCatalog(int version = 1)
{
    auto catalog = Updater::ProductCatalog();
    catalog.catalogVersion = version;
    catalog.signature = "dev-signature";
    return catalog;
}

Updater::Product makeProduct(const std::string& id,
                             const std::string& name,
                             Updater::PackageKind kind,
                             const std::string& version)
{
    auto product = Updater::Product();
    product.id = id;
    product.name = name;
    product.kind = kind;
    product.channel = "stable";
    product.latestVersion = version;
    return product;
}

Updater::ProductArtifact makeArtifact(Platform platform,
                                      Architecture architecture,
                                      const std::string& url,
                                      const std::string& sha256)
{
    auto artifact = Updater::ProductArtifact();
    artifact.platform = platform;
    artifact.architecture = architecture;
    artifact.url = url;
    artifact.sha256 = sha256;
    artifact.signature = "dev-artifact";
    return artifact;
}

Updater::Target makeTarget(Platform platform, Architecture architecture)
{
    auto target = Updater::Target();
    target.platform = platform;
    target.architecture = architecture;
    return target;
}

Updater::ProductReceipt makeReceipt(const std::string& productId,
                                    const std::string& version)
{
    auto receipt = Updater::ProductReceipt();
    receipt.productId = productId;
    receipt.version = version;
    return receipt;
}

Updater::PlanOperation makeOperation(Updater::PlanAction action,
                                     const std::string& productId,
                                     const std::string& name = {},
                                     const std::string& version = {},
                                     const std::string& artifactPath = {},
                                     const std::string& artifactSha256 = {})
{
    auto operation = Updater::PlanOperation();
    operation.action = action;
    operation.productId = productId;
    operation.name = name;
    operation.channel = "stable";
    operation.version = version;
    operation.artifactPath = artifactPath;
    operation.artifactSha256 = artifactSha256;
    return operation;
}

Updater::MockHelperOptions makeHelperOptions(const fs::path& root,
                                             const fs::path& staging)
{
    auto options = Updater::MockHelperOptions();
    options.root = root.string();
    options.stagingRoot = staging.string();
    return options;
}

Updater::ProductCatalog makeCatalog(const std::string& editorHash,
                                    const std::string& captureHash = {})
{
    auto catalog = makeEmptyCatalog();

    auto editor = makeProduct("tamber.editor",
                              "Example Editor",
                              Updater::PackageKind::App,
                              "1.0.0");
    editor.artifacts.add(makeArtifact(Platform::MacOS,
                                      Architecture::Any,
                                      "file://editor",
                                      editorHash));
    catalog.products.add(editor);

    if (!captureHash.empty())
    {
        auto capture = makeProduct("tamber.capture",
                                   "Example Capture",
                                   Updater::PackageKind::App,
                                   "1.0.0");
        capture.artifacts.add(makeArtifact(Platform::MacOS,
                                           Architecture::Any,
                                           "file://capture",
                                           captureHash));
        catalog.products.add(capture);
    }

    return catalog;
}
} // namespace

auto tCatalogUsesMiroJson = test("Updater/catalogRoundTripsThroughMiroJson") = []
{
    auto catalog = makeCatalog("hash");
    auto json = Updater::catalogToJson(catalog);

    check(json.find("tamber.editor") != std::string::npos);

    auto parsed = Updater::parseCatalogJson(json);
    check(parsed.catalogVersion == 1);
    check(parsed.products.size() == 1);
    check(parsed.products[0].id == "tamber.editor");
    check(parsed.products[0].kind == Updater::PackageKind::App);
    check(parsed.products[0].artifacts[0].sha256 == "hash");
};

auto tArtifactForTargetPrefersExactVariant =
    test("Updater/artifactForTargetPrefersExactVariant") = []
{
    auto product = makeProduct("shared.onnxruntime",
                               "ONNX Runtime",
                               Updater::PackageKind::Runtime,
                               "1.0.0");
    product.artifacts.add(
        makeArtifact(Platform::MacOS,
                     Architecture::Universal,
                     "universal",
                     "universal-hash"));
    product.artifacts.add(makeArtifact(Platform::MacOS,
                                       Architecture::Arm64,
                                       "arm64",
                                       "arm64-hash"));
    product.artifacts.add(
        makeArtifact(Platform::Any, Architecture::Any, "any", "any-hash"));

    auto exact =
        Updater::artifactForTarget(product,
                                   makeTarget(Platform::MacOS,
                                              Architecture::Arm64));
    auto universal =
        Updater::artifactForTarget(product,
                                   makeTarget(Platform::MacOS,
                                              Architecture::X64));
    auto fallback =
        Updater::artifactForTarget(product,
                                   makeTarget(Platform::Linux,
                                              Architecture::X64));

    check(exact.url == "arm64");
    check(universal.url == "universal");
    check(fallback.url == "any");
};

auto tTemplatedTargetDomainSupportsHubEnums =
    test("Updater/templatedTargetDomainSupportsHubEnums") = []
{
    using HubCatalog =
        Updater::ProductCatalogT<TestPlatform, TestArchitecture>;
    using HubProduct = Updater::ProductT<TestPlatform, TestArchitecture>;
    using HubArtifact =
        Updater::ProductArtifactT<TestPlatform, TestArchitecture>;
    using HubTarget = Updater::TargetT<TestPlatform, TestArchitecture>;

    auto product = HubProduct();
    product.id = "tamber.neural-editor";
    product.name = "Neural Editor";
    product.kind = Updater::PackageKind::App;
    product.channel = "stable";
    product.latestVersion = "1.0.0";

    auto fallback = HubArtifact();
    fallback.platform = TestPlatform::Any;
    fallback.architecture = TestArchitecture::Any;
    fallback.url = "fallback";
    fallback.sha256 = "fallback-hash";
    fallback.signature = "test";
    product.artifacts.add(fallback);

    auto universal = HubArtifact();
    universal.platform = TestPlatform::Workstation;
    universal.architecture = TestArchitecture::Universal;
    universal.url = "universal";
    universal.sha256 = "universal-hash";
    universal.signature = "test";
    product.artifacts.add(universal);

    auto exact = HubArtifact();
    exact.platform = TestPlatform::Workstation;
    exact.architecture = TestArchitecture::NeuralEngine;
    exact.url = "neural-engine";
    exact.sha256 = "neural-hash";
    exact.signature = "test";
    product.artifacts.add(exact);

    auto catalog = HubCatalog();
    catalog.catalogVersion = 1;
    catalog.signature = "hub-signature";
    catalog.products.add(product);

    auto json = Miro::toJSONString(catalog);
    auto parsed = HubCatalog();
    Miro::fromJSONString(parsed, json);

    auto target = HubTarget();
    target.platform = TestPlatform::Workstation;
    target.architecture = TestArchitecture::NeuralEngine;

    auto selected = Updater::artifactForTargetT(parsed.products[0], target);
    check(selected.url == "neural-engine");
};

auto tPlanInstallWithDependenciesInstallsSharedPackagesFirst =
    test("Updater/planInstallWithDependenciesInstallsSharedPackagesFirst") = []
{
    auto root = testRoot("dependency-plan");
    auto staging = root / "staging";
    writeFile(staging / "shared.clap.artifact", "clap");
    writeFile(staging / "tamber.editor.artifact", "editor");

    auto catalog = makeEmptyCatalog();

    auto shared = makeProduct("shared.clap",
                              "CLAP Model",
                              Updater::PackageKind::Model,
                              "1.0.0");
    shared.artifacts.add(makeArtifact(Platform::Any,
                                      Architecture::Any,
                                      "file://shared.clap",
                                      eacp::Crypto::sha256File(
                                          (staging / "shared.clap.artifact")
                                              .string())));

    auto app = makeProduct("tamber.editor",
                           "Example Editor",
                           Updater::PackageKind::App,
                           "1.0.0");
    app.dependencies.add("shared.clap");
    app.artifacts.add(makeArtifact(Platform::MacOS,
                                   Architecture::Arm64,
                                   "file://tamber.editor",
                                   eacp::Crypto::sha256File(
                                       (staging / "tamber.editor.artifact")
                                           .string())));

    catalog.products.add(shared);
    catalog.products.add(app);

    auto plan = Updater::planInstallWithDependencies(catalog,
                                                     {},
                                                     "tamber.editor",
                                                     makeTarget(
                                                         Platform::MacOS,
                                                         Architecture::Arm64),
                                                     staging.string());

    check(plan.operations.size() == 2);
    check(plan.operations[0].productId == "shared.clap");
    check(plan.operations[0].action == Updater::PlanAction::Install);
    check(plan.operations[1].productId == "tamber.editor");
    check(plan.operations[1].action == Updater::PlanAction::Install);
};

auto tPlanUpdateAllUpdatesSharedPackageIndependently =
    test("Updater/planUpdateAllUpdatesSharedPackageIndependently") = []
{
    auto root = testRoot("shared-update");
    auto staging = root / "staging";
    writeFile(staging / "shared.clap.artifact", "clap-v2");

    auto catalog = makeEmptyCatalog(2);
    auto shared = makeProduct("shared.clap",
                              "CLAP Model",
                              Updater::PackageKind::Model,
                              "2.0.0");
    shared.artifacts.add(makeArtifact(Platform::Any,
                                      Architecture::Any,
                                      "file://shared.clap",
                                      eacp::Crypto::sha256File(
                                          (staging / "shared.clap.artifact")
                                              .string())));
    catalog.products.add(shared);

    auto receipts = eacp::Vector<Updater::ProductReceipt>();
    receipts.add(makeReceipt("shared.clap", "1.0.0"));

    auto plan = Updater::planUpdateAll(catalog,
                                       receipts,
                                       makeTarget(Platform::MacOS,
                                                  Architecture::Arm64),
                                       staging.string());

    check(plan.operations.size() == 1);
    check(plan.operations[0].productId == "shared.clap");
    check(plan.operations[0].action == Updater::PlanAction::Update);
};

auto tInstallPlanUsesEnumClassWithMiroJson =
    test("Updater/installPlanRoundTripsEnumClassThroughMiroJson") = []
{
    auto plan = Updater::InstallPlan();
    plan.operations.add(makeOperation(Updater::PlanAction::Update,
                                      "tamber.editor",
                                      {},
                                      "2.0.0"));

    auto json = Updater::installPlanToJson(plan);
    check(json.find("\"Update\"") != std::string::npos);

    auto parsed = Updater::parseInstallPlanJson(json);
    check(parsed.operations.size() == 1);
    check(parsed.operations[0].action == Updater::PlanAction::Update);
};

auto tProductIdValidation = test("Updater/productIdValidation") = []
{
    check(Updater::isValidProductId("tamber.editor"));
    check(Updater::isValidProductId("com.example-App_1"));
    check(!Updater::isValidProductId(""));
    check(!Updater::isValidProductId("."));
    check(!Updater::isValidProductId(".."));
    check(!Updater::isValidProductId("../evil"));
    check(!Updater::isValidProductId("evil/path"));
    check(!Updater::isValidProductId("evil path"));
};

auto tPlanRemoveRejectsInvalidProductId =
    test("Updater/planRemoveRejectsInvalidProductId") = []
{
    check(Updater::planRemove("../evil").operations.empty());
    check(Updater::planRemove("").operations.empty());
};

auto tMockHelperInstallsAndWritesReceipt =
    test("Updater/mockHelperInstallsAndWritesReceipt") = []
{
    auto root = testRoot("install");
    auto staging = root / "staging";
    auto artifact = staging / "tamber.editor.artifact";
    writeFile(artifact, "editor-v1");

    auto hash = eacp::Crypto::sha256File(artifact.string());
    auto catalog = makeCatalog(hash);
    auto helper = Updater::MockPrivilegedHelper(makeHelperOptions(root, staging));

    auto plan = Updater::planInstall(
        catalog, {}, "tamber.editor", Platform::MacOS, artifact.string());

    auto result = helper.submit(plan);
    check(result.ok);

    auto receipts = helper.receipts();
    check(receipts.size() == 1);
    check(receipts[0].productId == "tamber.editor");
    check(receipts[0].version == "1.0.0");
    check(fs::exists(root / "Applications" / "tamber.editor" / "artifact.bin"));
};

auto tMockHelperRejectsForgedUnsafeProductId =
    test("Updater/mockHelperRejectsForgedUnsafeProductId") = []
{
    auto root = testRoot("unsafe-product-id");
    auto staging = root / "staging";
    auto artifact = staging / "evil.artifact";
    writeFile(artifact, "evil");

    auto plan = Updater::InstallPlan();
    plan.operations.add(makeOperation(Updater::PlanAction::Install,
                                      "../evil",
                                      "Evil",
                                      "1.0.0",
                                      artifact.string(),
                                      eacp::Crypto::sha256File(
                                          artifact.string())));

    auto helper = Updater::MockPrivilegedHelper(makeHelperOptions(root, staging));
    auto result = helper.submit(plan);

    check(!result.ok);
    check(result.error.find("product id") != std::string::npos);
    check(helper.receipts().empty());
    check(!fs::exists(root / "Applications" / "evil"));
};

auto tMockHelperRejectsDuplicateOperationsBeforeMutation =
    test("Updater/mockHelperRejectsDuplicateOperationsBeforeMutation") = []
{
    auto root = testRoot("duplicate-op");
    auto staging = root / "staging";
    auto artifact = staging / "tamber.editor.artifact";
    writeFile(artifact, "editor-v1");

    auto hash = eacp::Crypto::sha256File(artifact.string());
    auto plan = Updater::InstallPlan();
    plan.operations.add(makeOperation(Updater::PlanAction::Install,
                                      "tamber.editor",
                                      "Example Editor",
                                      "1.0.0",
                                      artifact.string(),
                                      hash));
    plan.operations.add(
        makeOperation(Updater::PlanAction::Remove, "tamber.editor"));

    auto helper = Updater::MockPrivilegedHelper(makeHelperOptions(root, staging));
    auto result = helper.submit(plan);

    check(!result.ok);
    check(result.error.find("duplicate") != std::string::npos);
    check(helper.receipts().empty());
    check(!fs::exists(root / "Applications" / "tamber.editor"));
};

auto tMockHelperRejectsHashMismatch =
    test("Updater/mockHelperRejectsHashMismatch") = []
{
    auto root = testRoot("hash-mismatch");
    auto staging = root / "staging";
    auto artifact = staging / "tamber.editor.artifact";
    writeFile(artifact, "editor-v1");

    auto catalog = makeCatalog("not-the-real-hash");
    auto helper = Updater::MockPrivilegedHelper(makeHelperOptions(root, staging));

    auto plan = Updater::planInstall(
        catalog, {}, "tamber.editor", Platform::MacOS, artifact.string());

    auto result = helper.submit(plan);
    check(!result.ok);
    check(result.error.find("hash") != std::string::npos);
};

auto tMockHelperValidatesWholePlanBeforeMutation =
    test("Updater/mockHelperValidatesWholePlanBeforeMutation") = []
{
    auto root = testRoot("validate-before-mutate");
    auto staging = root / "staging";
    auto editorArtifact = staging / "tamber.editor.artifact";
    auto captureArtifact = staging / "tamber.capture.artifact";
    writeFile(editorArtifact, "editor-v1");
    writeFile(captureArtifact, "capture-v1");

    auto plan = Updater::InstallPlan();
    plan.operations.add(makeOperation(Updater::PlanAction::Install,
                                      "tamber.editor",
                                      "Example Editor",
                                      "1.0.0",
                                      editorArtifact.string(),
                                      eacp::Crypto::sha256File(
                                          editorArtifact.string())));
    plan.operations.add(makeOperation(Updater::PlanAction::Install,
                                      "tamber.capture",
                                      "Example Capture",
                                      "1.0.0",
                                      captureArtifact.string(),
                                      "bad-hash"));

    auto helper = Updater::MockPrivilegedHelper(makeHelperOptions(root, staging));
    auto result = helper.submit(plan);

    check(!result.ok);
    check(result.error.find("hash") != std::string::npos);
    check(helper.receipts().empty());
    check(!fs::exists(root / "Applications" / "tamber.editor"));
    check(!fs::exists(root / "Applications" / "tamber.capture"));
};

auto tMockHelperRejectsStagingEscape =
    test("Updater/mockHelperRejectsArtifactOutsideStaging") = []
{
    auto root = testRoot("staging-escape");
    auto staging = root / "staging";
    auto outside = root / "outside.artifact";
    writeFile(outside, "editor-v1");

    auto hash = eacp::Crypto::sha256File(outside.string());
    auto catalog = makeCatalog(hash);
    auto helper = Updater::MockPrivilegedHelper(makeHelperOptions(root, staging));

    auto plan = Updater::planInstall(
        catalog, {}, "tamber.editor", Platform::MacOS, outside.string());

    auto result = helper.submit(plan);
    check(!result.ok);
    check(result.error.find("staging") != std::string::npos);
};

auto tMockHelperRejectsDowngrade = test("Updater/mockHelperRejectsDowngrade") = []
{
    auto root = testRoot("downgrade");
    auto staging = root / "staging";
    auto artifact = staging / "tamber.editor.artifact";
    writeFile(artifact, "editor-v2");

    auto hash = eacp::Crypto::sha256File(artifact.string());
    auto catalog = makeCatalog(hash);
    catalog.products[0].latestVersion = "2.0.0";

    auto helper = Updater::MockPrivilegedHelper(makeHelperOptions(root, staging));

    auto installPlan = Updater::planInstall(
        catalog, {}, "tamber.editor", Platform::MacOS, artifact.string());
    check(helper.submit(installPlan).ok);

    writeFile(artifact, "editor-v1");
    hash = eacp::Crypto::sha256File(artifact.string());
    catalog.products[0].latestVersion = "1.0.0";
    catalog.products[0].artifacts[0].sha256 = hash;

    auto downgradePlan = Updater::planInstall(catalog,
                                             helper.receipts(),
                                             "tamber.editor",
                                             Platform::MacOS,
                                             artifact.string());
    auto result = helper.submit(downgradePlan);

    check(!result.ok);
    check(result.error.find("downgrade") != std::string::npos);
};

auto tPlanUpdateAllFindsNewerInstalledProduct =
    test("Updater/planUpdateAllFindsNewerInstalledProduct") = []
{
    auto root = testRoot("update-plan");
    auto staging = root / "staging";
    auto artifact = staging / "tamber.editor.artifact";
    writeFile(artifact, "editor-v2");

    auto hash = eacp::Crypto::sha256File(artifact.string());
    auto catalog = makeCatalog(hash);
    catalog.products[0].latestVersion = "2.0.0";

    auto receipts = eacp::Vector<Updater::ProductReceipt>();
    receipts.add(makeReceipt("tamber.editor", "1.0.0"));

    auto plan = Updater::planUpdateAll(
        catalog, receipts, Platform::MacOS, staging.string());

    check(plan.operations.size() == 1);
    check(plan.operations[0].action == Updater::PlanAction::Update);
    check(plan.operations[0].artifactPath == artifact.string());
};

auto tMockHelperRemovesReceiptAndInstall =
    test("Updater/mockHelperRemovesReceiptAndInstall") = []
{
    auto root = testRoot("remove");
    auto staging = root / "staging";
    auto artifact = staging / "tamber.editor.artifact";
    writeFile(artifact, "editor-v1");

    auto hash = eacp::Crypto::sha256File(artifact.string());
    auto catalog = makeCatalog(hash);
    auto helper = Updater::MockPrivilegedHelper(makeHelperOptions(root, staging));

    auto installPlan = Updater::planInstall(
        catalog, {}, "tamber.editor", Platform::MacOS, artifact.string());
    check(helper.submit(installPlan).ok);

    auto removePlan = Updater::planRemove("tamber.editor");
    check(helper.submit(removePlan).ok);
    check(helper.receipts().empty());
    check(!fs::exists(root / "Applications" / "tamber.editor"));
};
