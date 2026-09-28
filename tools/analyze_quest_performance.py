"""Summarize recorded Quest logs without confusing XR refresh and new world images."""
import argparse
import csv
import json
import re
from pathlib import Path


def read_samples(text):
    samples = []
    target = None
    for line in text.splitlines():
        match = re.search(r"Perf: ([\d.]+)Hz display", line)
        if match:
            target = float(match[1])
        if "Stereo perf:" not in line:
            continue
        values = {key: float(value) for key, value in re.findall(r"([A-Za-z]\w*)=(-?\d+(?:\.\d+)?)", line)}
        if values.get("world") != 1:
            continue
        size = re.search(r"eye=(\d+)x(\d+)", line)
        samples.append({
            "time": line[:18].strip(), "target_hz": target,
            "source_hz": values.get("source"),
            "world_new_hz": values.get("worldNew", values.get("presented")),
            "presented_hz": values.get("presented"), "hud_new_hz": values.get("hudNew"),
            "resolution_percent": values.get("res"), "ring_full": values.get("ringFull"),
            "transfer_cpu_window_max_ms": values.get("copyMax"),
            "acquire_cpu_window_max_ms": values.get("acquireCpuMax"),
            "world_acquire_window_max_ms": values.get("worldAcquireMax"),
            "world_wait_window_max_ms": values.get("worldWaitMax"),
            "hud_acquire_window_max_ms": values.get("hudAcquireMax"),
            "hud_wait_window_max_ms": values.get("hudWaitMax"),
            "copy_gpu_window_max_ms": values.get("copyGpuMax"),
            "gpu_samples": values.get("gpuSamples"), "latency_ms": values.get("latency"),
            "eye_width": int(size[1]) if size else None, "eye_height": int(size[2]) if size else None,
        })
    return samples


def summarize(samples):
    result = {"world_windows": len(samples), "metrics": {}}
    if not samples:
        result["note"] = "No world-rendering windows found; no performance conclusion is possible."
        return result
    for key in samples[0]:
        values = sorted(row[key] for row in samples if isinstance(row[key], (int, float)) and row[key] >= 0)
        if values:
            result["metrics"][key] = {"min": values[0], "median": values[len(values) // 2],
                                      "max": values[-1], "samples": len(values)}
    result["note"] = "Window summaries, not individual frame percentiles. GPU copy timing is optional."
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    samples = read_samples(args.log.read_text(encoding="utf-8", errors="replace"))
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "summary.json").write_text(json.dumps(summarize(samples), indent=2), encoding="utf-8")
    if samples:
        with (args.output / "world-windows.csv").open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=samples[0].keys())
            writer.writeheader()
            writer.writerows(samples)
    print(json.dumps(summarize(samples), indent=2))


if __name__ == "__main__":
    main()
