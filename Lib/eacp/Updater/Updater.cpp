#include "Updater.h"

#include <eacp/Core/Utils/SHA256.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace eacp::Updater
{
namespace
{
namespace fs = std::filesystem;

std::string readTextFile(const fs::path& path)
{
    auto in = std::ifstream(path, std::ios::binary);
    if (!in)
        return {};

    auto out = std::ostringstream();
    out << in.rdbuf();
    return out.str();
}

bool writeTextFile(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
        return false;

    auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    out << text;
    return (bool) out;
}

fs::path canonicalExisting(const fs::path& path)
{
    std::error_code ec;
    auto result = fs::weakly_canonical(path, ec);
    return ec ? fs::absolute(path) : result;
}

std::string productDirectoryName(const std::string& id)
{
    return id;
}

std::string nowUtcForReceipt()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto tm = std::tm {};

#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif

    auto out = std::ostringstream();
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

Vector<int> versionParts(const std::string& version)
{
    auto result = Vector<int>();
    auto current = std::string();

    auto flush = [&]
    {
        if (current.empty())
        {
            result.add(0);
            return;
        }

        try
        {
            result.add(std::stoi(current));
        }
        catch (...)
        {
            result.add(0);
        }
        current.clear();
    };

    for (auto c: version)
    {
        if (std::isdigit(static_cast<unsigned char>(c)))
            current.push_back(c);
        else
            flush();
    }

    flush();
    return result;
}

std::string artifactPathFor(const std::string& directory,
                            const std::string& productId)
{
    return (fs::path(directory) / (productDirectoryName(productId) + ".artifact"))
        .string();
}

fs::path applicationPathFor(const fs::path& root, const std::string& productId)
{
    return root / "Applications" / productDirectoryName(productId);
}

fs::path receiptPathFor(const fs::path& root, const std::string& productId)
{
    return root / "receipts" / (productDirectoryName(productId) + ".json");
}

fs::path installStagingPathFor(const fs::path& root, const std::string& productId)
{
    return root / "install-staging" / productDirectoryName(productId);
}

fs::path rollbackPathFor(const fs::path& root, const std::string& productId)
{
    return root / "rollback" / productDirectoryName(productId);
}

bool atomicWriteTextFile(const fs::path& path, const std::string& text)
{
    auto temp = path;
    temp += ".tmp";

    if (!writeTextFile(temp, text))
        return false;

    std::error_code ec;
    fs::rename(temp, path, ec);
    if (!ec)
        return true;

    fs::remove(path, ec);
    ec.clear();
    fs::rename(temp, path, ec);
    return !ec;
}

struct PreparedOperation
{
    PlanOperation request;
    fs::path productDir;
    fs::path receiptPath;
    fs::path installStagingDir;
    fs::path rollbackDir;
    ProductReceipt receipt;
};

InstallResult ok()
{
    auto result = InstallResult();
    result.ok = true;
    return result;
}

InstallResult error(std::string message)
{
    auto result = InstallResult();
    result.ok = false;
    result.error = std::move(message);
    return result;
}

bool hasDuplicateProductOperation(const InstallPlan& plan,
                                  const std::string& productId)
{
    auto count = 0;
    for (const auto& op: plan.operations)
        if (op.productId == productId)
            ++count;

    return count > 1;
}

Target makeTarget(Platform platform, Architecture architecture)
{
    auto target = Target();
    target.platform = platform;
    target.architecture = architecture;
    return target;
}

PlanOperation makeOperation(PlanAction action,
                            const std::string& productId,
                            const std::string& name = {},
                            const std::string& channel = {},
                            const std::string& version = {},
                            const std::string& artifactPath = {},
                            const std::string& artifactSha256 = {})
{
    auto operation = PlanOperation();
    operation.action = action;
    operation.productId = productId;
    operation.name = name;
    operation.channel = channel;
    operation.version = version;
    operation.artifactPath = artifactPath;
    operation.artifactSha256 = artifactSha256;
    return operation;
}

ProductReceipt makeReceipt(const PlanOperation& op, const fs::path& productDir)
{
    auto receipt = ProductReceipt();
    receipt.productId = op.productId;
    receipt.name = op.name;
    receipt.version = op.version;
    receipt.installPath = productDir.string();
    receipt.channel = op.channel;
    receipt.artifactSha256 = op.artifactSha256;
    receipt.installedAt = nowUtcForReceipt();
    return receipt;
}

InstallResult prepareOperation(const MockHelperOptions& options,
                               const InstallPlan& fullPlan,
                               const Vector<ProductReceipt>& receipts,
                               const PlanOperation& op,
                               PreparedOperation& out)
{
    if (!isValidProductId(op.productId))
        return error("invalid product id");

    if (hasDuplicateProductOperation(fullPlan, op.productId))
        return error("duplicate product operation");

    out.request = op;
    out.productDir = applicationPathFor(options.root, op.productId);
    out.receiptPath = receiptPathFor(options.root, op.productId);
    out.installStagingDir = installStagingPathFor(options.root, op.productId);
    out.rollbackDir = rollbackPathFor(options.root, op.productId);

    switch (op.action)
    {
        case PlanAction::Remove:
            return ok();

        case PlanAction::Install:
        case PlanAction::Update:
            break;

        default:
            return error("invalid plan action");
    }

    if (op.name.empty())
        return error("product name is required");
    if (op.version.empty())
        return error("product version is required");
    if (op.artifactSha256.empty())
        return error("artifact hash is required");

    if (!pathIsUnder(op.artifactPath, options.stagingRoot))
        return error("artifact path is outside staging root");

    auto actualHash = Crypto::sha256File(op.artifactPath);
    if (actualHash.empty())
        return error("artifact could not be read");
    if (actualHash != op.artifactSha256)
        return error("artifact hash mismatch");

    if (auto* existing = findReceipt(receipts, op.productId);
        existing != nullptr && !options.allowDowngrade
        && compareVersions(op.version, existing->version) < 0)
        return error("downgrade rejected");

    out.receipt = makeReceipt(op, out.productDir);

    return ok();
}

InstallResult preparePlan(const MockHelperOptions& options,
                          const Vector<ProductReceipt>& receipts,
                          const InstallPlan& plan,
                          Vector<PreparedOperation>& out)
{
    for (const auto& op: plan.operations)
    {
        auto prepared = PreparedOperation();
        auto result = prepareOperation(options, plan, receipts, op, prepared);
        if (!result.ok)
            return result;

        out.add(std::move(prepared));
    }

    return ok();
}

InstallResult executeRemove(const PreparedOperation& op)
{
    auto ec = std::error_code{};
    fs::remove_all(op.productDir, ec);
    if (ec)
        return error("failed to remove product");

    fs::remove(op.receiptPath, ec);
    if (ec)
        return error("failed to remove receipt");

    return ok();
}

InstallResult executeInstall(const PreparedOperation& op)
{
    auto ec = std::error_code{};
    fs::remove_all(op.installStagingDir, ec);
    fs::create_directories(op.installStagingDir, ec);
    if (ec)
        return error("failed to create install staging");

    fs::copy_file(op.request.artifactPath,
                  op.installStagingDir / "artifact.bin",
                  fs::copy_options::overwrite_existing,
                  ec);
    if (ec)
        return error("failed to stage artifact");

    fs::remove_all(op.rollbackDir, ec);
    if (fs::exists(op.productDir, ec))
    {
        fs::create_directories(op.rollbackDir.parent_path(), ec);
        fs::rename(op.productDir, op.rollbackDir, ec);
        if (ec)
            return error("failed to create rollback copy");
    }

    fs::create_directories(op.productDir.parent_path(), ec);
    fs::rename(op.installStagingDir, op.productDir, ec);
    if (ec)
    {
        auto restoreEc = std::error_code();
        if (fs::exists(op.rollbackDir, restoreEc))
            fs::rename(op.rollbackDir, op.productDir, restoreEc);
        return error("failed to publish product install");
    }

    if (!atomicWriteTextFile(op.receiptPath, receiptToJson(op.receipt)))
        return error("failed to write receipt");

    return ok();
}
} // namespace

ProductCatalog parseCatalogJson(const std::string& json)
{
    auto catalog = ProductCatalog();
    Miro::fromJSONString(catalog, json);
    return catalog;
}

// @claude this is a nonsense function. delete it.
std::string catalogToJson(const ProductCatalog& catalog)
{
    return Miro::toJSONString(catalog);
}

ProductReceipt parseReceiptJson(const std::string& json)
{
    auto receipt = ProductReceipt();
    Miro::fromJSONString(receipt, json);
    return receipt;
}

std::string receiptToJson(const ProductReceipt& receipt)
{
    return Miro::toJSONString(receipt);
}

InstallPlan parseInstallPlanJson(const std::string& json)
{
    auto plan = InstallPlan();
    Miro::fromJSONString(plan, json);
    return plan;
}

std::string installPlanToJson(const InstallPlan& plan)
{
    return Miro::toJSONString(plan);
}

const Product* findProduct(const ProductCatalog& catalog,
                           const std::string& productId)
{
    for (const auto& product: catalog.products)
        if (product.id == productId)
            return &product;

    return nullptr;
}

const ProductReceipt* findReceipt(const Vector<ProductReceipt>& receipts,
                                  const std::string& productId)
{
    for (const auto& receipt: receipts)
        if (receipt.productId == productId)
            return &receipt;

    return nullptr;
}

int compareVersions(const std::string& lhs, const std::string& rhs)
{
    auto left = versionParts(lhs);
    auto right = versionParts(rhs);
    auto count = std::max(static_cast<std::size_t>(left.size()),
                          static_cast<std::size_t>(right.size()));

    for (std::size_t i = 0; i < count; ++i)
    {
        auto l = i < static_cast<std::size_t>(left.size()) ? left[i] : 0;
        auto r = i < static_cast<std::size_t>(right.size()) ? right[i] : 0;
        if (l < r)
            return -1;
        if (l > r)
            return 1;
    }

    return 0;
}

bool isNewerVersion(const std::string& candidate, const std::string& current)
{
    return compareVersions(candidate, current) > 0;
}

bool isValidProductId(const std::string& productId)
{
    if (productId.empty() || productId == "." || productId == "..")
        return false;

    for (auto c: productId)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.'
            && c != '-' && c != '_')
            return false;
    }

    return true;
}

ProductArtifact artifactForPlatform(const Product& product, Platform platform)
{
    return artifactForTarget(product, makeTarget(platform, Architecture::Any));
}

ProductArtifact artifactForTarget(const Product& product, const Target& target)
{
    return artifactForTargetT(product, target);
}

InstallPlan planInstall(const ProductCatalog& catalog,
                        const Vector<ProductReceipt>& receipts,
                        const std::string& productId,
                        Platform platform,
                        const std::string& artifactPath)
{
    auto plan = InstallPlan();
    if (!isValidProductId(productId))
        return plan;

    auto* product = findProduct(catalog, productId);
    if (product == nullptr)
        return plan;

    auto artifact = artifactForPlatform(*product, platform);
    if (artifact.url.empty())
        return plan;

    auto action = findReceipt(receipts, productId) == nullptr
                ? PlanAction::Install
                : PlanAction::Update;

    plan.operations.add(makeOperation(action,
                                      product->id,
                                      product->name,
                                      product->channel,
                                      product->latestVersion,
                                      artifactPath,
                                      artifact.sha256));
    return plan;
}

namespace
{
void appendInstallWithDependencies(const ProductCatalog& catalog,
                                   const Vector<ProductReceipt>& receipts,
                                   const std::string& productId,
                                   const Target& target,
                                   const std::string& artifactDirectory,
                                   std::set<std::string>& visiting,
                                   std::set<std::string>& planned,
                                   InstallPlan& plan)
{
    if (!isValidProductId(productId) || planned.contains(productId)
        || visiting.contains(productId))
        return;

    auto* product = findProduct(catalog, productId);
    if (product == nullptr)
        return;

    visiting.insert(productId);
    for (const auto& dependency: product->dependencies)
        appendInstallWithDependencies(catalog,
                                      receipts,
                                      dependency,
                                      target,
                                      artifactDirectory,
                                      visiting,
                                      planned,
                                      plan);
    visiting.erase(productId);

    auto artifact = artifactForTarget(*product, target);
    if (artifact.url.empty())
        return;

    auto action = findReceipt(receipts, productId) == nullptr
                ? PlanAction::Install
                : PlanAction::Update;

    plan.operations.add(makeOperation(action,
                                      product->id,
                                      product->name,
                                      product->channel,
                                      product->latestVersion,
                                      artifactPathFor(artifactDirectory,
                                                      product->id),
                                      artifact.sha256));
    planned.insert(productId);
}
} // namespace

InstallPlan planInstallWithDependencies(const ProductCatalog& catalog,
                                        const Vector<ProductReceipt>& receipts,
                                        const std::string& productId,
                                        const Target& target,
                                        const std::string& artifactDirectory)
{
    auto plan = InstallPlan();
    auto visiting = std::set<std::string>();
    auto planned = std::set<std::string>();

    appendInstallWithDependencies(catalog,
                                  receipts,
                                  productId,
                                  target,
                                  artifactDirectory,
                                  visiting,
                                  planned,
                                  plan);
    return plan;
}

InstallPlan planUpdateAll(const ProductCatalog& catalog,
                          const Vector<ProductReceipt>& receipts,
                          Platform platform,
                          const std::string& artifactDirectory)
{
    auto plan = InstallPlan();

    for (const auto& product: catalog.products)
    {
        if (!isValidProductId(product.id))
            continue;

        auto* receipt = findReceipt(receipts, product.id);
        if (receipt == nullptr)
            continue;

        if (!isNewerVersion(product.latestVersion, receipt->version))
            continue;

        auto artifact = artifactForPlatform(product, platform);
        if (artifact.url.empty())
            continue;

        plan.operations.add(makeOperation(PlanAction::Update,
                                          product.id,
                                          product.name,
                                          product.channel,
                                          product.latestVersion,
                                          artifactPathFor(artifactDirectory,
                                                          product.id),
                                          artifact.sha256));
    }

    return plan;
}

InstallPlan planUpdateAll(const ProductCatalog& catalog,
                          const Vector<ProductReceipt>& receipts,
                          const Target& target,
                          const std::string& artifactDirectory)
{
    auto plan = InstallPlan();

    for (const auto& product: catalog.products)
    {
        if (!isValidProductId(product.id))
            continue;

        auto* receipt = findReceipt(receipts, product.id);
        if (receipt == nullptr)
            continue;

        if (!isNewerVersion(product.latestVersion, receipt->version))
            continue;

        auto artifact = artifactForTarget(product, target);
        if (artifact.url.empty())
            continue;

        plan.operations.add(makeOperation(PlanAction::Update,
                                          product.id,
                                          product.name,
                                          product.channel,
                                          product.latestVersion,
                                          artifactPathFor(artifactDirectory,
                                                          product.id),
                                          artifact.sha256));
    }

    return plan;
}

InstallPlan planRemove(const std::string& productId)
{
    auto plan = InstallPlan();
    if (isValidProductId(productId))
        plan.operations.add(makeOperation(PlanAction::Remove, productId));
    return plan;
}

bool pathIsUnder(const std::string& path, const std::string& root)
{
    auto canonicalPath = canonicalExisting(path);
    auto canonicalRoot = canonicalExisting(root);

    std::error_code ec;
    auto relative = fs::relative(canonicalPath, canonicalRoot, ec);
    if (ec)
        return false;

    auto generic = relative.generic_string();
    return !generic.empty() && generic != ".." && generic.rfind("../", 0) != 0;
}

MockPrivilegedHelper::MockPrivilegedHelper(MockHelperOptions optionsToUse)
    : options(std::move(optionsToUse))
{
    if (options.stagingRoot.empty())
        options.stagingRoot = (fs::path(options.root) / "staging").string();
}

bool MockPrivilegedHelper::isInstalled() const
{
    std::error_code ec;
    fs::create_directories(applicationsRoot(), ec);
    if (ec)
        return false;

    fs::create_directories(receiptsRoot(), ec);
    return !ec;
}

Vector<ProductReceipt> MockPrivilegedHelper::receipts() const
{
    auto out = Vector<ProductReceipt>();
    std::error_code ec;
    auto root = fs::path(receiptsRoot());
    if (!fs::exists(root, ec))
        return out;

    for (const auto& entry: fs::directory_iterator(root, ec))
    {
        if (ec || !entry.is_regular_file())
            continue;

        try
        {
            out.add(parseReceiptJson(readTextFile(entry.path())));
        }
        catch (...)
        {
        }
    }

    return out;
}

InstallResult MockPrivilegedHelper::submit(const InstallPlan& plan)
{
    if (!isInstalled())
        return error("helper root could not be created");

    auto prepared = Vector<PreparedOperation>();
    if (auto result = preparePlan(options, receipts(), plan, prepared); !result.ok)
        return result;

    for (const auto& op: prepared)
    {
        auto result = op.request.action == PlanAction::Remove
                    ? executeRemove(op)
                    : executeInstall(op);
        if (!result.ok)
            return result;
    }

    return ok();
}

std::string MockPrivilegedHelper::applicationsRoot() const
{
    return (fs::path(options.root) / "Applications").string();
}

std::string MockPrivilegedHelper::receiptsRoot() const
{
    return (fs::path(options.root) / "receipts").string();
}

} // namespace eacp::Updater
