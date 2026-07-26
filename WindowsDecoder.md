# Windows video decode: where it stands and what is left

The Windows decode backend (`Lib/eacp/Video/Decode/Decoder-Windows.cpp`) runs on
Media Foundation's `IMFSourceReader`. This is the plan for taking it to a full
D3D12 pipeline, plus what was measured getting it to its current state, so the
next person does not have to rediscover it.

## Where it stands

Frames are decoded by Media Foundation into NV12, copied out to a recycled CPU
buffer, uploaded as two textures (`R8Unorm` luma, `RG8Unorm` chroma) and
converted to RGB in the sprite shader. Nothing converts colour on the CPU, and
nothing allocates per frame.

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

## The blocker: no hardware decode in the dev VM

The machine this was developed on is a Parallels ARM VM on Apple Silicon. Its
display adapter exposes **no video decoder at all**, through either API:

```
Adapter: Parallels Display Adapter (WDDM)
=== D3D12 Video ===  ID3D12VideoDevice NOT exposed by this driver
=== D3D11 Video ===  decoder profiles advertised: 0
```

So every measurement below is *software* decode of 4K60 H.264 and 8K60 HEVC.
Before doing any hardware-decode work, re-run this check on the target machine —
if it comes back empty there too, nothing in the next section will help.

```cpp
// Link d3d11 d3d12 dxgi dxguid.
ID3D12Device* device = nullptr;
D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));

ID3D12VideoDevice* video = nullptr;
auto hasD3D12Video = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&video)));

D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT support = {};
support.NodeIndex = 0;
support.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN;
support.Configuration.BitstreamEncryption = D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE;
support.Configuration.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;
support.Width = 7680;
support.Height = 4320;
support.DecodeFormat = DXGI_FORMAT_NV12;
video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT,
                           &support, sizeof(support));
// support.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED

// And the D3D11 side, which is the one Media Foundation actually uses:
//   D3D11CreateDevice(..., D3D11_CREATE_DEVICE_VIDEO_SUPPORT, ...)
//   ID3D11VideoDevice::GetVideoDecoderProfileCount()   // 0 means no decoder
```

## Two routes to hardware decode

D3D12 *does* have a decode API — `ID3D12VideoDevice`, `ID3D12VideoDecoder`,
`D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE`, in `d3d12video.h` since Windows 10 1809.
The catch is that **Media Foundation cannot drive it**.
`MF_SOURCE_READER_D3D_MANAGER` takes an `IMFDXGIDeviceManager`, which is built
from an `ID3D11Device`. MF's video pipeline is D3D11 throughout; there is no
D3D12 equivalent. So the choice is not "which is nicer" but "how much of a codec
front-end are we writing".

### Route A — Media Foundation on D3D11, shared into D3D12

Keep MF. Bind the reader to a D3D11 device and interop the output textures.

1. `D3D11CreateDevice` with `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`.
2. `MFCreateDXGIDeviceManager` + `ResetDevice(d3d11Device)`; set
   `MF_SOURCE_READER_D3D_MANAGER` on the reader attributes.
3. Frames arrive as `IMFDXGIBuffer` wrapping an `ID3D11Texture2D` (NV12, usually
   a slice of a texture array — check `IMFDXGIBuffer::GetSubresourceIndex`).
4. The decoder's textures are not shared, so copy into one created with
   `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYED_MUTEX`,
   then `ID3D12Device::OpenSharedHandle` on the D3D12 side.
5. Synchronise with the keyed mutex or a shared fence.

Still a GPU-side copy, but no CPU roundtrip and no software conversion. The NV12
sampling path this needs **already exists** — that is what `Nv12Shader` and
`VideoView::drawUpload` do today; only the texture source changes.

Note `D3D12Context.h` already records that the 2D layer keeps its own D3D11
device for Direct2D and that the two stacks meet in the compositor, so
cross-device interop is not new ground here.

### Route B — D3D12 Video directly

Pure D3D12, one device, no interop. But `ID3D12VideoDecoder` is a
**bitstream-level** API: per frame it wants the compressed slice data plus
DXVA-style picture parameters (`DXVA_PicParams_HEVC` and friends — roughly a
hundred fields covering POC, the derived reference picture set and DPB state).
Media Foundation produces none of that.

That means writing:

1. **MP4/ISOBMFF demux** — `moov` → `trak` → `mdia` → `minf` → `stbl`, then
   `stsd` (→ `avcC`/`hvcC`), `stts`, `ctts`, `stsc`, `stco`/`co64`, `stsz`,
   `stss`. Yields per frame: byte range, decode and presentation timestamps,
   keyframe flag.
2. **Bitstream parsing** — VPS/SPS/PPS and every slice header, POC computation,
   short/long-term reference picture set derivation, DPB management.
3. **Decode submission** — `ID3D12VideoDecoder` + `ID3D12VideoDecoderHeap` on a
   video-decode queue with its own fence, output into NV12 textures.

Layers 2 and 3 are the bulk of what FFmpeg's hwaccel layer does, and the
reference-management logic is spec-heavy and easy to get subtly wrong. Neither
can be executed on a machine with no decoder, so on hardware like the dev VM they
would be written blind. **Do not start here without a machine that can run it.**

## What is testable where

| Layer | Runs on a machine with no decoder? |
| --- | --- |
| NV12 through the renderer | Yes — **done** |
| MP4 demux (sample tables → byte ranges) | Yes — **done** (`Video::Mp4Demuxer`, `Lib/eacp/Video/Demux`) |
| Bitstream → DXVA picture params, DPB | No |
| `ID3D12VideoDecoder` submission | No |
| Route A's D3D11↔D3D12 interop | No (needs a real decoder to produce textures) |

The demuxer is now built: `Video::Mp4Demuxer` (`Lib/eacp/Video/Demux`) parses
one video track's sample tables — per-sample byte range, DTS/PTS, duration,
keyframe flag, plus the avcC/hvcC record — over a `MemoryMappedFile`, tested in
`Tests/Video/Mp4DemuxerTests.cpp` against both the encoder-written synthetic
clip and hand-built box structures (including every-prefix truncation). Edit
lists and fragmented MP4 are out of scope; both routes need what it produces
eventually (Route A does not strictly need it, but a demuxer is what frees the
stack from Media Foundation's container support).

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
  Any new decode path must carry this through to `FrameInfo`.
- **The shader and `Video::toImage` must stay in step.** Both derive their
  constants from `yuvTransformFor()`. No test covers the shader path directly, so
  a change there needs an A/B screenshot against a known-good frame.
- `Decoder::open` calls `CoInitializeEx` with `COINIT_APARTMENTTHREADED` on the
  calling thread, while `FrameStream` drives `nextFrame`/`seek` from its own
  decode thread, which never initialises COM. It works — the source reader is
  agile enough in practice — but MF generally wants MTA, and this is the first
  thing to suspect if a new backend hangs or fails cross-thread.
- **Zero-copy on Apple already exists** (`VideoFrame::fromNativeBuffer` wrapping a
  `CVPixelBuffer`). Route A is the Windows equivalent, and `UploadMode::ZeroCopy`
  is already the knob for forcing it.
