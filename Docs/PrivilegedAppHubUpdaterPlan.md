# Privileged App Hub Updater Plan

## Goal

Support an Adobe Creative Cloud style hub app that can install, update,
remove, and launch multiple native apps from one catalog while writing to
protected machine-level install locations. Apps are not the only installable
unit: AI models, runtimes, sample packs, scripts, and other binary blobs must be
first-class packages too.

The framework should provide the updater, catalog, package dependency planner,
and privileged helper building blocks. Product apps should not each own their
own privileged updater. They should be managed by one hub, one background agent,
and one narrowly scoped privileged install authority.

## Target Shape

```text
Tamber Hub.app / Tamber.exe          user-facing app catalog
Tamber Agent                         per-user background/tray/menu process
TamberUpdateService / privileged helper
                                     machine-level install/update/remove authority
/Applications/Tamber Apps/...        protected installed apps on macOS
C:\Program Files\Tamber\Apps\...     protected installed apps on Windows
```

## Responsibilities

### Hub App

- Display installed apps, shared packages, versions, channels, and install state.
- Let users install, update, remove, and launch app packages.
- Download package artifacts as the signed-in user.
- Verify the signed catalog, artifact hash, artifact signature, and version
  policy before asking the helper to install anything.
- Read helper-owned receipts to display the actual machine state.

### Agent

- Run as a per-user tray/menu-bar process.
- Poll for catalog updates.
- Continue downloads after the hub window closes.
- Coordinate updates that require the target app to quit.
- Show notifications.
- Keep product apps free of updater UI and privileged-install logic.

### Privileged Helper / Windows Service

- Own protected writes.
- Install, replace, rollback, and remove products.
- Register file associations, URL schemes, shortcuts, launch agents, services,
  and shared runtimes when needed.
- Write receipts after successful operations.
- Trust no paths, URLs, catalog metadata, or operation claims from the hub unless
  independently validated.

The helper should not fetch URLs or parse remote feeds. It should only accept a
verified local artifact and a narrow install request, then revalidate before
touching protected directories.

## Install Layout

### macOS

```text
/Applications/Tamber Hub.app
/Applications/Tamber Apps/Tamber Studio.app
/Applications/Tamber Apps/Tamber Capture.app
/Library/Application Support/Tamber/
  catalog.json
  receipts/
  shared/
  staging/
  rollback/
```

### Windows

```text
C:\Program Files\Tamber\Tamber Hub\Tamber.exe
C:\Program Files\Tamber\Apps\Tamber Studio\Tamber Studio.exe
C:\Program Files\Tamber\Apps\Tamber Capture\Tamber Capture.exe
C:\ProgramData\Tamber\
  catalog.json
  receipts\
  shared\
  staging\
  rollback\
```

## Catalog Model

The update feed should be a signed package catalog, not a single-app appcast.
An app is one package kind. Shared AI models, runtimes, and opaque blobs are
other package kinds and can be dependencies of many apps.

```json
{
  "catalogVersion": 42,
  "packages": [
    {
      "id": "tamber.studio",
      "name": "Tamber Studio",
      "kind": "App",
      "channel": "stable",
      "latestVersion": "3.4.1",
      "dependencies": ["shared.onnxruntime", "model.clap"],
      "artifacts": {
        "macos-arm64": {
          "url": "https://updates.example.com/tamber-studio-3.4.1-arm64.pkg",
          "sha256": "...",
          "signature": "..."
        },
        "windows-x64": {
          "url": "https://updates.example.com/tamber-studio-3.4.1-x64.msi",
          "sha256": "...",
          "signature": "..."
        }
      }
    },
    {
      "id": "model.clap",
      "name": "CLAP Model",
      "kind": "Model",
      "channel": "stable",
      "latestVersion": "c-a13f9a0db421",
      "dependencies": [],
      "artifacts": {
        "any-any": {
          "url": "https://updates.example.com/components/model.clap/c-a13f9a0db421.zip",
          "sha256": "...",
          "signature": "..."
        }
      }
    }
  ],
  "signature": "..."
}
```

## Receipt Model

Receipts are helper-owned machine state. The hub reads them, but does not write
them. There is one receipt per installed package, including shared models and
runtimes.

```json
{
  "productId": "tamber.studio",
  "kind": "App",
  "version": "3.4.1",
  "installPath": "/Applications/Tamber Apps/Tamber Studio.app",
  "channel": "stable",
  "artifactSha256": "...",
  "installedAt": "2026-06-29T00:00:00Z"
}
```

Receipts let the hub answer:

- Is this product installed?
- Are its shared package dependencies installed?
- Which version and channel are installed?
- Which artifact produced the installation?
- Is a rollback version available?
- Is the install state consistent with the catalog?

## Update Flow

```text
Hub/Agent checks catalog
Hub verifies catalog signature and monotonic catalog version
Hub computes install/update/remove plan, including shared dependencies
Hub downloads package artifacts to user cache
Hub verifies artifact hash, artifact signature, and version policy
Hub asks helper to install product X version Y from staged file Z
Helper validates caller and request
Helper re-hashes artifact
Helper verifies product signature or package signature
Helper extracts or installs into staging
Helper stops target app if allowed, or marks operation pending
Helper atomically swaps install directory
Helper writes receipt
Helper reports success or failure
Hub updates UI from receipts
```

For multi-package operations, the catalog produces a dependency-aware plan:

```cpp
auto plan = catalog.plan({
    .install = {"tamber.studio", "model.clap", "shared.onnxruntime"},
    .update = {"tamber.capture", "model.whisper"},
    .remove = {"tamber.legacy"}
});
```

The helper should execute plans transactionally where possible:

- Install shared package dependencies first.
- Stage all artifacts before replacing live apps.
- Keep the previous version available for rollback.
- Write receipts last.
- Never leave a half-written product directory marked as installed.

## Security Requirements

- The hub verifies before submitting an install request.
- The helper re-verifies before performing protected writes.
- The helper accepts only local staged artifact paths, not URLs.
- All helper file operations must be constrained to approved roots.
- Symlink and path traversal escapes must be rejected.
- Downgrades must be rejected unless an explicit developer policy enables them.
- The catalog version must be monotonic to prevent rollback attacks.
- The helper should bind each request to a nonce or request id.
- The helper should verify caller identity where the OS supports it.
- The helper should expose a narrow command set: install, update, remove,
  repair, query status, cancel pending operation.
- The helper should avoid general-purpose file copy, shell execution, arbitrary
  process launch, or arbitrary registry/plist writes.

## Framework API Sketch

```cpp
namespace eacp::Updater
{
struct ProductId
{
    std::string value;
};

struct ProductState
{
    ProductId id;
    std::string name;
    std::string installedVersion;
    std::string latestVersion;
    std::string channel;
    std::string installPath;
    bool installed = false;
    bool updateAvailable = false;
    bool running = false;
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
struct PlatformTraits;

template <typename ArchitectureEnum>
struct ArchitectureTraits;

template <typename PlatformEnum = Platform,
          typename ArchitectureEnum = Architecture>
struct TargetT
{
    PlatformEnum platform = PlatformEnum {};
    ArchitectureEnum architecture = ArchitectureEnum {};
};

template <typename PlatformEnum = Platform,
          typename ArchitectureEnum = Architecture>
struct ProductArtifactT
{
    PlatformEnum platform = PlatformEnum {};
    ArchitectureEnum architecture = ArchitectureEnum {};
    ProductId productId;
    std::string version;
    std::string url;
    std::string sha256;
    std::string signature;
};

using Target = TargetT<Platform, Architecture>;
using ProductArtifact = ProductArtifactT<Platform, Architecture>;

struct InstallPlan
{
    Vector<ProductId> install;
    Vector<ProductId> update;
    Vector<ProductId> remove;
};

struct InstallResult
{
    bool ok = false;
    std::string error;
};

class ProductCatalog
{
public:
    Vector<ProductState> products() const;
    InstallPlan planInstall(ProductId id) const;
    InstallPlan planUpdate(ProductId id) const;
    InstallPlan planRemove(ProductId id) const;
};

class HubUpdater
{
public:
    Threads::Async<ProductCatalog> refreshCatalog();
    Threads::Async<bool> downloadAndVerify(const InstallPlan& plan);
    Threads::Async<InstallResult> installViaHelper(const InstallPlan& plan);
};

class PrivilegedHelperClient
{
public:
    bool isInstalled();
    bool installOrRepair();
    Threads::Async<InstallResult> submit(const InstallPlan& plan);
};
}
```

The default aliases use EACP's own platform and architecture enums. A hub with
its own platform domain can use `TargetT<HubPlatform, HubArchitecture>` and the
matching templated catalog/artifact types, then specialize `PlatformTraits` and
`ArchitectureTraits` for domain-specific fallback behavior such as `Any` or
`Universal`.

## CMake Additions

Add build helpers for app-hub and privileged-helper targets:

```cmake
eacp_add_privileged_helper(<target>
    BUNDLE_ID ...
    SERVICE_NAME ...
    SOURCES ...
)

eacp_add_app_hub(<target>
    SOURCES ...
    AGENT <agent-target>
    HELPER <helper-target>
    CATALOG_URL ...
    PUBLIC_UPDATE_KEY ...
)
```

These helpers should eventually handle:

- macOS helper bundle placement and plist generation.
- macOS helper registration metadata.
- Windows service executable target metadata.
- Windows service install/uninstall commands for development.
- Code signing and notarization integration points.
- Dev-mode install-root overrides for tests.

## Platform Backends

### macOS

- Use a privileged helper registered through ServiceManagement.
- Prefer the modern `SMAppService` path where possible.
- Keep compatibility with older `SMJobBless` style requirements only if
  deployment targets require it.
- Install apps under `/Applications/Tamber Apps`.
- Keep machine state under `/Library/Application Support/Tamber`.

### Windows

- Use a Windows Service for machine-level writes.
- Communicate through a named pipe with an explicit security descriptor.
- Install apps under `C:\Program Files\Tamber\Apps`.
- Keep machine state under `C:\ProgramData\Tamber`.
- Use Authenticode-signed artifacts and helper binaries.
- Avoid per-user Squirrel-style installs for the product apps when the goal is
  a protected per-machine install.

## Example App

Create:

```text
Apps/System/AppHub
```

Targets:

- `AppHub`
- `AppHubAgent`
- `AppHubPrivilegedHelper`
- `ExampleEditor`
- `ExampleCapture`

The example should demonstrate:

- Catalog refresh from a local dev catalog.
- Install `ExampleEditor`.
- Install `ExampleCapture`.
- Update one product.
- Remove one product.
- Helper-owned receipts.
- Protected-root writes through the helper.
- Rollback state.
- Dev-mode root override so CI can exercise behavior without admin elevation.

For local development, the helper can support a simulation mode that writes to a
temporary install root:

```text
/tmp/eacp-apphub-demo/
  Applications/
  ProgramData/
  receipts/
  staging/
```

The same command protocol should be used in simulation mode so tests cover the
real helper boundary.

## Implementation Phases

1. Add `Lib/eacp/Updater` interfaces and a fake in-process backend for tests.
2. Add signed product catalog parsing and monotonic catalog-version policy.
3. Add SHA-256 artifact verification.
4. Add app-hub planning for install, update, remove, dependencies, and
   rollback.
5. Add helper command protocol and request validation.
6. Add dev-mode helper backend that writes to a temporary root.
7. Add platform IPC abstraction.
8. Add Windows service backend.
9. Add macOS privileged helper backend.
10. Add CMake helper functions for privileged helper and app-hub targets.
11. Add `Apps/System/AppHub` with two example products.
12. Add tests for bad hash, invalid signature, downgrade rejection, path
    traversal, receipt ownership, rollback, and partial-install recovery.

## Near-Term Slice

The first useful milestone should avoid OS-specific elevation and still prove
the framework shape:

- `Lib/eacp/Updater` with catalog, receipt, plan, verifier, and helper-client
  abstractions.
- A dev helper backend that uses the same protocol and writes to a temp root.
- `Apps/System/AppHub` showing two products and helper-owned receipts.
- Tests for catalog planning, artifact verification, receipt writes, and path
  containment.

After that slice is stable, attach the real macOS and Windows privileged helper
backends behind the same protocol.
