# Windows video decode: where it stands and what is left

The Windows decode backend (`Lib/eacp/Video/Decode/Decoder-Windows.cpp`) runs on
Media Foundation's `IMFSourceReader`. This is the record of taking it to hardware
decode, plus what was measured on the way, so the next person does not have to
rediscover it.

**Route A step 1 is done and hardware decode is live.** On a machine with a
video decoder the source reader is now bound to a D3D11 device and the decode
runs on the GPU. Step 2 (zero-copy) is still unwritten, and the step 1 numbers
say to leave it that way for now.

**If you only want the outcome, read "Route A step 1: the result".** The
sections before it are how the decision was reached; the ones after it are the
probe source and the older software-decode history.

## Where it stands

Frames are decoded into NV12, copied out to a recycled CPU buffer, uploaded as
two textures (`R8Unorm` luma, `RG8Unorm` chroma) and converted to RGB in the
sprite shader. Nothing converts colour on the CPU, and nothing allocates per
frame.

**Decode runs on the GPU's video engine where the machine has one, and in
software where it does not.** `createVideoDevice()` asks for a D3D11 device with
`D3D11_CREATE_DEVICE_VIDEO_SUPPORT` and requires it to report at least one
decoder profile; a null result binds no device manager and the reader decodes
exactly as it did before. The frames come back through `Lock2D` as CPU pixels
either way, so nothing downstream of the decoder knows which happened.

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
- **Route A step 1: the D3D11 device manager.** Hardware decode. Cut 8K60 CPU
  from 122.6 s to 11.7 s over a 20 s window and took `starved` from 902 to 2.

## The blocker on the dev VM: no hardware decode at all

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

Two things in that output are worth keeping:

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

An earlier draft of this document also called the MFT enumeration "the
conclusive test" and claimed a zero `MFT_ENUM_FLAG_HARDWARE` count means no
device-manager plumbing can produce hardware decode. **That is wrong**, and the
real-hardware probe below disproves it — see "What the MFT count does and does
not tell you". The conclusion held on this VM, but it held because the D3D11
decoder-profile count was zero, not because the MFT count was.

## Real hardware: the same probe on an RTX 5070 Ti

AMD Ryzen (16 logical processors), Windows 11, NVIDIA driver 32.0.15.9571.
Probe output, 2026-07-26, abridged where it repeats:

```
logical processors: 16

=== DXGI adapters ===
  [0] NVIDIA GeForce RTX 5070 Ti      vendor=10DE device=2C05
  [1] Microsoft Basic Render Driver   vendor=1414 device=008C (SOFTWARE)

=== D3D12 video ===
  H264 4K  : SUPPORTED
  HEVC 4K  : SUPPORTED
  HEVC 8K  : SUPPORTED

=== D3D11 video ===
  decoder profiles: 34
    {1B81BE68-...}  H264_VLD_NOFGT
    {5B11D51B-...}  HEVC_VLD_MAIN
    {107AF0E0-...}  HEVC_VLD_MAIN10
    (31 others)
  video processor: available (NV12 input yes)

=== Media Foundation decoder MFTs ===
  H264  HARDWARE : 0
  H264  SOFTWARE : 1   Microsoft H264 Video Decoder MFT   workerThreads=-1
  HEVC  HARDWARE : 0
  HEVC  SOFTWARE : 1   HEVCVideoExtension                 no ICodecAPI
```

### What the MFT count does and does not tell you

Read the last two blocks together. This machine has **34 D3D11 decoder profiles
including H.264 and HEVC Main/Main10, and zero hardware decoder MFTs** — the
same `HARDWARE : 0` lines as the VM with none of the same meaning. Binding the
D3D11 device manager to this machine produced full hardware decode anyway.

`MFT_ENUM_FLAG_HARDWARE` counts *vendor-registered* MFTs. NVIDIA registers none.
Hardware decode instead reaches the GPU through DXVA2: `Microsoft H264 Video
Decoder MFT` and `HEVCVideoExtension` are Microsoft's own MFTs, and given a
`MF_SOURCE_READER_D3D_MANAGER` they route the decode to the driver rather than
running it on the CPU. The same MFT name appears in the probe whether it will
decode in hardware or in software; the name is not the answer. Intel is the case
that misleads in the other direction, registering a Quick Sync MFT that does
show up under `HARDWARE`.

So, to ask "will this machine decode in hardware":

- **`ID3D11VideoDevice::GetVideoDecoderProfileCount() > 0` is the test**, and it
  is the one `createVideoDevice()` uses. Zero means software, on both machines
  here, and correctly.
- **`MFT_ENUM_FLAG_HARDWARE` answers a narrower question** — whether a vendor
  shipped an MFT — and a zero there rules nothing out.
- **A non-empty profile list is still not per-clip.** Profiles are enumerated as
  GUIDs, so H.264 present says nothing about HEVC Main10 at 8K. `open()` binds
  the manager and lets Media Foundation pick; if no hardware path fits the clip,
  MF falls back to a software MFT on its own and playback is unaffected.

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
**Done** — `createVideoDevice()` and `bindVideoDevice()` in `Decoder-Windows.cpp`.

1. `D3D11CreateDevice` with `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`.
2. Set `ID3D10Multithread::SetMultithreadProtected(TRUE)` on the device — MF
   drives it from its own threads.
3. `MFCreateDXGIDeviceManager` + `ResetDevice(d3d11Device)`.
4. Set `MF_SOURCE_READER_D3D_MANAGER` on the reader attributes before
   `MFCreateSourceReaderFromURL`.
5. Leave the existing CPU readback and NV12 upload path **exactly as they are**.

Contained, mostly attribute-setting, and it fails safely: no video device means
no manager and the current path is untouched. It captured the large win, because
hardware versus software decode dwarfs everything in the measurement tables
below.

**Step 2 — D3D11↔D3D12 interop**, to remove the GPU→CPU→GPU roundtrip.
**Not done, and measurement says do not start it yet** — see "Route A step 1:
the result". Kept here because the reasoning stays valid for weaker GPUs.

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

## Route A step 1: the result

Measured on the RTX 5070 Ti above. Release (clang-cl), 20 s windows,
`PlayingHeavyContent`. Same binary, same clips, same session; the only
difference is the device manager.

| | software (before) | hardware (after) |
| --- | --- | --- |
| 4K60 decode | 60.1 fps | 60.0 fps |
| 4K60 skipped / starved | 1 / 2 | 2 / 3 |
| 4K60 CPU | 45.0 s | **6.5 s** |
| 8K60 decode | 51.7 fps | **60.1 fps** |
| 8K60 render | 45.1 fps | **59.9 fps** |
| 8K60 skipped / starved | 176 / 902 | **1 / 2** |
| 8K60 CPU | 122.6 s | **11.7 s** |
| 8K24 decode | 24.2 fps | 24.1 fps |
| 8K24 skipped / starved | 3 / 23 | **0 / 0** |
| 8K24 CPU | 74.4 s | **7.7 s** |

Reading these:

- **8K60 was the only clip that did not play, and now it does.** Software decode
  managed 51.7 of the needed 60 fps and dragged the render loop down to 45.1 fps
  with it. Note what `starved` at 902 means against a render loop drawing ~902
  frames in that window: essentially *every* frame the renderer drew was one it
  had already drawn. Hardware decode holds 60.1 fps with `starved` at 2.
- **4K60 and 8K24 already played in software on this CPU**, at 16 threads. Their
  win is not throughput, it is the 7–10x CPU drop: 45.0 → 6.5 s and 74.4 → 7.7 s.
  A frame rate at the vsync cap hid a machine spending two to four cores on
  decode, which on a laptop is battery and fan and every other thread's cache.
- **Decode fps cannot exceed the clip's rate** — `FrameStream` backpressures at
  `queueDepth`, so 60.0 means "kept up", not "the ceiling". These numbers say
  nothing about headroom, and were not asked to.
- **Resident bytes barely moved** (8K60 482 → 747 MB) and are noise here: the
  queue depth, not the decoder, sets that, and it varies with where playback
  happens to be when the sampler looks.

### Verifying hardware decode actually engaged

Frame rate alone will not tell you — 4K60 read 60 fps both ways. Two checks that
will, neither needing a debugger:

- **`Get-Counter "\GPU Engine(*engtype_VideoDecode)\Utilization Percentage"`**
  while the clip plays, filtered to the process's `pid_<n>_` instances. This read
  37.8% during 8K60 and is the direct evidence.
- **CPU seconds.** A 10x drop is not something a code path change produces.

Enumerating **all** engine instances for the process is also how the
`MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING` worry below was settled.

### Decision on step 2: not now

The plan expected a discrete GPU to "solve 4K outright and still want step 2 at
8K", on the reasoning that 8K NV12 readback is ~3 GB/s over PCIe. **8K60 plays at
the vsync cap with `starved` at 2 and 11.7 s of CPU.** There is no symptom left
for step 2 to cure, so the risky half stays unwritten — which is the whole point
of having staged it.

What would change that verdict:

- A GPU whose decoder outruns its readback more than this one does, or a narrower
  PCIe link.
- Content past 8K60, or several 8K streams composited at once.
- Caring about the ~0.6 cores 8K60 still costs — most of which is the readback
  and the NV12 copy, which is exactly what step 2 removes.

The UMA-versus-discrete split above was aimed at the wrong axis: readback did not
turn out to be a bottleneck on a *discrete* card, which is the case it predicted
would need step 2 most. Modern PCIe has more headroom than the 3 GB/s estimate
assumed it would strain.

### What was expected to be trouble, and was not

- **COM apartment model.** Flagged as the first thing to suspect, and the likely
  prerequisite. It was neither. `nextFrame`/`seek` were driven from a decode
  thread with no COM apartment at all, hardware decode and D3D manager included,
  and it worked — tested deliberately by removing the apartment call and
  re-running. `joinComApartment()` is in the file anyway, because the old code
  had a genuine defect unrelated to hardware decode: it called `CoInitializeEx`
  on whichever thread ran `open()` and `CoUninitialize` from the destructor,
  which is not required to be the same thread. A `thread_local` pairs them
  correctly by construction. Treat it as contract-correctness, not as the fix.
- **`MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING`.** It is still set, and
  with the D3D manager bound it inserts nothing: the process has **no
  `videoprocessing` GPU engine instance at all** while playing, only `3d` (our
  own upload and sprite shader, ~25%) and `videodecode` (~38%). Asking for NV12
  when the decoder already produces NV12 leaves the flag with no work to do, as
  its comment in the source claims. Left alone.
- **Async MFTs.** As predicted, `IMFSourceReader` absorbs them and the
  synchronous `ReadSample` loop was untouched.

The one thing that did need care was ordering in the destructor: the reader holds
the device manager, which holds the D3D11 device, and all three have to be
released before `MFShutdown`.

### Reproducing the measurements

`PlayingHeavyContent` takes an optional second argument:

```
PlayingHeavyContent <clip> <seconds>
```

which plays for that long, logs one summary line and quits. It is a
GUI-subsystem binary with no console, so redirect stdout to a file to read it.
The line reports only the window: the stream's counters run from `open()`, and
the frames decoded while the window was still being created are subtracted out.

Without this the numbers have to be read off the HUD by eye, which is neither
repeatable nor loggable — the before-and-after pairs above are the same clip,
same binary, same session, and that is only affordable because it is one command
each. Resident bytes and CPU seconds are not in the line; take those from the
process (`TotalProcessorTime`, and poll `WorkingSet64` — the peak is not
readable once the process has exited).

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
| Route A step 1 (D3D manager binding) | Runs, selects nothing — **done** |
| Route A step 2 (D3D11↔D3D12 interop) | No — needs real decoder textures |
| Bitstream → DXVA picture params, DPB | Writable, not verifiable |
| `ID3D12VideoDecoder` submission | No |

Step 1 turned out to be verifiable on hardware without any test of its own:
`Decoder/decodesEncodedPixels` in `Tests/Video` encodes a synthetic clip, decodes
it through `makeDecoder()` and compares actual pixel values, so it now exercises
the D3D-bound reader and its colour handling on any machine with a decoder, and
the software path on any machine without one. It passes on both.

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

## Reference measurements (ARM VM, software only)

**These are software decode on the Parallels ARM VM, and predate hardware
decode. They are a record of what the CPU-side optimisation work bought, not a
baseline for any other machine** — the x86 hardware numbers are in "Route A step
1: the result". Take a fresh baseline on whatever machine you are on before
comparing anything.

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
  Any new decode path must carry this through to `FrameInfo`. Confirmed to hold
  with the D3D manager bound: the media-type attributes are read the same way and
  `decodesEncodedPixels` passes against hardware decode unchanged.
- **The shader and `Video::toImage` must stay in step.** Both derive their
  constants from `yuvTransformFor()`. No test covers the shader path directly, so
  a change there needs an A/B screenshot against a known-good frame.
- **Zero-copy on Apple already exists** (`VideoFrame::fromNativeBuffer` wrapping a
  `CVPixelBuffer`). Route A step 2 is the Windows equivalent, and
  `UploadMode::ZeroCopy` is already the knob for forcing it.
- **`nativeBufferToImage` on Windows still returns `{}`.** Frames from this
  backend are always CPU pixels, even with hardware decode: the reader hands back
  a DXGI-backed buffer and `Lock2D` reads it down. The function grows a body only
  with step 2, when a frame starts carrying a texture instead.
