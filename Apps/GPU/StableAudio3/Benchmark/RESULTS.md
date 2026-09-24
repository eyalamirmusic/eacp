# eacp vs PyTorch: Stable Audio 3 inference

> Superseded on 2026-09-24 by `../PERFORMANCE.md`, which carries the
> current medians (`Outputs/r4-*.json`, taken after zero-copy loading and
> the rest of the optimisation work). What follows is the 2026-09-23
> baseline measured before any of it.

Apple M5 Max (40-core GPU), 128 GB, macOS 26.5.1. eacp on Metal, PyTorch 2.7.1
on MPS. 2026-09-23.

**Headline (medium, 30 s, Release build):** eacp is **5.9× slower** than
PyTorch-MPS warm at generating (prompt encode + sampling + decode: 15.2 s vs
2.6 s): **2.3× slower at sampling** (3.2 s vs 1.35 s) and **9.9× slower at
decoding** (12.0 s vs 1.2 s). It is **7× faster at loading** (1.3 s vs 9.5 s),
which is why it is only 1.5× slower end to end from a cold process (18.3 s vs
PyTorch's 9.5 s load + 2.8 s first generate).

Decoding is where eacp loses most of its time: ~10.7 s of the ~12.6 s gap to
PyTorch's warm generate, with the GPU ~98% busy the whole time, so it is doing
far more GPU work than PyTorch's decode, not waiting on the host.

## Results

Medians of four runs per side (ranges in brackets). Seconds.

### medium, 30 s, 8 steps, seed 7

Prompt: "Pendulum style rock drum and bass, live drums, distorted bass, guitars,
174 bpm, energetic".

| Phase | eacp Release | PyTorch MPS cold | PyTorch MPS warm | eacp vs warm |
|---|---|---|---|---|
| Load (weights, codec, text encoder) | 1.33 (1.20–11.6) | 9.51 (9.06–10.25) | — | 7.2× faster |
| Prompt encoding | 0.05 (0.05–0.13) | 0.24 (0.20–0.25) | 0.02 | 2.5× slower |
| Sampling (8 steps) | 3.17 (2.89–4.73) | 1.30 (1.22–1.40) | 1.35 (1.13–1.93) | 2.3× slower |
| Decoding | 11.96 (11.21–16.32) | 1.30 (1.20–2.01) | 1.21 (1.03–1.59) | 9.9× slower |
| Generate (encode + sample + decode) | 15.18 | 2.83 (2.64–3.61) | 2.57 (2.18–3.55) | 5.9× slower |
| Process total | 18.34 (15.83–28.96) | 12.3 (load + cold generate) | — | 1.5× slower |

eacp's run with an 11.6 s load had its checkpoint evicted from the page cache
by the PyTorch run before it; the other three loaded in 1.2–1.4 s.

### small (small-music), 12 s, 8 steps, seed 42, "lofi house loop"

| Phase | eacp Release | PyTorch MPS cold | PyTorch MPS warm | eacp vs warm |
|---|---|---|---|---|
| Load | 1.75 (1.37–2.64) | 4.54 (4.30–4.85) | — | 2.6× faster |
| Prompt encoding | 0.07 (0.05–0.08) | 0.23 (0.21–0.26) | 0.02 | 3× slower |
| Sampling | 1.07 (0.88–1.35) | 0.38 (0.36–0.40) | 0.22 (0.21–0.22) | 5.0× slower |
| Decoding | 1.03 (0.86–1.76) | 0.17 (0.15–0.21) | 0.09 | 11× slower |
| Generate | 2.17 | 0.79 (0.73–0.84) | 0.33 (0.32–0.33) | 6.6× slower |
| Process total | 4.19 (3.51–5.04) | 5.3 (load + cold generate) | — | 1.3× faster |

### Memory

| | eacp medium | PyTorch medium | eacp small | PyTorch small |
|---|---|---|---|---|
| Peak RSS (GB) | 19.26 | 19.36 | 5.49 | 5.45 |
| Peak memory footprint (GB) | 17.7–20.0 | 14.7 | 4.8 | 4.9 |
| GPU "In use system memory", peak over idle (GB) | 12.6–13.2 | 11.5–12.7 | 3.1–3.8 | 3.3–3.8 |

RSS is the same on both sides. eacp's footprint (which counts Metal
allocations; RSS does not see all of them) runs 3–5 GB above PyTorch's on
medium.

### GPU utilization (ioreg "Device Utilization %", 0.25 s samples)

| Phase | eacp medium | PyTorch medium warm | eacp small | PyTorch small warm |
|---|---|---|---|---|
| Sampling | 77–96% | 99–100% | 68–81% | 31–98%\* |
| Decoding | 87–99% | 99%† | 29–55% | —\* |
| Load | — | 0–5% | — | 0–2% |

\* PyTorch's small warm phases last 0.1–0.2 s, one sample or none, so these
are noise. † One medium run caught 9% in a 1 s decode window; the other three
were 99%.

eacp's small decode runs the GPU at roughly half utilization: at 12 s it is
dispatch- or sync-bound, while at 30 s the GPU work itself dominates. eacp's
medium sampling sits a little under PyTorch's saturation.

### Other builds and backends (one quiet run each)

| medium, 30 s | Load | Text encoder load | Sampling | Decoding | Total |
|---|---|---|---|---|---|
| eacp, `build/` (no `CMAKE_BUILD_TYPE`, no `-O`) | 4.63 | 3.59 | 6.60 | 15.55 | 27.10 |
| eacp Release (median above) | 1.33 | ~0.37 | 3.17 | 11.96 | 18.34 |
| PyTorch CPU, cold generate | 8.90 | — | 7.52 | 20.84 | 37.6 |

| small, 12 s | Load | Sampling | Decoding | Total |
|---|---|---|---|---|
| eacp, `build/` (unoptimized) | 5.27 | 4.18 | 4.32 | 14.24 |
| eacp Release (median above) | 1.75 | 1.07 | 1.03 | 4.19 |

The unoptimized build is 1.5× (medium) to 3.4× (small) slower overall.
Most of that is host code: the T5Gemma load goes from ~0.4 s to 3.6–4 s, and
sampling doubles or worse, which points to per-dispatch CPU work. Use
`build-release` for any number worth quoting. PyTorch on CPU finished medium in
38 s, slower than eacp Release in every phase.

## Methodology

- `benchmark.py` runs eacp, then PyTorch, one after the other and never at the
  same time. Each side runs as its own process under `/usr/bin/time -l`, which
  gives "maximum resident set size" and "peak memory footprint". A background
  thread polls `ioreg -r -c IOAccelerator -d 1` every 0.25 s for "Device
  Utilization %" and "In use system memory"; memory is reported as the peak
  minus the idle reading taken just before the run.
- **eacp**: `build-release/Apps/GPU/StableAudio3/StableAudio3` (Release,
  `EACP_UNITY_BUILD=OFF`). `Main.cpp` prints "`<phase> took Xs`" for DiT
  weights, codec, text encoder, prompt encoding, sampling, decoding, WAV write
  and total. `CommandBuffer::commit()` blocks until the GPU finishes, so each
  phase's time includes its GPU work. "Load" is DiT + codec + text encoder.
  Checkpoints come from the app's own HF cache (not re-downloaded). The CLI
  loads fresh every time and has no warm path, so each eacp run is one cold
  process (the OS Metal shader cache stays warm between runs).
- **PyTorch**: Stability-AI/stable-audio-3 at
  `779434a908193105335fd8d833418603625b2859`, `uv sync` (torch 2.7.1).
  `StableAudioModel.from_pretrained(name, device="mps")` is timed as load.
  Then `generate(prompt, duration, steps=8, cfg_scale=1.0, seed,
  duration_padding_sec=0.0, truncate_output_to_duration=True)` runs twice in
  the same process: the first call is "cold", the second "warm". The
  `generate` signature at this commit still takes all of these. To split
  generate into phases without editing the library, the child wraps
  `model.model.conditioner.forward` (prompt encoding) and
  `model.model.pretransform.decode` (decoding) with `torch.mps.synchronize()`
  before and after. Sampling is generate minus those two, so it also includes
  generate's setup (noise, masks, schedule). This split is coarser than eacp's.
- **Checkpoints**: PyTorch downloads from `main`. It resolved small-music to
  `0fef1392cd842149a2b6d445e181c97608faac06` and medium to
  `27b5a21b791b1b033d193a9e1e3ce78493f102f9`, the same revisions eacp pins in
  `Checkpoints.h`. Both sides run the same weights.
- **Precision**: fp32 on both sides. `from_pretrained` forces
  `model_half=False` whenever CUDA is unavailable, and the loaded DiT's dtype
  is `torch.float32` on MPS and CPU.
- **Outputs**: all runs produce real, non-silent audio of the right length
  (eacp 30.00 s / 12.00 s, 16-bit; PyTorch float WAV). The waveforms differ
  because the two sides draw their initial noise from different RNGs. Timing
  depends on length, not content.
- Raw JSON per run: `Outputs/{medium-30s,small-12s}-run{1..4}.json`,
  `*-nobuildtype.json`, `medium-30s-cpu.json`.

## Caveats

- **This machine was shared, and the first two passes were thrown away.** In
  pass one, other processes (a running Tamber app and browser video) held the GPU
  at ~40% busy. In pass two, a concurrent cold build and clangd used
  1100–1300% CPU. Those passes ran up to 2–3× slower on both sides (eacp medium
  decode 26–30 s, PyTorch warm sampling 2.7–4.6 s). The runs reported here
  started only once the GPU read under 10% and no process used more than 150%
  CPU. Even so, eacp's medium total ranges from 15.8 to 29 s, so read these as
  ±20% numbers, not precise ones.
- PyTorch's warm number is the fair comparison for a long-lived process such
  as a plugin or a server. eacp has no warm path, so its sampling and decode
  times are single-call numbers. They look stable across runs, which suggests
  pipeline compilation is not what is slowing them down.
- The ioreg utilization counter says the GPU was busy, not how efficiently it
  was used. At 0.25 s sampling it can say little about PyTorch's sub-second
  phases.
- Load times depend on the page cache. Every quoted run had its checkpoint
  cached except eacp medium run 1 (11.6 s load).
