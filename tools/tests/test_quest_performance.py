import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("quest_analysis", Path(__file__).parents[1] / "analyze_quest_performance.py")
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class PerformanceLogTests(unittest.TestCase):
    def test_distinguishes_refresh_from_new_world_frames(self):
        text = """09-27 12:00:00.000 I Perf: 120Hz display, xr=120Hz
09-27 12:00:02.000 I Stereo perf: source=110.0Hz presented=100.0Hz worldNew=88.0Hz hudNew=30.0Hz ringFull=12 copyMax=4.1ms acquireCpuMax=2.0ms copyGpuMax=-1.0ms gpuSamples=0 world=1 res=80% latency=25ms eye=1344x1408
09-27 12:00:04.000 I Stereo perf: source=0.0Hz presented=0.0Hz world=0
"""
        rows = analysis.read_samples(text)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["target_hz"], 120)
        self.assertEqual(rows[0]["world_new_hz"], 88)
        self.assertEqual(rows[0]["eye_height"], 1408)
        summary = analysis.summarize(rows)
        self.assertNotIn("copy_gpu_window_max_ms", summary["metrics"])

    def test_game_thread_draws_and_memory(self):
        text = """09-27 12:00:00.000 I Game perf: scene=89 52.3 frames/s interval avg=19.12ms max=41.00ms stutters=3 record avg=9.80ms max=14.20ms
09-27 12:00:05.000 I Game perf: scene=428 88.0 frames/s interval avg=11.36ms max=20.00ms stutters=0 record avg=4.10ms max=6.00ms
09-27 12:00:05.100 I Stereo draws: 240 frames, world 612 avg / 700 max per eye (0% both eyes at once), HUD 40 avg, 2192x2104 image
09-27 12:00:05.200 I Memory: rss=900MB available=2100MB
09-27 12:00:10.200 I Memory: rss=1010MB available=1800MB
09-27 12:00:11.000 W Perf settings: GPU thermal normal -> warning
"""
        summary = analysis.summarize(analysis.read_samples(text), text)
        self.assertEqual(summary["game_by_scene"]["89"]["frames_per_s"]["max"], 52.3)
        self.assertEqual(summary["game"]["record_max_ms"]["max"], 14.2)
        self.assertEqual(summary["draws"]["world_draws_avg"]["max"], 612)
        self.assertEqual(summary["memory"]["rss_growth_mb"], 110)
        self.assertEqual(summary["memory"]["available_mb"]["min"], 1800)
        self.assertEqual(summary["world_windows"], 0)
        self.assertEqual(summary["perf_events"][0]["to"], "warning")
        self.assertEqual(summary["perf_events"][0]["sub_domain"], "thermal")

    def test_cadence(self):
        rows = analysis.read_samples("Stereo perf: source=60Hz presented=120Hz world=1 latency=25.0ms "
                                     "holds=3/110/4/1 latencyMin=16.7ms latencyMax=33.3ms paced=1 "
                                     "paceWork=19.2ms paceLooks=3 pacePhase=4.3ms")
        self.assertAlmostEqual(rows[0]["off_cadence_percent"], 100 * 8 / 118)
        self.assertEqual(rows[0]["latency_max_ms"], 33.3)
        self.assertEqual(rows[0]["paced"], 1)
        self.assertEqual(rows[0]["pace_phase_ms"], 4.3)
        self.assertIsNone(analysis.read_samples("Stereo perf: world=1")[0]["off_cadence_percent"])

    def test_pipeline_drops(self):
        text = """09-28 12:00:05.000 I Pipelines: 240 draws dropped in the last 5 s, their pipeline not built yet (12 queued, 88 built)
09-28 12:00:10.000 I Pipelines: 30 draws dropped in the last 5 s, their pipeline not built yet (2 queued, 98 built)
"""
        summary = analysis.summarize([], text)
        self.assertEqual(summary["pipeline_drops"]["total_draws_dropped"], 270)
        self.assertEqual(summary["pipeline_drops"]["draws_dropped"]["max"], 240)
        self.assertEqual(analysis.summarize([], "nothing")["pipeline_drops"]["total_draws_dropped"], 0)

    def test_legacy_logs_and_missing_data(self):
        rows = analysis.read_samples("Stereo perf: source=60Hz presented=59Hz copyMax=1.0ms world=1")
        self.assertEqual(rows[0]["world_new_hz"], 59)
        self.assertIsNone(rows[0]["target_hz"])
        self.assertEqual(analysis.summarize([])["world_windows"], 0)


if __name__ == "__main__":
    unittest.main()
