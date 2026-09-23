#!/usr/bin/env python3
import json
import subprocess
import sys
import threading
import time
from pathlib import Path

EACP_REPO = Path("/Users/jamiepond/projects/StableAudioEACP/eacp")
EACP_BINARY = EACP_REPO / "build/Apps/GPU/StableAudio3/StableAudio3"
PYTORCH_REPO = Path("/Users/jamiepond/projects/StableAudioEACP/stable-audio-3")
OUTPUT_DIR = Path(__file__).resolve().parent / "Outputs"

PROMPT = "lofi house loop"
DURATION_SECONDS = 12.0
STEPS = 8
SEED = 42


def sample_gpu_utilization(stop_event, samples):
    while not stop_event.is_set():
        try:
            out = subprocess.run(
                ["ioreg", "-r", "-c", "IOAccelerator", "-d", "1"],
                capture_output=True,
                text=True,
                timeout=2,
            ).stdout
            for line in out.splitlines():
                if '"Device Utilization %"=' in line:
                    marker = '"Device Utilization %"='
                    start = line.index(marker) + len(marker)
                    end = start
                    while end < len(line) and line[end].isdigit():
                        end += 1
                    samples.append(int(line[start:end]))
                    break
        except Exception:
            pass
        time.sleep(0.5)


def run_eacp():
    OUTPUT_DIR.mkdir(exist_ok=True)
    output_path = OUTPUT_DIR / "eacp_small.wav"

    stop_event = threading.Event()
    gpu_samples = []
    sampler_thread = threading.Thread(
        target=sample_gpu_utilization, args=(stop_event, gpu_samples)
    )
    sampler_thread.start()

    start = time.perf_counter()
    proc = subprocess.run(
        [
            str(EACP_BINARY),
            "--model", "small",
            "--prompt", PROMPT,
            "--seconds", str(DURATION_SECONDS),
            "--seed", str(SEED),
            "--output", str(output_path),
        ],
        capture_output=True,
        text=True,
        cwd=str(EACP_REPO),
    )
    total = time.perf_counter() - start

    stop_event.set()
    sampler_thread.join()

    if proc.returncode != 0:
        print("eacp run FAILED", file=sys.stderr)
        print(proc.stdout, file=sys.stderr)
        print(proc.stderr, file=sys.stderr)
        raise SystemExit(1)

    sampling_seconds = None
    for line in proc.stdout.splitlines():
        if line.startswith("Sampling took"):
            sampling_seconds = float(line.split()[2].rstrip("s"))

    return {
        "total_seconds": total,
        "sampling_seconds": sampling_seconds,
        "load_encode_decode_seconds": (
            total - sampling_seconds if sampling_seconds is not None else None
        ),
        "gpu_utilization_samples": gpu_samples,
        "gpu_utilization_peak": max(gpu_samples) if gpu_samples else None,
        "gpu_utilization_avg": (
            sum(gpu_samples) / len(gpu_samples) if gpu_samples else None
        ),
        "stdout": proc.stdout,
        "output_path": str(output_path),
    }


def run_pytorch(device):
    script = f"""
import sys
sys.path.insert(0, {str(PYTORCH_REPO)!r})
import time
import torch
from stable_audio_3.model import StableAudioModel

load_start = time.perf_counter()
model = StableAudioModel.from_pretrained("small-music", device={device!r})
load_seconds = time.perf_counter() - load_start

gen_start = time.perf_counter()
audio = model.generate(
    prompt={PROMPT!r},
    duration={DURATION_SECONDS},
    steps={STEPS},
    cfg_scale=1.0,
    seed={SEED},
    duration_padding_sec=0.0,
    truncate_output_to_duration=True,
)
if {device!r} == "mps":
    torch.mps.synchronize()
gen_seconds = time.perf_counter() - gen_start

import json
print("BENCHMARK_RESULT_JSON:" + json.dumps({{
    "load_seconds": load_seconds,
    "generate_seconds": gen_seconds,
    "audio_shape": list(audio.shape),
}}))
"""
    script_path = OUTPUT_DIR / f"_pytorch_run_{device}.py"
    OUTPUT_DIR.mkdir(exist_ok=True)
    script_path.write_text(script)

    proc = subprocess.run(
        ["uv", "run", "python3", str(script_path)],
        capture_output=True,
        text=True,
        cwd=str(PYTORCH_REPO),
    )

    if proc.returncode != 0:
        print(f"pytorch({device}) run FAILED", file=sys.stderr)
        print(proc.stdout, file=sys.stderr)
        print(proc.stderr, file=sys.stderr)
        return None

    result = None
    for line in proc.stdout.splitlines():
        if line.startswith("BENCHMARK_RESULT_JSON:"):
            result = json.loads(line[len("BENCHMARK_RESULT_JSON:"):])

    if result is None:
        print(f"pytorch({device}) produced no result line", file=sys.stderr)
        print(proc.stdout, file=sys.stderr)
        print(proc.stderr, file=sys.stderr)
        return None

    result["total_seconds"] = result["load_seconds"] + result["generate_seconds"]
    return result


def main():
    results = {}

    print("=== Running eacp (small-music) ===")
    results["eacp"] = run_eacp()
    print(json.dumps(results["eacp"], indent=2, default=str))

    for device in ["cpu", "mps"]:
        print(f"=== Running PyTorch reference ({device}) ===")
        results[f"pytorch_{device}"] = run_pytorch(device)
        print(json.dumps(results[f"pytorch_{device}"], indent=2, default=str))

    OUTPUT_DIR.mkdir(exist_ok=True)
    with open(OUTPUT_DIR / "raw_results.json", "w") as f:
        json.dump(results, f, indent=2, default=str)

    print("\nDone. Raw results in Outputs/raw_results.json")


if __name__ == "__main__":
    main()
