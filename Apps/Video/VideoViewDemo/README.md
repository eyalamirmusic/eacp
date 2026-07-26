# VideoViewDemo

Plays a video file in a GPU-composited view. The video twin of
[`CameraViewDemo`](../../Camera/CameraViewDemo), deliberately: same structure,
same overlay hook, same menu pattern — a `Clip` menu switching between four
clips where the camera demo has a `Camera` menu switching between devices.

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF
cmake --build build --target VideoViewDemo
open build/Apps/Video/VideoViewDemo/VideoViewDemo.app
```

The first configure downloads ~25 MB of clips and needs network access. They
are not committed; CMake fetches them hash-pinned into a git-ignored `media/`
shared with `Tests/Video`. Big Buck Bunny and Sintel (CC-BY, &copy; Blender
Foundation), plus the Jellyfish sample from test-videos.co.uk.

## What it shows

The clip plays on launch, looping and muted. The `Clip` menu switches files;
the view stays attached across the switch because it follows the `Player`
object, not the file the player has open. `drawOverlay` draws the progress bar
over the video **in the same render pass** — that is the composition point an
app hangs its own chrome on.

## The bit that matters: frames push, they aren't pulled

`Player` decodes on a thread of its own and announces each frame through
`setFrameArrivedCallback`. `VideoView` renders on that announcement
(`RenderMode::OnFrameArrival`, the default), so it redraws at the clip's frame
rate rather than the display's.

On a 120Hz display playing 30fps content that is 30 renders a second instead of
120 — the other 90 were re-drawing a frame that had not changed. Measured on an
M4 Max, same clip, only the render mode differing:

| render mode | CPU |
| --- | --- |
| `Continuous` (every display refresh) | 8.3% |
| `OnFrameArrival` (default) | 3.7% |

Nothing decodes on the render thread either, so a slow frame cannot stall the
compositor.

`RenderMode::Continuous` is still there for overlays that need to animate at
display rate independently of the video.

### Where the 4% goes

The steady state was profiled to the microsecond (xctrace Time Profiler, every
on-CPU sample attributed). There is no polling, no timer ticking, and no
redundant render left in it:

| consumer | of one core | note |
| --- | --- | --- |
| AVFoundation decode + CoreMedia | ~2.7% | the decoder's own threads; inherent |
| frame acquisition (pacing thread) | ~0.45% | almost all `copyPixelBufferForItemTime` itself |
| render + present, 30/s | ~1.25% | ~410 µs/frame: pass encode, CA commit, drawable |

For scale, measured on the same machine: QuickTime Player playing this same
file used 22.6% CPU, and a browser playing YouTube totalled ~27% across its
helper processes and the out-of-process decoder (`VTDecoderXPCService`) — the
familiar ~1% Activity Monitor row is only the browser's shell process. An app
that composites video inside its own GPU pass pays for decode, one pass encode
and one present per video frame; that floor is where this sits.

### Headless verification

```bash
EACP_DEMO_AUTOQUIT_SECONDS=10 \
  ./build/Apps/Video/VideoViewDemo/VideoViewDemo.app/Contents/MacOS/VideoViewDemo
```

Prints the render count and how many of those renders had a video frame, then
quits. `EACP_DEMO_UPLOAD_MODE=copy` forces the CPU-upload path (the one Windows
uses) so it can be exercised on macOS.

## Beyond one video

`VideoView` is the 90% case. It is built out of `Video::FramePresenter`, which
draws one player's current frame into any rect of any pass a `SpriteRenderer`
is running. Own several and every video — plus whatever else is being drawn —
lands in a **single pass on one device**, which stacking a native video view
per clip cannot do. `Tests/Video` covers four clips decoding at once.

## Windows

`Player-Windows.cpp` is a Media Foundation `IMFSourceReader` backend: RGB32
out, a decode thread paced by sample timestamps, latest-frame store, loop by
`SetCurrentPosition(0)`. It reaches the display path through `copyLatestFrame`
and a reused upload texture rather than the zero-copy wrap macOS gets, and it
plays video only — no audio track, so `setMuted`/`setVolume` are inert there.

The decode thread announces frames through the same `setFrameArrivedCallback`,
so `VideoView` behaves identically on both platforms.
