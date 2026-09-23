# eacp vs PyTorch: Stable Audio 3 inference benchmark

## Methodology

- **Model**: `small-music` (433M DiT + SAME-S codec + T5Gemma text encoder), the only size benchmarked here. `medium` was deliberately skipped: this machine had a real OOM crash earlier from running two 8.6GB-checkpoint inference processes concurrently, and a fair medium benchmark would need eacp and PyTorch loaded at the same time (or careful sequential teardown) to compare load/generate phases without risking a repeat.
- **Prompt**: `"lofi house loop"`, duration 12s, 8 sampling steps, seed 42, `cfg_scale=1.0` (no CFG) on both sides — matching the real `demo_steps=8`/`demo_cfg_scales=[1]` config from the actual checkpoint, used throughout this project.
- **eacp**: `Apps/GPU/StableAudio3/StableAudio3 --model small --prompt "lofi house loop" --seconds 12 --seed 42`, timed externally (`time.perf_counter()` around the subprocess). eacp itself prints `Sampling took Xs`; `load_encode_decode_seconds` = total wall time minus that, i.e. weight loading + prompt tokenize/encode + codec decode + WAV write combined (eacp doesn't print a finer split than that today).
- **PyTorch reference**: `stable_audio_3.model.StableAudioModel.from_pretrained("small-music", device=...)` (timed as "load") then `.generate(prompt=..., duration=12, steps=8, cfg_scale=1.0, seed=42, duration_padding_sec=0.0)` (timed as "generate" — this call does text encoding + diffusion sampling + codec decode all together, no finer split available without instrumenting the library). `duration_padding_sec=0.0` was passed explicitly (default is 6.0s of padding then truncation) so PyTorch generates the same latent length as eacp rather than a padded-then-cropped one.
- Benchmarked both `cpu` and `mps` (Metal Performance Shaders) PyTorch backends — MPS is the fairer "real GPU" comparison; CPU is included for context, not as an equivalent baseline. No CUDA on this machine (Apple Silicon).
- **Hardware**: Apple M1 Pro (14-core GPU, Metal 3), confirmed as the actual device eacp's `Device::shared()` selects (not a software fallback — verified separately via a standalone check printing the real `MTLDevice` name).
- **GPU utilization evidence for eacp**: sampled the non-privileged `ioreg -r -c IOAccelerator -d 1` `"Device Utilization %"` counter every 0.5s in a background thread for the whole eacp subprocess lifetime (load, encode, sample, decode).
- Ran the full comparison **twice** (two independent processes per config, not just repeated calls in one process) to check stability. Raw JSON for both runs is in `Outputs/raw_results.json` and `Outputs/raw_results_run2.json`.

## Results

| Config | Load (s) | Generate/Sample (s) | Total (s) | Run |
|---|---|---|---|---|
| **eacp (small)** | *(not split; see load_encode_decode)* | 7.82 (sampling only) | 23.23 | 1 |
| **eacp (small)** | *(not split)* | 7.96 (sampling only) | 20.41 | 2 |
| eacp — load+encode+decode (everything but sampling) | 15.41 | — | — | 1 |
| eacp — load+encode+decode (everything but sampling) | 12.45 | — | — | 2 |
| **PyTorch CPU** | 5.88 | 3.56 | 9.45 | 1 |
| **PyTorch CPU** | 5.42 | 3.47 | 8.89 | 2 |
| **PyTorch MPS** | 6.04 | 7.47 | 13.51 | 1 |
| **PyTorch MPS** | 5.95 | 1.95 | 7.90 | 2 |

**GPU utilization during eacp's run** (non-privileged `ioreg` counter, sampled every 0.5s): peak **100%**, average **38–43%** across the whole process lifetime (load/idle periods pull the average down; utilization sat at 90–100% specifically during the sampling window and dropped to 0% during weight loading and idle gaps, exactly as expected for a load→compute→load→compute pipeline). This confirms eacp's compute genuinely lands on the GPU, not just that a `MTLDevice` object exists.

## Headline

- **eacp is slower end-to-end than PyTorch on this machine for `small-music`** at this size/duration — roughly 2–2.5x PyTorch-CPU's total time, and 1.5–3x PyTorch-MPS's total time (MPS varied a lot between runs — see caveats).
- eacp's own sampling phase (~7.8–8.0s, stable across runs) is in the same ballpark as PyTorch-MPS's *combined* encode+sample+decode phase (1.95–7.47s) — i.e. eacp's per-step DiT/attention compute itself isn't wildly out of line with PyTorch's GPU backend, but eacp spends much more time than PyTorch does in the load/encode/decode phases surrounding it (12.4–15.4s vs PyTorch's ~6s load).
- The most likely cause, already flagged earlier in this project (not something discovered by this benchmark): every eacp kernel is constructed and Metal-compiled **inline on each call site** rather than as a persistent, reused pipeline object — this was called out as a known, unaddressed performance gap in the original `eacp-ml` foundation work and never revisited. Given eacp's own sampling loop alone dispatches hundreds of kernel calls (20 DiT layers × 8 steps, plus codec/text-encoder kernels), repeated shader compilation is a very plausible explanation for the load/encode/decode-phase gap, though this benchmark did not isolate that specific cause with a controlled experiment — it's the most likely explanation given what's already known, not a proven root cause.

## Caveats

- **Numeric precision differs**: eacp stores/computes primarily in fp32 through its EDSL kernels; PyTorch's `small-music` checkpoint loads as fp32 by default here too (`model_half=False` forced when not on CUDA, confirmed from `model.py`'s `from_pretrained`), so precision is actually matched in this specific comparison — but this is incidental to *this* benchmark's device choice (no CUDA available), not a guarantee across all configurations.
- **PyTorch-MPS timing was unstable between runs** (7.47s → 1.95s generate time) — very likely MPS kernel/graph warm-up (first invocation on a freshly loaded model pays Metal shader compilation cost the same way eacp's per-call approach does every time, but PyTorch/MPS appears to cache and reuse compiled kernels across calls within a process, which a single `generate()` call per process here didn't get to benefit from on the first run). A fairer MPS number would run several `generate()` calls in one warmed-up process; this benchmark did not do that.
- **Only two runs per config** — real timing variance (especially MPS's 3.8x swing) means these numbers should be read as ballpark, not precise.
- **eacp's timing isn't split as finely as PyTorch's** — eacp only exposes a "sampling took Xs" marker; PyTorch's "generate" number bundles text encoding + sampling + decode together, and eacp's "load_encode_decode" number likewise bundles weight loading + prompt encoding + codec decode + WAV write. The two phase-splits aren't perfectly apples-to-apples, only the totals are directly comparable.
- **`medium` not benchmarked** — see Methodology.
