# Stable Audio 3 in eacp: performance against PyTorch

Every number below is tied to one of these two machines. A figure without its
machine is meaningless, so each table says which one it came from.

| | Machine A: MacBook Pro | Machine B: tamby-windows |
|---|---|---|
| GPU | Apple M5 Max, 40-core GPU, Metal 4 | NVIDIA RTX 6000 Ada, 48 GB |
| CPU / RAM | Apple M5 Max, 128 GB unified | see `Benchmark/WINDOWS.md` |
| OS | macOS 26.5.1 | Windows 11, build 26200 |
| eacp backend | Metal | Direct3D 12 (FXC, shader model 5; DXC opt-in) |
| eacp build | Apple clang 21, `-DCMAKE_BUILD_TYPE=Release` | MSVC 19.51, Release |
| PyTorch | 2.7.1 on MPS | 2.7.1+cu126 on CUDA 12.6, driver 528.49 |
| Power | mains, High Power mode | mains |
| Date | 2026-09-23 | 2026-09-23 |

fp32 on both sides unless a row says otherwise, the same pinned checkpoints
(`stabilityai/stable-audio-3-medium` at 27b5a21b, `-small-music` at 0fef1392),
8 sampling steps, no classifier-free guidance. "Warm" is a second generation
in a process that has already loaded and generated once — PyTorch's second
`generate()`, eacp's `--repeat` — the fair steady-state number. "Cold" is one
process from launch to a WAV on disk, what a command-line user gets. Read cold
against cold and warm against warm; mixing them is how decode once looked six
times worse than it is. Full method and raw JSON: `Benchmark/`.

## Machine A (Apple M5 Max, macOS 26.5.1): eacp on Metal against PyTorch on MPS

### Medium model, 30 s clip

Machine A, 2026-09-24, medians of four `benchmark.py` runs with the spread in
brackets (`Benchmark/Outputs/r4-medium-30s-run*.json`). The MacBook was on
mains but charging from a low battery, and PyTorch ran slower than in its
2026-09-23 mains session (warm generate 4.5 s here against 2.5 s then), so
its column may understate it; eacp's settled runs match the day before.

| Phase | eacp Metal | PyTorch MPS warm | eacp is |
|---|---|---|---|
| Load (DiT, codec, text encoder) | 0.45 s (0.34–0.49) | 6.8 s (6.5–11.8) | 15× faster |
| Sampling, 8 steps | 1.75 s (1.36–2.27) | 2.4 s (1.4–3.5) | 1.4× faster |
| Decode | 0.78 s (0.47–1.87) | 2.35 s (2.0–2.9) | 3× faster |
| Generate (encode + sample + decode) | 2.7 s (1.9–4.0) | 4.5 s (3.9–6.4) | 1.7× faster |
| Cold process to WAV on disk | 3.2 s (2.5–4.5) | 16 s (15–25) | 5× faster |
| Peak RSS (footprint) | 0.7 GB (4.6 GB) | 19.4 GB (14.7 GB) | |

The two settled runs of the four (3 and 4) are the ones to read for eacp's
steady state: sampling 1.36–1.41 s, decode 0.47–0.50 s, generate 1.9 s,
total 2.5 s. The weights are not counted in the eacp RSS because they are
never copied: on Metal every checkpoint tensor is a range of one buffer over
the file's own mapping (`SafetensorsFile::loadF32`), so they live in the page
cache, wired for the GPU while the buffer lives. Before that the same run
peaked at 15 GB RSS and 12.1 GB footprint.

### Small model, 12 s clip

Machine A, 2026-09-24, medians of three runs
(`Benchmark/Outputs/r4-small-12s-run*.json`).

| Phase | eacp Metal | PyTorch MPS warm | eacp is |
|---|---|---|---|
| Load | 0.29 s (0.25–0.46) | 3.5 s (3.5–4.3) | 12× faster |
| Sampling | 0.20 s | 0.22 s | level |
| Decode | 0.05 s | 0.10 s | 2× faster |
| Generate | 0.31 s (0.30–0.31) | 0.34 s (0.32–0.35) | 1.1× faster |
| Cold process to WAV | 0.63 s (0.58–0.80) | 6.0 s (5.9–7.1) | 9× faster |
| Peak RSS | 0.7 GB | 5.4 GB | |

## Machine B (NVIDIA RTX 6000 Ada, Windows 11): eacp on D3D12 against PyTorch on CUDA

PyTorch's own default on a CUDA GPU is fp16, so both are given: the fp32
figure is the like-for-like against eacp's precision, and on this card fp32
is the faster of the two for PyTorch. The GPU driver is 528.49 (January 2023),
old for an Ada card; the numbers are worth retaking on a current one.

### Small model, 12 s clip

Machine B.

| Phase | eacp D3D12 | PyTorch CUDA | eacp is |
|---|---|---|---|
| **Generate, warm** | **0.40 s** | 0.48 s fp32, 0.56 s fp16 | **1.2–1.4× faster** |
| Load | 1.25 s | 9.2 s | 7× faster |
| Prompt encoding, cold | 0.18 s | 1.26 s | 7× faster |
| Sampling, cold | 0.36 s | 1.23 s | 3.4× faster |
| Decode, cold | 0.17 s | 0.065 s | 2.6× slower |
| Decode, warm | 0.02 s | 0.022 s | level |
| **Cold process to WAV on disk** | **2.7 s** | 17.4 s fp32, 20.9 s fp16 | **6.3–7.6× faster** |

Medium, 30 s, Machine B: 7.0 s cold process to WAV.

Most of what a first decode pays on Machine B is one-time pipeline creation
and a buffer pool with nothing in it yet, which a warm PyTorch process paid for
during its nine-second load.

## Where it started

On Machine A the same medium clip took 38.6 s end to end on the first
measured run of this branch: 22 s of that was decode building a dense 5491×5491 attention
for a ±17-row window, and the build had no optimisation flags. Everything
between that and the table above is bit-exact — every commit was byte-compared
against the original WAVs — and landed as library affordances in
`Lib/eacp/GPU` and `Lib/eacp/ML` rather than as model code: kernels compiled
once per device and cached on disk, a buffer pool behind `Device::makeBuffer`,
per-dispatch GPU timing, banded and tiled attention, a retiled matmul, tensor
views instead of copies, and an emitter that no longer re-evaluates a value
after a store. The ranked history is `Benchmark/OPTIMIZATION.md`; the D3D12
half, where the same commits turned a broken Windows build into one that is
6–7× faster than PyTorch on CUDA end to end and faster warm-for-warm at
generating, is `Benchmark/WINDOWS.md`.

## Examples

Generated by this CLI on Machine A and encoded to MP3 at 192 kb/s;
`Examples/`:

| File | Model | Seconds | Seed | Prompt |
|---|---|---|---|---|
| `pendulum-rock-dnb-medium-30s.mp3` | medium | 30 | 7 | Pendulum style rock drum and bass, live drums, distorted bass, guitars, 174 bpm, energetic |
| `liquid-dnb-medium-30s.mp3` | medium | 30 | 11 | liquid drum and bass, rolling breakbeat, warm sub bass, atmospheric pads, 174 bpm |
| `jazz-trio-medium-20s.mp3` | medium | 20 | 3 | late night jazz trio, upright bass, brushed drums, rhodes, slow swing |
| `lofi-house-loop-small-12s.mp3` | small | 12 | 42 | lofi house loop |

```bash
build-release/Apps/GPU/StableAudio3/StableAudio3 --model medium --seconds 30 \
    --seed 7 --prompt "Pendulum style rock drum and bass, live drums, distorted bass, guitars, 174 bpm, energetic" \
    --output pendulum.wav
```

A MacBook on battery runs in Low Power Mode and every number comes out about
2× slower; `Benchmark/benchmark.py` refuses to run there and records the power
source with each result. Nothing here was measured with anything else on the
GPU.
