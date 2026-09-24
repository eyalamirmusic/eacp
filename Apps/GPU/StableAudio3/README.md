# StableAudio3

Stable Audio 3 text-to-audio inference on eacp's GPU/ML stack.

```bash
cmake --build build --target StableAudio3
build/Apps/GPU/StableAudio3/StableAudio3 --model small --prompt "lofi house loop" \
      --seconds 8 --seed 42 --output out.wav
```

Flags: `--model small|medium` (default `small`), `--prompt`, `--seconds`,
`--seed`, `--samplerSteps` (default 8), `--output`.

## Checkpoints

Fetched from Hugging Face on first run through `OnlineResource`, at pinned
commits (`Checkpoints.h`):

| `--model` | Repo | Revision | Size |
|---|---|---|---|
| `small` | `stabilityai/stable-audio-3-small-music` | `0fef1392…` | ~3.2 GB |
| `medium` | `stabilityai/stable-audio-3-medium` | `27b5a21b…` | ~9.7 GB |

From each: `model.safetensors`, `t5gemma-b-b-ul2/model.safetensors`,
`t5gemma-b-b-ul2/tokenizer.json`. The codec tests also read
`stabilityai/SAME-L` (`41acf79d…`, `model.safetensors`, ~3.4 GB, not gated),
which the app never fetches.

Both model repos are gated. Accept the terms on the repo page
(`https://huggingface.co/<repo>`) and provide a read token; it is read from
`HF_TOKEN`, then `$HF_HOME/token`, then `~/.cache/huggingface/token` (what
`huggingface-cli login` writes). Without one the fetch fails with HTTP 401.

Files land at
`~/Library/Application Support/StableAudio3/Resources/huggingface/<owner>--<name>/<revision>/<path in repo>`
(`SA3Checkpoints::directory(repo)`), each with a `.resource.json` sidecar.
A pinned commit never changes, so once a file is there the network is not
touched again.

## Tests

The SA3 test targets (`SA3Codec*`, `SA3DiT*`, `SA3Sampler*`,
`SA3TextEncoder*`) read checkpoints from the same place and never download.
Run the app once per model first; for the `SA3CodecSameL*` tests, put SAME-L's
`model.safetensors` in `SA3Checkpoints::directory(SA3Checkpoints::sameL)`.

They are registered with ctest like every other suite (`ctest --test-dir build
-R SA3`), so CI runs them too. A case whose checkpoint is not cached prints
`skipped: checkpoint not cached (<path>)` and passes; the suites that need no
checkpoint (`SA3CodecFastTests`, `SA3CodecPatchedPretransformGoldenTests`,
`SA3DiTUnitTests`, `SA3SamplerFastTests`) run everywhere.
`EACP_REQUIRE_CHECKPOINTS=1` turns every skip into a failure, for a machine
that is meant to have them.
