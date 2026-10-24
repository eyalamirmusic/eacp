# Task: prove and fix the D3D12 prefix-scan overread on x64

This is a brief for Claude Code running on an **x64 Windows machine with a
real D3D12 GPU**. It was written on a Windows-on-ARM VM, which can't run the
test (see "Why this needs your machine"). Work through the steps in order and
report the results of each one.

The branch `fix/d3d12-prefix-scan-overread` contains this file and a **test
that has never been compiled**. The fix isn't applied yet; step 4 adds it.

## The bug

`GPUWidgets::PrefixSum` runs `ScanBlockKernel`
(`Lib/eacp/GPUWidgets/Path/PrefixSumKernels.cpp`). The kernel guards its 32
reads of `counts` with `at < elementCount`:

```cpp
ifThen(at < elementCount, [&] { mine += counts.load(at); });
...
ifThen(at < elementCount, [&] { auto value = counts.load(at); ... });
```

On D3D12 these reads still go past the end of `counts` on every partial
thread group. A group is 64 lanes × 16 elements = 1024 elements, so any count
that isn't a multiple of 1024 has a partial group. The computed sums are still
correct, so the existing `PrefixSum` tests pass. Only D3D12 GPU-based
validation (GBV) reports the overread, as
`D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_ROOT_DESCRIPTOR_ACCESS_OUT_OF_BOUNDS`
(961).

The cause has two parts:

1. `GPU/Shader/ShaderLibrary-Windows.cpp` compiles compute shaders with FXC as
   `cs_5_0`. Under Shader Model 5, an out-of-range structured load through a
   descriptor returns 0, so FXC removes the `if` and masks the result instead.
   The load runs unconditionally:

   ```text
   ult r0.y, r0.x, cb0[0].x
   ld_structured_indexable(structured_buffer, stride=4) r0.x, r0.x, l(0), u0.xxxx
   and r0.x, r0.x, r0.y
   ```

2. The backend binds compute buffers as **root UAVs**
   (`GPU/Windows/D3D12Context-Windows.cpp`, `GPU/Frame/ComputePass-Windows.cpp`,
   `SetComputeRootUnorderedAccessView`). A root descriptor carries no size, so
   nothing bounds that load.

The same HLSL compiled as `cs_5_1` keeps the branch, but this task doesn't
change the backend-wide profile.

## The test (already on the branch, never compiled)

- `Tests/GPUWidgets/PrefixSumValidationTests-Windows.cpp` builds the
  Windows-only executable `GPUWidgetsValidationTests`. Its one case is
  `PrefixSumValidation/scansReadOnlyInsideTheirBuffers`, and the ctest group
  is `PrefixSumValidation`.
- It's registered in `Tests/GPUWidgets/CMakeLists.txt` with `NO_MAIN`, and it
  links `d3d12` and `dxgi`.

How it works:

- GBV must be enabled **before** the D3D12 device exists, so the file's own
  `main` calls `EnableDebugLayer()` and `SetEnableGPUBasedValidation(TRUE)`
  before `nano::run`. Nothing may touch `GPU::Device::shared()` before that.
- **Positive control:** `OverreadKernel` reads 64 elements past a one-element
  buffer, bound as a root UAV like `counts`. The case trusts a clean scan only
  if GBV reports that read with message 961.
- Then it runs the full `PrefixSum` over exact-sized buffers at every size
  `PrefixSumTests` uses (1 up to 1024² + 17). It checks the offsets against the
  running sum and the counts against zero. After each size it fails if the info
  queue holds any error-severity message, and it reports how many were
  out-of-bounds errors.

The case **skips**, printing `GPU-based validation unavailable, skipping:
<reason>`, when any of these is true:

- There's no debug layer (the Graphics Tools optional feature isn't installed).
- There's no device.
- GBV doesn't report the control read.
- The adapter is WARP.

Set `EACP_REQUIRE_GPU_VALIDATION=1` to turn a skip into a failure. Keep it set
for every run in this task, so a skip can never pass as a result.

Why skip on WARP: on the ARM VM, WARP's GBV reports the overread correctly. But
WARP then crashes with an access violation inside its shader JIT while running
the full validated scan kernel. It happens on x64 and ARM64 WARP, with and
without the fix. It's a WARP bug, and the skip keeps the Windows CI lanes, which
run on WARP, from crashing.

## Steps

### 0. Check the machine

- Confirm D3D12 runs on a hardware adapter, not WARP. Make sure
  `EACP_D3D12_WARP` isn't set.
- Confirm Graphics Tools is installed: `C:\Windows\System32\d3d12SDKLayers.dll`
  exists, and if it doesn't, *Settings → System → Optional features → Graphics
  Tools*.
- Read `CLAUDE.md`. Its git rules apply: no commits or pushes without the
  user's permission, and Claude never appears in commit metadata. Activate the
  repo's hooks with `git config core.hooksPath .githooks`.

### 1. Build the test

From an x64 developer shell (Developer PowerShell or x64 Native Tools):

```powershell
git checkout fix/d3d12-prefix-scan-overread
cmake -G Ninja -B build-x64 -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF
cmake --build build-x64 --target GPUWidgetsValidationTests GPUWidgetsTests
```

The test file hasn't been compiled before, so fix any compile errors in it. Keep
its approach (own `main`, positive control, skips) and the repo's style:
Allman braces, `auto`, 85 columns, `.clang-format`.

Points most likely to need attention:

- `skipBecause` returns `bool` and is used as
  `return skipBecause(...), nullptr;` inside a function returning `com_ptr`.
- `OverreadKernel` uses `threadId() + distance` and `source.load(...)` on an
  `AtomicBuffer`.
- `sharedKernel<OverreadKernel>()` comes from `GPUWidgets/Path/CoverageKernel.h`.
- `GetMessage` is the `windows.h` macro, which is consistent with how
  `d3d12sdklayers.h` declares the method.

### 2. Run it on the unfixed code: it must fail

```powershell
$env:EACP_REQUIRE_GPU_VALIDATION = '1'
.\build-x64\Tests\GPUWidgets\GPUWidgetsValidationTests.exe
```

Expected:

- A line `GPU-based validation is reporting on <GPU name>`, meaning the control
  passed.
- Failures at every size except 1024 and 2048, each with root-descriptor
  out-of-bounds errors.
- Messages similar to `Root descriptor access out of bounds ... Root Parameter
  Index: [10] ... Highest byte offset from view start accessed: [4035], Bytes
  available ...: 516` at 129 elements.

If it skips instead, stop and report the printed reason. Also check
`.\build-x64\Tests\GPUWidgets\GPUWidgetsTests.exe --test PrefixSum/withinOneGroup`
passes, which shows the numbers are right even while the reads go out of bounds.

### 3. Commit the test (only with the user's permission)

Once it compiles and fails as expected, ask the user before committing. Suggested
message: `Tests: D3D12 GPU-based validation of the prefix scan`.

### 4. Apply the fix

In `Lib/eacp/GPUWidgets/Path/PrefixSumKernels.cpp`, clamp **both** `counts.load`
addresses in `ScanBlockKernel::define()` to the last element. Keep the
`at < elementCount` guards, the contribution and store logic, and every
`barrier()` exactly as they are:

```cpp
// D3D12 compiles a guarded read as an unconditional load whose value the guard
// then discards, through a root UAV that carries no size - so the address has
// to stay inside the counts, not only the value that is kept.
auto clampedToCounts = [&](auto at) { return min(at, elementCount - 1u); };
...
ifThen(at < elementCount, [&] { mine += counts.load(clampedToCounts(at)); });
...
auto value = counts.load(clampedToCounts(at));
```

`PrefixSum::run` returns before dispatching for `count <= 0`, so
`elementCount - 1u` can't wrap. If the lambda doesn't compile in the EDSL, write
`min(at, elementCount - 1u)` inline at both reads; that exact spelling was
validated. `ScanAddKernel` has no guarded reads and needs no change.
Optionally, extend the comment on `elementCount` in `PrefixSumKernels.h`: it
now bounds the reads as well as the stores.

### 5. Run it on the fixed code: it must pass

```powershell
cmake --build build-x64 --target GPUWidgetsValidationTests GPUWidgetsTests
.\build-x64\Tests\GPUWidgets\GPUWidgetsValidationTests.exe   # 0 errors at every size
.\build-x64\Tests\GPUWidgets\GPUWidgetsTests.exe             # all pass, PrefixSum/* included
```

As an extra check, revert only the fix and confirm the validation test fails
again.

### 6. Run the wider suite

```powershell
cmake --build build-x64
ctest --test-dir build-x64 --output-on-failure -j 8
```

Report anything that fails, and say whether it also fails on `develop` without
this branch's changes.

### 7. Commit the fix (only with the user's permission)

Suggested message: `GPUWidgets: clamp the prefix scan's count reads to the
buffer`. Then update the "Results" section below with what was observed.

## Evidence already gathered (ARM VM)

- With a standalone D3D12 harness on WARP, EACP's own generated
  `ScanBlockKernel` HLSL reproduces the bug. Its first read loop alone produced
  exactly one out-of-bounds error per out-of-range load: 1023 at 1 element, 895
  at 129, 0 at 1024, and 1023 at 1025. With the `min()` clamp, it produced 0 at
  every size.
- FXC (SDK 10.0.26100) output: `cs_5_0` loads unconditionally and `cs_5_1`
  keeps the `if_nz`. After the clamp, the DXBC has `umin` before the loads.
- The original report measured the same thing on x64 with an RTX 2080 SUPER
  using a standalone EACP harness: 1,024 retained GBV errors before the clamp
  and 0 after, with correct sums in both cases.

## Why this needs your machine

On the ARM VM, the Parallels display adapter accepts GBV but reports nothing,
not even the deliberate control overread. WARP reports the overread but crashes
running the full validated scan kernel. A hardware x64 GPU with Graphics Tools
is the only setup known to run this test end to end.

## Out of scope (for follow-up)

- Audit other kernels for buffer reads guarded only by an `ifThen`, especially
  kernels with barriers, which have no generated bounds guard.
- `Lib/eacp/GPU/README.md` says an out-of-bounds read returns 0 on D3D12. That
  holds through descriptors but not through the root descriptors this backend
  uses.
- Backend-wide alternatives (`cs_5_1`, or descriptor tables with real bounds)
  haven't been qualified.

## Results

_To be filled in on the x64 machine._
