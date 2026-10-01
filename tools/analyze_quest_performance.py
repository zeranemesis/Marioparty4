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
        # Display frames each new world image stayed on show: 1, 2, 3, 4 or more.
        holds = re.search(r"holds=(\d+)/(\d+)/(\d+)/(\d+)", line)
        hold_counts = [int(value) for value in holds.groups()] if holds else None
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
            "latency_min_ms": values.get("latencyMin"), "latency_max_ms": values.get("latencyMax"),
            "paced": values.get("paced"), "pace_work_ms": values.get("paceWork"),
            "pace_phase_ms": values.get("pacePhase"),
            # Images not shown for the window's usual number of display frames: judder.
            "off_cadence_percent": (100.0 * (sum(hold_counts) - max(hold_counts)) / sum(hold_counts)
                                    if hold_counts and sum(hold_counts) else None),
        })
    return samples


def read_game(text):
    """The game thread's windows ("Game perf"): frames drawn for the eyes and time spent recording them."""
    rows = []
    pattern = (r"Game perf: scene=(-?\d+) ([\d.]+) frames/s interval avg=([\d.]+)ms max=([\d.]+)ms "
               r"stutters=(\d+) record avg=([\d.]+)ms max=([\d.]+)ms")
    for line in text.splitlines():
        match = re.search(pattern, line)
        if match:
            rows.append({"scene": int(match[1]), "frames_per_s": float(match[2]),
                         "interval_avg_ms": float(match[3]), "interval_max_ms": float(match[4]),
                         "stutters": int(match[5]), "record_avg_ms": float(match[6]),
                         "record_max_ms": float(match[7])})
    return rows


def read_draws(text):
    """World draws per eye ("Stereo draws") and the share drawn for both eyes at once."""
    rows = []
    for line in text.splitlines():
        match = re.search(r"Stereo draws: \d+ frames, world (\d+) avg / (\d+) max per eye \((\d+)% both", line)
        if match:
            rows.append({"world_draws_avg": int(match[1]), "world_draws_max": int(match[2]),
                         "instanced_percent": int(match[3])})
    return rows


def read_memory(text):
    """Resident memory of the game and memory left to the system ("Memory")."""
    rows = []
    for line in text.splitlines():
        match = re.search(r"Memory: rss=(-?\d+)MB available=(-?\d+)MB", line)
        if match:
            rows.append({"rss_mb": int(match[1]), "available_mb": int(match[2])})
    return rows


def read_pipeline_drops(text):
    """Draws dropped for a pipeline still to be built ("Pipelines:"), per 5 s window; absent when none."""
    rows = []
    for line in text.splitlines():
        match = re.search(r"Pipelines: (\d+) draws dropped in the last 5 s.*\((\d+) queued, (\d+) built\)", line)
        if match:
            rows.append({"draws_dropped": int(match[1]), "queued": int(match[2]), "built": int(match[3])})
    return rows


def read_perf_events(text):
    """The headset's throttling notifications ("Perf settings"), in order."""
    events = []
    for line in text.splitlines():
        match = re.search(r"Perf settings: (CPU|GPU) (\w+) (\w+) -> (\w+)", line)
        if match:
            events.append({"time": line[:18].strip(), "domain": match[1], "sub_domain": match[2],
                           "from": match[3], "to": match[4]})
    return events


def spread(rows):
    metrics = {}
    for key in rows[0] if rows else []:
        values = sorted(row[key] for row in rows if isinstance(row[key], (int, float)) and row[key] >= 0)
        if values:
            metrics[key] = {"min": values[0], "median": values[len(values) // 2],
                            "max": values[-1], "samples": len(values)}
    return metrics


def summarize(samples, text=None):
    result = {"world_windows": len(samples), "metrics": {}}
    if text is not None:
        game = read_game(text)
        result["game"] = spread(game)
        result["game_by_scene"] = {str(scene): spread([row for row in game if row["scene"] == scene])
                                   for scene in sorted({row["scene"] for row in game})}
        result["draws"] = spread(read_draws(text))
        result["perf_events"] = read_perf_events(text)
        drops = read_pipeline_drops(text)
        result["pipeline_drops"] = spread(drops)
        result["pipeline_drops"]["total_draws_dropped"] = sum(row["draws_dropped"] for row in drops)
        memory = read_memory(text)
        result["memory"] = spread(memory)
        if memory and memory[0]["rss_mb"] >= 0 and memory[-1]["rss_mb"] >= 0:
            result["memory"]["rss_growth_mb"] = memory[-1]["rss_mb"] - memory[0]["rss_mb"]
    if not samples:
        result["note"] = "No world-rendering windows found; no performance conclusion is possible."
        return result
    result["metrics"] = spread(samples)
    result["note"] = "Window summaries, not individual frame percentiles. GPU copy timing is optional."
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    text = args.log.read_text(encoding="utf-8", errors="replace")
    samples = read_samples(text)
    summary = summarize(samples, text)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    if samples:
        with (args.output / "world-windows.csv").open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=samples[0].keys())
            writer.writeheader()
            writer.writerows(samples)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
