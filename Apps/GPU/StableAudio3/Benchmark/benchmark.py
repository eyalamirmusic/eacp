#!/usr/bin/env python3
"""eacp vs PyTorch: Stable Audio 3 inference, per phase, with memory and GPU use.

Runs the eacp StableAudio3 binary and Stability's PyTorch reference one after
the other (never at the same time) with the same prompt, seed, length and step
count, and writes the raw numbers to Outputs/<label>.json.

    python3 benchmark.py --pytorch-repo ~/stable-audio-3 --model medium \\
        --seconds 30 --seed 7 --prompt "..." --label medium-run1
"""
import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

BENCHMARK_DIR = Path(__file__).resolve().parent
EACP_REPO = BENCHMARK_DIR.parents[3]
OUTPUT_DIR = BENCHMARK_DIR / "Outputs"
DEFAULT_EACP_BINARY = EACP_REPO / (
    "build-release/Apps/GPU/StableAudio3/StableAudio3"
    + (".exe" if sys.platform == "win32" else ""))
PYTORCH_MODEL_NAMES = {"small": "small-music", "medium": "medium"}

EACP_PHASES = {
    "DiT weights": "dit_weights",
    "Decoder": "codec",
    "Waiting for kernels": "kernel_wait",
    "Text encoder": "text_encoder",
    "Prompt encoding": "prompt_encoding",
    "Sampling": "sampling",
    "Decoding": "decoding",
    "WAV write": "wav_write",
    "Total": "total",
}


class GpuSampler:
    """Polls the GPU's own utilisation counter in a background thread.

    ioreg on macOS, nvidia-smi where there is an NVIDIA card. Both answer the
    same two questions - how busy the GPU is, and how much of its memory is in
    use - so the rest of the script does not care which one replied.
    """

    def __init__(self, interval=0.25):
        self.interval = interval
        self.samples = []
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self.run)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop_event.set()
        self.thread.join()

    @staticmethod
    def read():
        if sys.platform == "darwin":
            return GpuSampler.read_ioreg()

        return GpuSampler.read_nvidia_smi()

    @staticmethod
    def read_ioreg():
        out = subprocess.run(
            ["ioreg", "-r", "-c", "IOAccelerator", "-d", "1"],
            capture_output=True,
            text=True,
            timeout=2,
        ).stdout
        utilization = re.search(r'"Device Utilization %"=(\d+)', out)
        in_use = re.search(r'"In use system memory"=(\d+)', out)
        return (
            int(utilization.group(1)) if utilization else None,
            int(in_use.group(1)) if in_use else None,
        )

    @staticmethod
    def read_nvidia_smi():
        try:
            out = subprocess.run(
                ["nvidia-smi",
                 "--query-gpu=utilization.gpu,memory.used",
                 "--format=csv,noheader,nounits"],
                capture_output=True,
                text=True,
                timeout=2,
            ).stdout.strip().splitlines()
        except (OSError, subprocess.SubprocessError):
            return (None, None)

        if not out:
            return (None, None)

        fields = [field.strip() for field in out[0].split(",")]

        if len(fields) < 2:
            return (None, None)

        # Megabytes, reported as bytes, so both readers answer in the same unit.
        return (int(fields[0]), int(fields[1]) * 1024 * 1024)

    def run(self):
        while not self.stop_event.is_set():
            try:
                utilization, in_use = self.read()
                self.samples.append((time.time(), utilization, in_use))
            except Exception:
                pass
            time.sleep(self.interval)

    def summarize(self, start, end, baseline_in_use):
        window = [s for s in self.samples if start <= s[0] <= end]
        utilization = [s[1] for s in window if s[1] is not None]
        in_use = [s[2] for s in window if s[2] is not None]
        return {
            "samples": len(window),
            "gpu_utilization_avg": (
                round(sum(utilization) / len(utilization), 1) if utilization else None
            ),
            "gpu_utilization_peak": max(utilization) if utilization else None,
            "gpu_in_use_memory_peak_delta_gb": (
                round((max(in_use) - baseline_in_use) / 1e9, 2) if in_use else None
            ),
        }


def run_timed(command, cwd=None, timeout=None):
    """Runs command and returns (proc, rusage).

    /usr/bin/time -l is where the memory figures come from, and it is macOS's;
    elsewhere the command is run directly and the memory keys come back None,
    which every reader of them already handles. The timings are the script's
    own either way.
    """
    measured = ["/usr/bin/time", "-l", *command] if sys.platform == "darwin"         else list(command)

    proc = subprocess.run(
        measured,
        capture_output=True,
        text=True,
        cwd=cwd,
        timeout=timeout,
    )
    rusage = {}
    for key, pattern in [
        ("peak_rss_gb", r"(\d+)\s+maximum resident set size"),
        ("peak_footprint_gb", r"(\d+)\s+peak memory footprint"),
    ]:
        match = re.search(pattern, proc.stderr)
        rusage[key] = round(int(match.group(1)) / 1e9, 2) if match else None
    return proc, rusage


def fail(name, proc):
    print(f"{name} FAILED ({proc.returncode})", file=sys.stderr)
    print(proc.stdout[-4000:], file=sys.stderr)
    print(proc.stderr[-4000:], file=sys.stderr)
    raise SystemExit(1)


def run_eacp(args):
    output_path = OUTPUT_DIR / f"eacp_{args.label}.wav"
    command = [
        str(args.eacp_binary),
        "--model", args.model,
        "--prompt", args.prompt,
        "--seconds", str(args.seconds),
        "--seed", str(args.seed),
        "--output", str(output_path),
    ]

    baseline_in_use = GpuSampler.read()[1]
    with GpuSampler() as sampler:
        start = time.time()
        proc, rusage = run_timed(command)
        end = time.time()

    if proc.returncode != 0:
        fail("eacp", proc)

    phases = {}
    for line in proc.stdout.splitlines():
        match = re.match(r"(.+) took ([\d.]+)s$", line)
        if match and match.group(1) in EACP_PHASES:
            phases[EACP_PHASES[match.group(1)]] = float(match.group(2))

    # eacp's stdout is buffered, so phase windows are placed by walking the
    # printed durations back from the process exit.
    windows = {}
    cursor = end
    for name in ["wav_write", "decoding", "sampling"]:
        windows[name] = (cursor - phases[name], cursor)
        cursor -= phases[name]

    return {
        "wall_seconds": end - start,
        "phases": phases,
        "load_seconds": phases["dit_weights"] + phases["codec"] + phases["text_encoder"],
        **rusage,
        "gpu_whole_process": sampler.summarize(start, end, baseline_in_use),
        "gpu_sampling": sampler.summarize(*windows["sampling"], baseline_in_use),
        "gpu_decoding": sampler.summarize(*windows["decoding"], baseline_in_use),
        "output_path": str(output_path),
        "stdout": proc.stdout,
    }


PYTORCH_CHILD = r'''
import json, sys, time
import torch
import torchaudio
from stable_audio_3.model import StableAudioModel

config = json.loads(sys.argv[1])
device = config["device"]

def sync():
    if device == "mps":
        torch.mps.synchronize()
    elif device == "cuda":
        torch.cuda.synchronize()

def timed_method(owner, name, log):
    original = getattr(owner, name)
    def wrapper(*a, **k):
        sync()
        start = time.time()
        result = original(*a, **k)
        sync()
        log.append((start, time.time()))
        return result
    setattr(owner, name, wrapper)

load_start = time.time()
model = StableAudioModel.from_pretrained(config["model_name"], device=device,
                                         model_half=config["model_half"])
sync()
load_end = time.time()

encode_log, decode_log = [], []
timed_method(model.model.conditioner, "forward", encode_log)
timed_method(model.model.pretransform, "decode", decode_log)

def generate():
    encode_log.clear(); decode_log.clear()
    sync()
    start = time.time()
    audio = model.generate(
        prompt=config["prompt"],
        duration=config["seconds"],
        steps=config["steps"],
        cfg_scale=1.0,
        seed=config["seed"],
        duration_padding_sec=0.0,
        truncate_output_to_duration=True,
    )
    sync()
    end = time.time()
    encode = sum(e - s for s, e in encode_log)
    decode = sum(e - s for s, e in decode_log)
    return audio, {
        "start": start,
        "end": end,
        "generate_seconds": end - start,
        "prompt_encoding_seconds": encode,
        "decoding_seconds": decode,
        "sampling_seconds": end - start - encode - decode,
        "sampling_window": [encode_log[-1][1], decode_log[0][0]],
        "decoding_window": [decode_log[0][0], decode_log[-1][1]],
    }

runs = []
for _ in range(2 if config["warmup"] else 1):
    audio, timing = generate()
    runs.append(timing)

torchaudio.save(config["output"], audio[0].float().cpu(), 44100)

print("BENCHMARK_RESULT_JSON:" + json.dumps({
    "load_start": load_start,
    "load_end": load_end,
    "load_seconds": load_end - load_start,
    "dtype": str(next(model.model.model.parameters()).dtype),
    "model_half": model.model_half,
    "audio_shape": list(audio.shape),
    "runs": runs,
}))
'''


def pytorch_python(repo):
    """The interpreter to run the PyTorch child with.

    SA3_PYTORCH_PYTHON names one outright. Otherwise `uv run --no-sync`, and
    the --no-sync matters: the project pins torch to the CUDA index only for
    Linux, so on Windows a sync installs the CPU wheel - and it will do that
    over a CUDA one somebody installed on purpose, silently, leaving the
    benchmark to report PyTorch running on the CPU as if it were the GPU.
    """
    named = os.environ.get("SA3_PYTORCH_PYTHON")

    if named:
        return named

    return subprocess.run(
        ["uv", "run", "--no-sync", "--project", str(repo), "python", "-c",
         "import sys; print(sys.executable)"],
        capture_output=True, text=True, cwd=str(repo), check=True,
    ).stdout.strip()


def run_pytorch(args):
    output_path = OUTPUT_DIR / f"pytorch_{args.device}_{args.label}.wav"
    config = {
        "model_name": PYTORCH_MODEL_NAMES[args.model],
        "device": args.device,
        "model_half": args.pytorch_half,
        "prompt": args.prompt,
        "seconds": args.seconds,
        "steps": args.steps,
        "seed": args.seed,
        "warmup": args.warmup,
        "output": str(output_path),
    }
    child_path = OUTPUT_DIR / "_pytorch_run.py"
    child_path.write_text(PYTORCH_CHILD)
    command = [pytorch_python(args.pytorch_repo), str(child_path), json.dumps(config)]

    baseline_in_use = GpuSampler.read()[1]
    with GpuSampler() as sampler:
        start = time.time()
        proc, rusage = run_timed(command, cwd=args.pytorch_repo, timeout=args.timeout)
        end = time.time()

    if proc.returncode != 0:
        fail(f"pytorch({args.device})", proc)

    line = next(l for l in proc.stdout.splitlines() if l.startswith("BENCHMARK_RESULT_JSON:"))
    result = json.loads(line.split(":", 1)[1])

    for run, name in zip(result["runs"], ["cold", "warm"]):
        run["gpu_whole_generate"] = sampler.summarize(run["start"], run["end"], baseline_in_use)
        run["gpu_sampling"] = sampler.summarize(*run["sampling_window"], baseline_in_use)
        run["gpu_decoding"] = sampler.summarize(*run["decoding_window"], baseline_in_use)
        result[name] = run
    del result["runs"]

    result["gpu_load"] = sampler.summarize(result["load_start"], result["load_end"], baseline_in_use)
    result["wall_seconds"] = end - start
    result.update(rusage)
    result["output_path"] = str(output_path)
    return result


def power_source():
    """AC, Battery, or unknown - and a machine with no battery is on AC.

    The guard exists because a laptop on battery caps its GPU clock and halves
    every number. A desktop cannot do that, and answering "unknown" for one
    would make it refuse to benchmark at all.
    """
    if sys.platform == "win32":
        return windows_power_source()

    try:
        out = subprocess.run(["pmset", "-g", "batt"], capture_output=True, text=True).stdout
    except OSError:
        return "unknown"
    return "AC" if "AC Power" in out else "Battery" if "Battery" in out else "unknown"


def windows_power_source():
    """GetSystemPowerStatus: ACLineStatus 1 is mains, 0 is battery.

    BatteryFlag 128 is "no system battery", which is every desktop, and is AC
    whatever the line status claims.
    """
    import ctypes

    class SystemPowerStatus(ctypes.Structure):
        _fields_ = [("ACLineStatus", ctypes.c_ubyte),
                    ("BatteryFlag", ctypes.c_ubyte),
                    ("BatteryLifePercent", ctypes.c_ubyte),
                    ("SystemStatusFlag", ctypes.c_ubyte),
                    ("BatteryLifeTime", ctypes.c_ulong),
                    ("BatteryFullLifeTime", ctypes.c_ulong)]

    status = SystemPowerStatus()

    if not ctypes.windll.kernel32.GetSystemPowerStatus(ctypes.byref(status)):
        return "unknown"

    if status.BatteryFlag == 128 or status.ACLineStatus == 1:
        return "AC"

    return "Battery" if status.ACLineStatus == 0 else "unknown"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--side", choices=["eacp", "pytorch", "both"], default="both")
    parser.add_argument("--model", choices=["small", "medium"], default="small")
    parser.add_argument("--seconds", type=float, default=12.0)
    parser.add_argument("--steps", type=int, default=8)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--prompt", default="lofi house loop")
    parser.add_argument("--eacp-binary", type=Path, default=DEFAULT_EACP_BINARY)
    parser.add_argument("--pytorch-repo", type=Path, default=os.environ.get("SA3_PYTORCH_REPO"))
    parser.add_argument("--device", choices=["mps", "cuda", "cpu"],
                        default="cuda" if sys.platform != "darwin" else "mps")
    parser.add_argument("--no-warmup", dest="warmup", action="store_false",
                        help="PyTorch: time only the first (cold) generate()")
    parser.add_argument("--timeout", type=float, default=None,
                        help="PyTorch: give up after this many seconds")
    parser.add_argument("--label", default=None)
    parser.add_argument("--pytorch-fp32", dest="pytorch_half", action="store_false",
                        help="PyTorch: fp32 weights, to match eacp's precision "
                             "(its default on a GPU is fp16)")
    parser.add_argument("--allow-battery", action="store_true",
                        help="run even on battery power (Low Power Mode caps the GPU clock)")
    args = parser.parse_args()

    power = power_source()
    if power != "AC" and not args.allow_battery:
        parser.error(f"on {power}: Low Power Mode caps the GPU clock and halves every "
                     "number; plug in, or pass --allow-battery")

    if args.steps != 8:
        print("note: the eacp binary always samples 8 steps", file=sys.stderr)
    if args.side != "eacp" and args.pytorch_repo is None:
        parser.error("--pytorch-repo (or SA3_PYTORCH_REPO) is required for PyTorch")
    args.label = args.label or f"{args.model}_{int(args.seconds)}s"
    OUTPUT_DIR.mkdir(exist_ok=True)

    results = {"config": {k: str(v) for k, v in vars(args).items()},
               "power_source": power}
    if args.side in ("eacp", "both"):
        print(f"=== eacp ({args.model}) ===", flush=True)
        results["eacp"] = run_eacp(args)
    if args.side in ("pytorch", "both"):
        print(f"=== PyTorch {args.device} ({args.model}) ===", flush=True)
        results[f"pytorch_{args.device}"] = run_pytorch(args)

    path = OUTPUT_DIR / f"{args.label}.json"
    path.write_text(json.dumps(results, indent=2))
    print(json.dumps({k: ({kk: vv for kk, vv in v.items() if kk != "stdout"}
                          if isinstance(v, dict) else v)
                      for k, v in results.items() if k != "config"}, indent=2))
    print(f"Raw results in {path}")


if __name__ == "__main__":
    main()
