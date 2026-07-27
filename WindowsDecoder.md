# Windows video decode: where it stands and what is left

The Windows decode backend (`Lib/eacp/Video/Decode/Decoder-Windows.cpp`) runs on
Media Foundation's `IMFSourceReader`. This is the plan for taking it to hardware
decode, plus what was measured getting it to its current state, so the next
person does not have to rediscover it.

**If you are picking this up on a machine with a real GPU, skip to
"First session on real hardware".** Everything above it is context.

## Where it stands

Frames are decoded by Media Foundation into NV12, copied out to a recycled CPU
buffer, uploaded as two textures (`R8Unorm` luma, `RG8Unorm` chroma) and
converted to RGB in the sprite shader. Nothing converts colour on the CPU, and
nothing allocates per frame. **Decode is entirely in software** — see the next
section for why.

The work that got it here, in the order it was done:

- **Recycled decode buffers.** `VideoFrame` shares a `shared_ptr` pixel buffer;
  the decoder reuses any buffer no live frame still reads. This removed a
  per-frame allocation, a page fault per page, and a full zero-fill of the
  buffer that was immediately overwritten. Biggest single win: 8K decode went
  from 12.6 to 34.5 fps.
- **Pooled D3D12 staging buffers.** `D3D12Context::acquireStagingBuffer` lends
  from a fence-tracked pool instead of a `CreateCommittedResource` per frame.
  At 8K that call was allocating and freeing a 133 MB upload heap every frame.
- **`FrameStream::Stats::starved`.** Counts `frameAt()` calls handed a stale
  frame. `skipped` only sees frames that were decoded and then passed over, so
  it cannot see a frame that was never decoded — and at 8K it read a reassuring
  zero while the picture juddered.
- **`maxQueueBytes` raised to 512 MiB**, so 8K keeps the full queue depth of 4
  instead of collapsing to 1. Correct in principle — a queue of one gives the
  decode thread zero slack — but see the measurements: it buys nothing while
  decode is below real time.
- **NV12 end to end.** Cut per-frame bytes from 4 to 1.5 per pixel through both
  the copy and the upload, and removed the software colour-conversion MFT.
- **MP4 demuxer.** `Video::Mp4Demuxer` (`Lib/eacp/Video/Demux`) parses one video
  track's sample tables over a `MemoryMappedFile`. Not yet wired into decode.

## The blocker: no hardware decode in the dev VM

The machine this was developed on is a Parallels ARM VM on Apple Silicon (8
cores, Windows on ARM). Its display adapter exposes **no video decoder through
any API**. Full probe output, 2026-07-26:

```
=== DXGI adapters ===
  [0] Parallels Display Adapter (WDDM)  vendor=5404C42 device=0000
  [1] Microsoft Basic Render Driver     vendor=1414 device=008C (SOFTWARE)

=== D3D12 video ===
  ID3D12VideoDevice NOT exposed

=== D3D11 video ===
  decoder profiles: 0
  video processor: available (NV12 input yes)

=== Media Foundation decoder MFTs ===
  H264  HARDWARE : 0
  H264  SOFTWARE : 1   Microsoft H264 Video Decoder MFT
  HEVC  HARDWARE : 0
  HEVC  SOFTWARE : 1   HEVCVideoExtension
```

Apple Silicon has a hardware Media Engine that decodes H.264 and HEVC. Parallels
does not pass it through to the Windows guest. Nothing in the guest can route
around that.

Three things in that output are worth keeping:

- **The MFT enumeration is the conclusive test**, more so than the D3D checks.
  It answers the question from the other side: it is not merely that the D3D
  APIs advertise no decoder, it is that Media Foundation has no hardware decoder
  registered to select. If `MFT_ENUM_FLAG_HARDWARE` returns zero for your
  codec, no amount of device-manager plumbing will produce hardware decode.
- **A video processor exists without a decoder.** The D3D11 video processor does
  fixed-function NV12→RGB, scaling and deinterlace, and it is available here.
  It is useless to us: the render path already sits at the vsync cap and the
  colour conversion is free in the sprite shader. It would accelerate the part
  that is not the bottleneck. Do not be tempted by it.
- **The software HEVC decoder has no tunable knobs.** The H.264 MFT exposes
  `ICodecAPI` with `AVDecNumWorkerThreads = -1` (auto, range up to 32) on 8
  cores — already self-tuning. `HEVCVideoExtension` exposes **no `ICodecAPI` at
  all**. So the slowest case, 8K HEVC, has zero software levers. The wall is the
  software decode itself and on this VM it is unmovable.

## Which API reaches the most machines

D3D12 *does* have a decode API — `ID3D12VideoDevice`, `ID3D12VideoDecoder`,
`D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE`, in `d3d12video.h` since Windows 10 1809.
The instinct is that D3D12 is the modern choice and therefore the safe one. For
video decode that instinct is wrong, and it is the main correction this document
makes to its earlier draft.

**D3D12 video decode is a separate driver opt-in from DXVA2/D3D11 decode.** A
machine having a working hardware decoder does not imply `ID3D12VideoDevice` is
exposed. Real hardware routinely advertises D3D11 decoder profiles while
returning nothing for D3D12 video, because the vendor never implemented that DDI
for that generation, or the OEM-pinned driver predates it. Support skews by
driver age more than by GPU age. Current NVIDIA/AMD/Intel drivers on anything
reasonably modern are fine; older GPUs, frozen OEM drivers and Windows-on-ARM are
where it thins out. The ordering is not in doubt even without market-share
numbers: **D3D11/DXVA2 decode support is strictly wider than D3D12 decode
support**, because MF/DXVA2 has a decade-plus head start.

So the fallback path is not just VMs like this one. It is also:

- Remote Desktop and Citrix sessions, which commonly drop video acceleration
- GPU-paravirtualised cloud and Hyper-V guests
- Older or OEM-frozen drivers on otherwise capable hardware
- WARP and the Basic Render Driver

**And support is not one binary check per machine.**
`D3D12_FEATURE_VIDEO_DECODE_SUPPORT` is queried per profile, per resolution, per
bit depth — which is why the probe below checks H.264 4K, HEVC 4K and HEVC 8K
separately. A GPU can do H.264 to 4K but not 8K HEVC Main10. That pushes the
fallback decision to `Decoder::open()` time, per clip, not to startup.

## Recommendation: Route A (Media Foundation on D3D11)

Keep Media Foundation. Bind the source reader to a D3D11 device so MF selects the
*hardware* decoder MFT, and interop the output textures into D3D12.

The decisive argument is not performance — both routes end at the same hardware
block — it is what the fallback costs.

**Route B's fallback is the entire Media Foundation path.** If D3D12 video is
absent, or present but not for this clip's profile, you still need a complete
working decoder. So Route B does not replace today's code, it adds a second full
codec front-end *on top* of permanently maintaining the first. And the new one is
the spec-heavy half: bitstream parsing, POC derivation, DPB management, DXVA
picture parameters — the bulk of what FFmpeg's hwaccel layer does, easy to get
subtly wrong in ways that only show up on rare clips.

**Route A degrades on the same axis for free.** Same MF code either way; you bind
`MF_SOURCE_READER_D3D_MANAGER` when a D3D11 video device exists and you do not
when it does not. The fallback is literally today's behaviour, and it lands on
the wider support surface while doing so.

The only strong argument for Route B is independence from MF's container and
codec support — and `Video::Mp4Demuxer` already buys most of that separately.
Reach for Route B only after hitting something MF genuinely will not decode.

### Route A, staged

Route A is two independent pieces and only the second is risky. Do not do them
in one go.

**Step 1 — bind the D3D11 device manager.** This alone gets hardware decode.

1. `D3D11CreateDevice` with `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`.
2. Set `ID3D10Multithread::SetMultithreadProtected(TRUE)` on the device — MF
   drives it from its own threads.
3. `MFCreateDXGIDeviceManager` + `ResetDevice(d3d11Device)`.
4. Set `MF_SOURCE_READER_D3D_MANAGER` on the reader attributes before
   `MFCreateSourceReaderFromURL`.
5. Leave the existing CPU readback and NV12 upload path **exactly as they are**.

Contained, mostly attribute-setting, and it fails safely: no video device means
no manager and the current path is untouched. It captures the large win, because
hardware versus software decode dwarfs everything in the measurement table below.

**Step 2 — D3D11↔D3D12 interop**, to remove the GPU→CPU→GPU roundtrip.

1. Frames arrive as `IMFDXGIBuffer` wrapping an `ID3D11Texture2D` (NV12, usually
   a slice of a texture array — check `IMFDXGIBuffer::GetSubresourceIndex`).
2. The decoder's textures are not shared, so copy into one created with
   `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYED_MUTEX`.
3. `ID3D12Device::OpenSharedHandle` on the D3D12 side.
4. Synchronise with the keyed mutex or a shared fence.

The NV12 sampling path this needs **already exists** — that is what `Nv12Shader`
and `VideoView::drawUpload` do today, and `UploadMode::ZeroCopy` is already the
knob for forcing it. Only the texture source changes. `D3D12Context.h` already
records that the 2D layer keeps its own D3D11 device for Direct2D and that the
two stacks meet in the compositor, so cross-device interop is not new ground.

Step 2 is the genuinely blind part: shared-handle synchronisation cannot be
validated without a decoder producing real textures.

### How much step 2 actually buys depends on the GPU

Step 1 leaves a GPU→CPU readback, and its cost is not the same everywhere:

- **Discrete GPU.** Readback is the slow PCIe direction. 8K NV12 is ~50 MB per
  frame; at 60 fps that is ~3 GB/s of readback. Expect step 1 to solve 4K
  outright and expect 8K to still want step 2.
- **Intel integrated (UMA).** There is no PCIe crossing — CPU and GPU share
  memory. Readback is over the memory bus, and the real cost is detiling and
  uncached reads rather than a bus transfer. Step 1 may well be sufficient at
  both 4K and 8K, which would make the risky half unnecessary.

Measure after step 1 before committing to step 2. A mediocre 8K number on a
discrete card is expected and is not a failed approach.

## First session on real hardware

Concrete order of work for the Intel machine. The point of this ordering is that
nothing is written blind until it has been shown to be needed.

1. **Run the probe** (source below). Confirm `H264 HARDWARE` and/or
   `HEVC HARDWARE` are non-zero and note the friendly names — on Intel you should
   see an Intel hardware decoder MFT rather than `Microsoft H264 Video Decoder
   MFT` / `HEVCVideoExtension`. Also note whether `ID3D12VideoDevice` is exposed
   and which profiles pass, purely as a data point for the Route B question.
   **Paste the output into this file**, next to the VM output above.
2. **Baseline before changing anything.** Release build, `PlayingHeavyContent`,
   the 4K and 8K clips, 20 s each. The numbers in the table below are from an ARM
   VM and are worthless as a comparison on x86 — you need this machine's own
   software-decode baseline or step 1's win is unquantifiable.
3. **Implement step 1.** Behind a capability check, so a machine without a video
   device silently keeps today's path.
4. **Re-measure.** Decode fps, `starved`, `skipped`, CPU seconds, resident bytes.
5. **Decide on step 2 from those numbers**, per the UMA/discrete split above.

Two things to expect trouble from, in order of likelihood:

- **COM apartment model.** `Decoder::open` calls `CoInitializeEx` with
  `COINIT_APARTMENTTHREADED` on the calling thread, while `FrameStream` drives
  `nextFrame`/`seek` from its own decode thread, which never initialises COM at
  all. This survives today with a software MFT. It is much more likely to matter
  once a hardware MFT and a D3D device manager are in the pipeline — MF generally
  wants MTA. **This is the first thing to suspect if the new path hangs, returns
  `E_UNEXPECTED`, or fails only on the decode thread.** Switching the decode
  thread to explicit `COINIT_MULTITHREADED` is likely a prerequisite, not a
  cleanup.
- **`MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING`** is set today
  (`Decoder-Windows.cpp:129`). With a D3D manager bound it can insert a video
  processor into the chain and quietly undo the "no conversion MFT" work. Try
  clearing it once hardware decode is running and confirm the output media type
  is still NV12.

A note on the source reader and async MFTs: hardware decoder MFTs are typically
asynchronous, but `IMFSourceReader` manages async MFTs internally, so the
existing synchronous `ReadSample` loop should survive. The apartment question
above is the real risk, not the loop shape.

## The capability probe

Self-contained. Build and run:

```
clang-cl /std:c++20 /EHsc VideoProbe.cpp /link d3d11.lib d3d12.lib dxgi.lib \
    dxguid.lib mfplat.lib mfuuid.lib ole32.lib oleaut32.lib strmiids.lib
```

`ICodecAPI` comes from `strmif.h`; including `icodecapi.h` as well is a
redefinition error.

```cpp
#include <windows.h>
#include <strmif.h>

#include <codecapi.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <cstdio>

template <class T>
struct Rel
{
    T* p = nullptr;
    ~Rel() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
};

static void probeAdapters()
{
    std::printf("=== DXGI adapters ===\n");
    Rel<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return;

    for (UINT i = 0;; ++i)
    {
        Rel<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;

        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        char name[256] {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, 256,
                            nullptr, nullptr);
        std::printf("  [%u] %s  vendor=%04X device=%04X %s\n", i, name,
                    desc.VendorId, desc.DeviceId,
                    (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "(SOFTWARE)"
                                                              : "");
    }
    std::printf("\n");
}

static void probeD3D12()
{
    std::printf("=== D3D12 video ===\n");
    Rel<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&device))))
    {
        std::printf("  D3D12CreateDevice failed\n\n");
        return;
    }

    Rel<ID3D12VideoDevice> video;
    if (FAILED(device.p->QueryInterface(IID_PPV_ARGS(&video))))
    {
        std::printf("  ID3D12VideoDevice NOT exposed\n\n");
        return;
    }

    struct Case { const GUID& profile; const char* name; UINT w, h; };
    const Case cases[] = {
        {D3D12_VIDEO_DECODE_PROFILE_H264, "H264 4K", 3840, 2160},
        {D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN, "HEVC 4K", 3840, 2160},
        {D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN, "HEVC 8K", 7680, 4320},
    };

    for (auto& c: cases)
    {
        D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT s {};
        s.Configuration.DecodeProfile = c.profile;
        s.Configuration.BitstreamEncryption =
            D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE;
        s.Configuration.InterlaceType =
            D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;
        s.Width = c.w;
        s.Height = c.h;
        s.DecodeFormat = DXGI_FORMAT_NV12;

        auto hr = video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT,
                                             &s, sizeof(s));
        std::printf("  %-8s : %s\n", c.name,
                    (SUCCEEDED(hr) && (s.SupportFlags
                        & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED))
                        ? "SUPPORTED" : "no");
    }
    std::printf("\n");
}

static void probeD3D11()
{
    std::printf("=== D3D11 video ===\n");
    Rel<ID3D11Device> device;
    Rel<ID3D11DeviceContext> context;

    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr,
                                 &context)))
    {
        std::printf("  D3D11CreateDevice(VIDEO_SUPPORT) failed\n\n");
        return;
    }

    Rel<ID3D11VideoDevice> video;
    if (FAILED(device.p->QueryInterface(IID_PPV_ARGS(&video))))
    {
        std::printf("  ID3D11VideoDevice NOT exposed\n\n");
        return;
    }

    auto profiles = video->GetVideoDecoderProfileCount();
    std::printf("  decoder profiles: %u\n", profiles);

    for (UINT i = 0; i < profiles; ++i)
    {
        GUID g {};
        if (SUCCEEDED(video->GetVideoDecoderProfile(i, &g)))
        {
            wchar_t buf[64] {};
            StringFromGUID2(g, buf, 64);
            char narrow[64] {};
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, narrow, 64, nullptr,
                                nullptr);
            std::printf("    %s%s\n", narrow,
                        IsEqualGUID(D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g)
                            ? "  H264_VLD_NOFGT"
                        : IsEqualGUID(D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, g)
                            ? "  HEVC_VLD_MAIN"
                        : IsEqualGUID(D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10, g)
                            ? "  HEVC_VLD_MAIN10" : "");
        }
    }

    // A video processor can exist without a decoder: fixed-function
    // NV12->RGB, scaling and deinterlace. See the note above on why it does
    // not help us.
    Rel<ID3D11VideoProcessorEnumerator> procEnum;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc {};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = 3840;
    desc.InputHeight = 2160;
    desc.OutputWidth = 3840;
    desc.OutputHeight = 2160;
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (SUCCEEDED(video->CreateVideoProcessorEnumerator(&desc, &procEnum)))
    {
        UINT flags = 0;
        auto nv12 = procEnum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12,
                                                        &flags);
        std::printf("  video processor: available (NV12 input %s)\n",
                    (SUCCEEDED(nv12) && (flags
                        & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
                        ? "yes" : "no");
    }
    else
    {
        std::printf("  video processor: NOT available\n");
    }
    std::printf("\n");
}

static void probeMfts(const GUID& subtype, const char* label)
{
    struct Mode { UINT32 flags; const char* name; };
    const Mode modes[] = {
        {MFT_ENUM_FLAG_HARDWARE, "HARDWARE"},
        {MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT, "SOFTWARE"},
    };

    for (auto& mode: modes)
    {
        MFT_REGISTER_TYPE_INFO input {MFMediaType_Video, subtype};
        IMFActivate** activates = nullptr;
        UINT32 count = 0;

        auto hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, mode.flags, &input,
                            nullptr, &activates, &count);
        std::printf("  %-5s %-9s: %u\n", label, mode.name,
                    SUCCEEDED(hr) ? count : 0u);

        if (FAILED(hr))
            continue;

        for (UINT32 i = 0; i < count; ++i)
        {
            LPWSTR name = nullptr;
            UINT32 len = 0;
            if (SUCCEEDED(activates[i]->GetAllocatedString(
                    MFT_FRIENDLY_NAME_Attribute, &name, &len)))
            {
                char narrow[256] {};
                WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, 256, nullptr,
                                    nullptr);
                std::printf("        %s\n", narrow);
                CoTaskMemFree(name);
            }

            // Only a software MFT is likely to expose tunable knobs.
            IMFTransform* transform = nullptr;
            if (SUCCEEDED(activates[i]->ActivateObject(
                    IID_PPV_ARGS(&transform))))
            {
                ICodecAPI* codec = nullptr;
                if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(&codec))))
                {
                    VARIANT v {};
                    VariantInit(&v);
                    if (SUCCEEDED(codec->GetValue(
                            &CODECAPI_AVDecNumWorkerThreads, &v)))
                        std::printf("          workerThreads=%ld\n",
                                    (long) v.lVal);
                    VariantClear(&v);
                    codec->Release();
                }
                else
                {
                    std::printf("          no ICodecAPI\n");
                }
                transform->Release();
            }

            activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }
}

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    SYSTEM_INFO info {};
    GetSystemInfo(&info);
    std::printf("logical processors: %lu\n\n", info.dwNumberOfProcessors);

    probeAdapters();
    probeD3D12();
    probeD3D11();
    std::printf("=== Media Foundation decoder MFTs ===\n");
    probeMfts(MFVideoFormat_H264, "H264");
    probeMfts(MFVideoFormat_HEVC, "HEVC");

    MFShutdown();
    CoUninitialize();
    return 0;
}
```

## What is testable where

| Layer | Runs on a machine with no decoder? |
| --- | --- |
| NV12 through the renderer | Yes — **done** |
| MP4 demux (sample tables → byte ranges) | Yes — **done** (`Video::Mp4Demuxer`) |
| Route A step 1 (D3D manager binding) | Compiles, but selects nothing |
| Route A step 2 (D3D11↔D3D12 interop) | No — needs real decoder textures |
| Bitstream → DXVA picture params, DPB | Writable, not verifiable |
| `ID3D12VideoDecoder` submission | No |

The demuxer parses one video track's sample tables — per-sample byte range,
DTS/PTS, duration, keyframe flag, plus the avcC/hvcC record — over a
`MemoryMappedFile`, tested in `Tests/Video/Mp4DemuxerTests.cpp` against both the
encoder-written synthetic clip and hand-built box structures (including
every-prefix truncation). Edit lists and fragmented MP4 are out of scope. It is
not yet wired into the decode path; Route A does not strictly need it, but it is
what frees the stack from Media Foundation's container support.

## Where MemoryMappedFile fits

`eacp::MemoryMappedFile` is the right tool for the demuxer and for nothing else
in this stack. A demuxer walks scattered sample byte ranges over a large file;
mapping it means no allocation, no copy, and the page cache does the work.

It cannot help the current pipeline, and the arithmetic is not close:

- 4K clip: 673 MB / 634 s = **1.06 MB/s**
- 8K60 clip: 237 MB / 235 s = **1.01 MB/s**

File reading is about 1 MB/s. The pixel path moves multiple GB/s. I/O is roughly
0.02% of the data movement.

## Reference measurements

**These are software decode on an ARM VM. They are a record of what the
optimisation work bought, not a baseline for any other machine.** Take a fresh
baseline on real hardware before comparing anything.

Release (clang-cl), 20 s of playback, `Apps/Video/PlayingHeavyContent` with the
clip path as `argv[1]`. Clips are the 4K and 8K entries in
`Apps/Video/DownloadAndPlay/Clips.json`.

| | before any of this | after buffer + staging pools | after NV12 |
| --- | --- | --- | --- |
| 4K60 decode | 46 fps | 60.1 fps | 60.1 fps |
| 4K60 skipped / 20 s | 110 | 19 | 11–38 |
| 4K60 CPU | 68.9 s | 69.1 s | 48–63 s |
| 8K60 decode | 12.6 fps | 34.5–41.0 fps | 39–44 fps |
| 8K60 render | 47.4 fps | 47.4 fps | 60.0 fps (vsync cap) |
| 8K60 CPU | — | 94.1 s | 72–77 s |
| 8K24 resident | — | 3609 MB | 2422 MB |

Read these carefully before assuming a change helped:

- **NV12 did not raise decode throughput.** It was expected to, on the theory
  that the colour conversion was on the decode thread's critical path. It is not
  — the software HEVC decode itself is the wall. NV12 delivered ~20% less CPU,
  a third less memory, and a render path cheap enough to sit at the vsync cap.
- **Raising `maxQueueBytes` did not help either.** Decode throughput rose 11–19%
  because the decode thread stopped blocking on a one-deep queue, but `starved`
  did not move, because the extra frames arrive too late to be shown. It only
  pays off when decode averages *above* the frame rate and merely jitters.
- **8K run-to-run variance is wide** (38.8–43.8 fps on the same build). Do not
  read a 10% difference as a result.
- Debug builds are ~6x worse for dropped frames at 4K. Always measure Release.

## Things worth knowing

- **Colour matrix is not assumable.** The decoder reads `MF_MT_YUV_MATRIX` and
  `MF_MT_VIDEO_NOMINAL_RANGE`, falling back to height (>576 → BT.709). A
  hardcoded BT.709 decodes standard-definition content visibly wrong — saturated
  green lands 0.155 off, which is what `Decoder/decodesEncodedPixels` catches.
  Any new decode path must carry this through to `FrameInfo`. A hardware MFT
  reports these the same way, but confirm rather than assume.
- **The shader and `Video::toImage` must stay in step.** Both derive their
  constants from `yuvTransformFor()`. No test covers the shader path directly, so
  a change there needs an A/B screenshot against a known-good frame.
- **Zero-copy on Apple already exists** (`VideoFrame::fromNativeBuffer` wrapping a
  `CVPixelBuffer`). Route A step 2 is the Windows equivalent, and
  `UploadMode::ZeroCopy` is already the knob for forcing it.
