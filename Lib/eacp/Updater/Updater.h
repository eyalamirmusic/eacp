#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <Miro/Miro.h>

#include <string>

namespace eacp::Updater
{

struct ProductId
{
    std::string value;

    MIRO_REFLECT(value)
};

enum class PackageKind
{
    App,
    Runtime,
    Model,
    Blob
};

enum class Platform
{
    Any,
    Windows,
    MacOS,
    Linux
};

enum class Architecture
{
    Any,
    X64,
    Arm64,
    Universal
};

template <typename PlatformEnum>
struct PlatformTraits
{
    static int specificity(PlatformEnum artifact, PlatformEnum target)
    {
        return artifact == target ? 2 : -1;
    }
};

template <>
struct PlatformTraits<Platform>
{
    static int specificity(Platform artifact, Platform target)
    {
        if (artifact == Platform::Any)
            return 0;
        if (target == Platform::Any)
            return 0;
        return artifact == target ? 2 : -1;
    }
};

template <typename ArchitectureEnum>
struct ArchitectureTraits
{
    static int specificity(ArchitectureEnum artifact, ArchitectureEnum target)
    {
        return artifact == target ? 2 : -1;
    }
};

template <>
struct ArchitectureTraits<Architecture>
{
    static int specificity(Architecture artifact, Architecture target)
    {
        if (artifact == Architecture::Any)
            return 0;
        if (artifact == Architecture::Universal)
            return 1;
        if (target == Architecture::Any)
            return 0;
        return artifact == target ? 2 : -1;
    }
};

template <typename PlatformEnum = Platform,
          typename ArchitectureEnum = Architecture>
struct TargetT
{
    using PlatformType = PlatformEnum;
    using ArchitectureType = ArchitectureEnum;

    PlatformEnum platform = PlatformEnum {};
    ArchitectureEnum architecture = ArchitectureEnum {};

    MIRO_REFLECT(platform, architecture)
};

template <typename PlatformEnum = Platform,
          typename ArchitectureEnum = Architecture>
struct ProductArtifactT
{
    using PlatformType = PlatformEnum;
    using ArchitectureType = ArchitectureEnum;

    PlatformEnum platform = PlatformEnum {};
    ArchitectureEnum architecture = ArchitectureEnum {};
    std::string url;
    std::string sha256;
    std::string signature;

    MIRO_REFLECT(platform, architecture, url, sha256, signature)
};

template <typename PlatformEnum = Platform,
          typename ArchitectureEnum = Architecture>
struct ProductT
{
    using PlatformType = PlatformEnum;
    using ArchitectureType = ArchitectureEnum;
    using ArtifactType = ProductArtifactT<PlatformEnum, ArchitectureEnum>;

    std::string id;
    std::string name;
    PackageKind kind = PackageKind::App;
    std::string channel = "stable";
    std::string latestVersion;
    Vector<std::string> dependencies;
    Vector<ArtifactType> artifacts;

    MIRO_REFLECT(id,
                 name,
                 kind,
                 channel,
                 latestVersion,
                 dependencies,
                 artifacts)
};

template <typename PlatformEnum = Platform,
          typename ArchitectureEnum = Architecture>
struct ProductCatalogT
{
    using PlatformType = PlatformEnum;
    using ArchitectureType = ArchitectureEnum;
    using ProductType = ProductT<PlatformEnum, ArchitectureEnum>;

    int catalogVersion = 0;
    Vector<ProductType> products;
    std::string signature;

    MIRO_REFLECT(catalogVersion, products, signature)
};

using Target = TargetT<Platform, Architecture>;
using ProductArtifact = ProductArtifactT<Platform, Architecture>;
using Product = ProductT<Platform, Architecture>;
using ProductCatalog = ProductCatalogT<Platform, Architecture>;

template <typename ArtifactType, typename TargetType>
int artifactSpecificity(const ArtifactType& artifact, const TargetType& target)
{
    auto platformScore =
        PlatformTraits<typename ArtifactType::PlatformType>::specificity(
            artifact.platform,
            target.platform);
    if (platformScore < 0)
        return -1;

    auto architectureScore =
        ArchitectureTraits<typename ArtifactType::ArchitectureType>::specificity(
            artifact.architecture,
            target.architecture);
    if (architectureScore < 0)
        return -1;

    return platformScore + architectureScore;
}

template <typename ProductType, typename TargetType>
typename ProductType::ArtifactType artifactForTargetT(const ProductType& product,
                                                      const TargetType& target)
{
    const auto* best =
        static_cast<const typename ProductType::ArtifactType*>(nullptr);
    auto bestScore = -1;

    for (const auto& artifact: product.artifacts)
    {
        auto score = artifactSpecificity(artifact, target);
        if (score > bestScore)
        {
            best = &artifact;
            bestScore = score;
        }
    }

    return best == nullptr ? typename ProductType::ArtifactType {} : *best;
}

struct ProductReceipt
{
    std::string productId;
    std::string name;
    std::string version;
    std::string installPath;
    std::string channel;
    std::string artifactSha256;
    std::string installedAt;

    MIRO_REFLECT(productId,
                 name,
                 version,
                 installPath,
                 channel,
                 artifactSha256,
                 installedAt)
};

enum class PlanAction
{
    Install,
    Update,
    Remove
};

struct PlanOperation
{
    PlanAction action = PlanAction::Install;
    std::string productId;
    std::string name;
    std::string channel;
    std::string version;
    std::string artifactPath;
    std::string artifactSha256;

    MIRO_REFLECT(action,
                 productId,
                 name,
                 channel,
                 version,
                 artifactPath,
                 artifactSha256)
};

struct InstallPlan
{
    Vector<PlanOperation> operations;

    MIRO_REFLECT(operations)
};

struct InstallResult
{
    bool ok = false;
    std::string error;

    MIRO_REFLECT(ok, error)
};

struct MockHelperOptions
{
    std::string root;
    std::string stagingRoot;
    bool allowDowngrade = false;
};

ProductCatalog parseCatalogJson(const std::string& json);
std::string catalogToJson(const ProductCatalog& catalog);
ProductReceipt parseReceiptJson(const std::string& json);
std::string receiptToJson(const ProductReceipt& receipt);
InstallPlan parseInstallPlanJson(const std::string& json);
std::string installPlanToJson(const InstallPlan& plan);

const Product* findProduct(const ProductCatalog& catalog,
                           const std::string& productId);
const ProductReceipt* findReceipt(const Vector<ProductReceipt>& receipts,
                                  const std::string& productId);

int compareVersions(const std::string& lhs, const std::string& rhs);
bool isNewerVersion(const std::string& candidate, const std::string& current);
bool isValidProductId(const std::string& productId);

ProductArtifact artifactForPlatform(const Product& product, Platform platform);
ProductArtifact artifactForTarget(const Product& product, const Target& target);
InstallPlan planInstall(const ProductCatalog& catalog,
                        const Vector<ProductReceipt>& receipts,
                        const std::string& productId,
                        Platform platform,
                        const std::string& artifactPath);
InstallPlan planInstallWithDependencies(const ProductCatalog& catalog,
                                        const Vector<ProductReceipt>& receipts,
                                        const std::string& productId,
                                        const Target& target,
                                        const std::string& artifactDirectory);
InstallPlan planUpdateAll(const ProductCatalog& catalog,
                          const Vector<ProductReceipt>& receipts,
                          Platform platform,
                          const std::string& artifactDirectory);
InstallPlan planUpdateAll(const ProductCatalog& catalog,
                          const Vector<ProductReceipt>& receipts,
                          const Target& target,
                          const std::string& artifactDirectory);
InstallPlan planRemove(const std::string& productId);

bool pathIsUnder(const std::string& path, const std::string& root);

class MockPrivilegedHelper
{
public:
    explicit MockPrivilegedHelper(MockHelperOptions options);

    bool isInstalled() const;
    Vector<ProductReceipt> receipts() const;
    InstallResult submit(const InstallPlan& plan);

    std::string applicationsRoot() const;
    std::string receiptsRoot() const;

private:
    MockHelperOptions options;
};

} // namespace eacp::Updater
