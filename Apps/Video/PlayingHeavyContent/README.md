# PlayingHeavyContent

The native twin of [`ServingHeavyContent`](../../WebView/ServingHeavyContent):
the same four heavy clips, the same hover-switched stage, the same self-checking
report — but decoded by the OS and drawn on the GPU, with **no webview and no
native view per video**.

The whole UI is **one `GPU::GPUView`**. Four `Video::Player`s decode
simultaneously and four `Video::FramePresenter`s draw them — the fullscreen
stage, the four hover tiles and all the HUD chrome — inside a **single render
pass on one device**.

The clips aren't committed: CMake **downloads them at configure time** (see
`CMakeLists.txt`), the same four files and hashes the web demo uses, so the two
demos play identical content. Big Buck Bunny and Sintel (CC-BY, &copy; Blender
Foundation) plus the Jellyfish sample from test-videos.co.uk.

Every check prints to **stdout**, so the demo can be read from a terminal
instead of squinted at:

```
[PASS] All clips reach a playable state — 4/4 clips Ready with dimensions and duration
[PASS] A clip auto-plays on launch (no interaction) — active clip heavy.mp4 advanced with no interaction
[PASS] Every clip advances while playing simultaneously — 4/4 clips advanced while playing simultaneously
[PASS] Fast sweep (24 switches @ 60 ms) with no stalls — 24 switches @ 60 ms, 0 stalls
[PASS] A looping clip wraps — loop wrap observed on heavy.mp4
ALL CHECKS PASSED — 24 hover switches, 0 stalls
```

## What it demonstrates

**One GPU view, N videos.** The eventual customer is a glyph background view
that has to composite video *and* shader layers in a single pass on one device.
Stacking a native video view per clip cannot do that — the layers end up in
separate surfaces the compositor owns, not in the app's own pass. So the
composable piece here is not a view at all:

```cpp
Graphics::Rect FramePresenter::draw(Player&, Sprites::SpriteRenderer&,
                                    const Graphics::Rect& dst, Fit, const Graphics::Color&);
```

A presenter draws one player's current frame into any rect of any pass a
`SpriteRenderer` is running. Own several and every video, plus whatever else is
being drawn, lands in the same pass — which is exactly what this demo's
`render()` does, and what `GlyphBackgroundView` will do with it.

**Frames reach the GPU without a copy where the platform allows it.** On macOS
`Player::acquireFramePixelBuffer()` hands back the decoder's `CVPixelBuffer`,
which `Device::wrapPixelBuffer` wraps as a texture — the pixels never leave the
GPU. Windows has no such buffer yet, so `copyLatestFrame()` fills a reused
upload texture instead. The presenter tries the first and falls back to the
second; nothing above it knows which ran.

**Cover crops through the source rect.** `CameraView`'s Cover lets the image
overflow its destination, which is invisible when the camera fills the window
and wrong the moment several videos tile one view — a tile would bleed over its
neighbours. `FramePresenter` computes the centred source sub-rect instead and
draws to exactly `dst`.

**Switching never touches the players.** Every clip opens once, loops, and runs
from launch to quit. Hovering (or the programmatic sweep) only changes which
index is drawn fullscreen — the same discipline the web demo needs to avoid
re-fetching a clip on every hover, arrived at from the other direction: there is
no `src` to swap and no element to remount, so the failure mode simply doesn't
exist natively.

## Run it (macOS)

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF
cmake --build build --target PlayingHeavyContent
open build/Apps/Video/PlayingHeavyContent/PlayingHeavyContent.app
```

The first configure downloads ~25 MB of clips and needs network access.

On launch the stage shows the first clip **immediately, playing** — no click, no
hover. All four clips decode continuously; hovering a tile only changes which
one is on the stage. Tile borders are green once that clip is `Ready`, amber
while it loads; the bar under each tile is its playback position.

The checks then run on their own, without interaction:

- **all four clips reach `Ready`** with dimensions and duration known;
- **the stage clip advances** with nothing touching it — autoplay;
- **all four advance at once**, so the tiles are live rather than frozen
  thumbnails;
- a **sweep of 24 switches at 60 ms** with **0 stalls**. A stall is a render
  where the active player is `Ready` but drew an empty rect, or half a second
  in which `frameSequence()` never advanced while the clip was playing — a
  frozen picture the empty-rect test alone would miss;
- a **loop wrap**: the active clip is seeked near its end and its clock is seen
  going backwards.

### Headless verification

```bash
EACP_DEMO_AUTOQUIT_SECONDS=45 \
EACP_DEMO_SNAPSHOT_PATH=/tmp/stage.png \
  ./build/Apps/Video/PlayingHeavyContent/PlayingHeavyContent.app/Contents/MacOS/PlayingHeavyContent
```

`EACP_DEMO_AUTOQUIT_SECONDS` quits as soon as the checks report (and hard-stops
at the deadline if they never do). `EACP_DEMO_SNAPSHOT_PATH` writes what the
pass actually produced to a PNG through `View::renderToImage` — the checks
measure the path *through* the player, and the snapshot shows the pixels that
came out of it, which is the part a machine with no screen recording permission
cannot otherwise see.

## Windows

`Player-Windows.cpp` is a Media Foundation `IMFSourceReader` backend: RGB32 out,
a decode thread paced by sample timestamps, latest-frame store, loop by
`SetCurrentPosition(0)`. Three details keep it smooth:

- **Hardware-friendly decode.** The reader is created with
  `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING` — the basic
  `ENABLE_VIDEO_PROCESSING` flag would switch hardware-accelerated decoding
  off as a side effect of enabling the RGB32 conversion.
- **Sub-millisecond pacing.** Frames are timed against a high-resolution
  waitable timer; a plain sleep quantises to the ~15.6 ms scheduler tick and
  lands every frame up to a whole refresh late.
- **O(1) frame hand-off.** The decode thread converts each frame outside any
  lock and publishes it by pointer swap, so the render thread's
  `frameSequence()`/`copyLatestFrame()` never wait behind a frame's worth of
  conversion — with four decoders running, `Tests/Video` measures the pull
  path at p95 well under a millisecond.
- **Input never queues behind frames.** GetMessage hands out posted messages
  before hardware input, and a continuous render stream is posted messages —
  so `GPUView`'s tick dispatches queued input itself before each frame
  (standing down inside the OS's modal loops, which own their input). Hover
  reaches the stage in single-digit milliseconds under full render load.
- **A size/move drag freezes the last frame.** The frozen picture scales with
  the window through the composition transform (committed inside the WM_SIZE
  itself), and nothing else runs on the thread the OS drag loop is chasing
  the mouse with — the classic Windows trade of liveness for a drag that
  tracks the hand. Playback and the swapchain rebuild resume on release.

Two caveats against macOS remain:

- **CPU upload, not zero-copy** — there is no shared pixel buffer to wrap yet,
  so frames go through `copyLatestFrame()` and an upload texture.
- **No audio.** The backend plays video only.

The clips are copied next to the executable into `media/` rather than bundled,
and the app falls back to that path when there is no bundle resource.

The demo prints its own presented-frame cadence next to the checks, so the
render path's health is a measured fact rather than a feeling:

```
render cadence: median 8.4 ms (119 fps), p95 10.5 ms, worst 36.9 ms over 339 frames
```

Verified on Windows 11 (ARM64): the checklist reads `ALL CHECKS PASSED`, the
cadence holds the display's native 120 Hz with all four clips decoding, and
the snapshot shows all four live. `Tests/Video` covers the backend directly —
open/metadata, frame delivery, pause/seek/loop/close, four-clip concurrency,
and perf gates on decode throughput, pull latency and frame cadence — against
the same four clips.

## Files

- `Main.cpp` — the whole demo: one `GPUView`, four players, four presenters,
  the HUD, and the check machine.
- `media/*.mp4` — the clips, **fetched by CMake at configure time** via
  `CMake/EacpFetchHeavyClips.cmake` (hash-pinned, git-ignored, shared with
  `Tests/Video`). Not committed.
