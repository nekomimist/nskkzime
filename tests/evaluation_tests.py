"""Checks the evaluator's measurements/denominators and real native lookup path."""
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("evaluate", ROOT / "scripts/evaluate.py")
evaluation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluation)
CORE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None


class EvaluationTests(unittest.TestCase):
    def test_nearest_rank_percentile_and_empty_group(self):
        self.assertEqual(evaluation.percentile(list(range(1, 21)), .95), 19)
        self.assertIsNone(evaluation.latency([])["p95_ms"])
        self.assertEqual(evaluation.latency([2])["samples"], 1)

    def test_accuracy_counts_misses_and_alternative_answers(self):
        score = evaluation.accuracy([(["甲", "乙"], ["乙"]), (["甲"], ["丙", "甲"]), (["甲"], [])])
        self.assertEqual((score["cases"], score["top1"], score["found"]), (3, 1, 2))
        self.assertEqual(score["mrr"], .5)
        self.assertEqual(score["space_enter_mean"], 1.5)
        self.assertEqual(score["page_digit_mean"], 1)

    def test_page_actions_and_no_invented_cost_for_missing_gold(self):
        self.assertEqual(evaluation.accuracy([(["7"], [str(i) for i in range(8)])])["page_digit_mean"], 2)
        self.assertIsNone(evaluation.accuracy([(["missing"], [])])["space_enter_mean"])

    def test_promote_threshold_ties_and_unscored_tail(self):
        words = ["回答", "解答", "解凍", "怪盗"]
        self.assertEqual(evaluation.promote(words, [0, 1, -1]), words)
        self.assertEqual(evaluation.promote(words, [0, 1.01, -1]), ["解答", "回答", "解凍", "怪盗"])
        self.assertEqual(evaluation.promote(words, [0, 2, 2]), ["解答", "回答", "解凍", "怪盗"])
        self.assertEqual(words[0], "回答")
        with self.assertRaises(ValueError):
            evaluation.promote(words, [math.nan])

    def test_generated_filter_and_stable_merge(self):
        values = evaluation.filtered_generation("かんじ", ["かんじ", "カンジ", "漢字", "感じ", "\ufffd", "漢字", "幹事"], [0, 0, 0, -8, 0, 0, -8.1])
        self.assertEqual(values, ["漢字", "感じ"])
        self.assertEqual(evaluation.merge(values, ["感じ", "幹事"]), ["漢字", "感じ", "幹事"])

    def test_labels_are_validated_not_silently_skipped(self):
        with self.assertRaises(ValueError):
            evaluation.validate_cases([])
        case = dict(id="one", kana="かな", gold=["仮名"])
        with self.assertRaises(ValueError):
            evaluation.validate_cases([case, case])
        with self.assertRaises(ValueError):
            evaluation.validate_cases([dict(case, gold=[])])
        with self.assertRaises(ValueError):
            evaluation.validate_cases([dict(case, lookup="か", okuri="く")])

    def test_unicode_lengths_and_reading_eligibility(self):
        self.assertEqual(evaluation.hiragana("カナ。ABC"), "かな。ABC")
        self.assertTrue(evaluation.allowed_reading("かな、です。"))
        self.assertFalse(evaluation.allowed_reading("abc"))
        self.assertFalse(evaluation.allowed_reading("あ" * 1025))
        self.assertEqual(evaluation.length_bucket("あ" * 10), "10-19")
        self.assertEqual(("😀" * 41)[-40:], "😀" * 40)

    def test_nonfinite_json_is_rejected(self):
        for literal in ("NaN", "Infinity", "-Infinity"):
            with self.assertRaises(ValueError):
                evaluation.strict_json('{"score":' + literal + '}')

    def test_repetitions_do_not_inflate_quality_denominator(self):
        row = dict(corpus="diagnostic", operation="score", context="left", id="one", kana="かな", gold=["仮名"],
                   dictionary=["佳名", "仮名"], order=["仮名", "佳名"], round=0, ms=1,
                   response={"scores": [-3, 0]}, within_deadline=True)
        report = evaluation.summarize([row, dict(row, round=1, ms=2)])[0]
        self.assertEqual(report["samples"], 2)
        self.assertEqual(report["result"]["cases"], 1)
        self.assertEqual(report["rank_changes"]["improved"], 1)
        self.assertEqual(report["paired_actions"]["space_enter_delta_mean"], -1)

    def test_error_and_empty_result_remain_in_accuracy(self):
        row = dict(corpus="ajimee", operation="convert", context="none", id="one", kana="かな", gold=["仮名"],
                   dictionary=[], order=[], filtered=[], round=0, ms=1001,
                   response={"error": "timeout or model error"}, within_deadline=False)
        report = evaluation.summarize([row])[0]
        self.assertEqual((report["server_errors"], report["over_budget"]), (1, 1))
        self.assertEqual((report["raw_generation"]["cases"], report["raw_generation"]["top1"]), (1, 0))

    def test_rank_worsening_and_coverage_changes_are_counted(self):
        row = dict(corpus="diagnostic", operation="score", context="none", kana="かな", gold=["仮名"], round=0,
                   ms=1, response={}, within_deadline=True)
        rows = [dict(row, dictionary=["仮名", "佳名"], order=["佳名", "仮名"]),
                dict(row, dictionary=["仮名"], order=[]), dict(row, dictionary=[], order=["仮名"])]
        result = evaluation.summarize(rows)[0]
        self.assertEqual(result["rank_changes"], dict(improved=1, worsened=2, unchanged=0))
        self.assertEqual(result["paired_actions"]["cases"], 1)

    def test_ineligible_readings_and_short_stress_outputs_are_visible(self):
        row = dict(corpus="ajimee", operation="convert", context="none", id="one", kana="「かな」", gold=["「仮名」"],
                   dictionary=[], order=[], filtered=[], round=0, ms=10, eligible=False,
                   response={"candidates": ["「仮名」"], "scores": [0]}, within_deadline=True)
        report = evaluation.summarize([row])[0]
        self.assertEqual((report["ineligible_cases"], report["raw_generation"]["top1"], report["result"]["top1"]), (1, 1, 0))
        stress = dict(row, corpus="stress", id="reading-512", response={"candidates": ["。"]})
        self.assertEqual(evaluation.summarize([stress])[0]["by_case"]["reading-512"]["generated_length_min"], 1)

    def test_inference_response_contract_rejects_bad_frames(self):
        invalid = [dict(id=True, candidates=["語"], scores=[0]), dict(id=2, candidates=["語"], scores=[0]),
                   dict(id=1, candidates=["語"], scores=[]), dict(id=1, candidates=["bad\n"], scores=[0]),
                   dict(id=1, candidates=["語"], scores=[math.inf])]
        for response in invalid:
            server = object.__new__(evaluation.ModelServer)
            server.request_id, server.deadline = 0, 1000
            server.request = lambda request: (response, 1)
            with self.assertRaises(ValueError):
                server.infer("convert", "ご", "", "")
        server.request_id = 0
        server.request = lambda request: (dict(id=1, candidates=["語"], scores=[0]), 1)
        with self.assertRaises(ValueError):
            server.infer("score", "ご", "", "", ["語"])

    def test_child_transport_unicode_and_error_frame_recovery(self):
        code = 'import sys; print("{}", flush=True)\nfor line in sys.stdin: print(line.strip(), flush=True)'
        child = evaluation.JsonProcess([sys.executable, "-u", "-X", "utf8", "-c", code], subprocess.DEVNULL)
        try:
            value, _ = child.request(dict(text="日本語 😀"))
            self.assertEqual(value["text"], "日本語 😀")
            self.assertIn("error", child.request(dict(error="failed"))[0])
            self.assertEqual(child.request(dict(next=1))[0]["next"], 1)
        finally:
            child.close()
        self.assertIsNotNone(child.process.poll())

    def test_child_timeout_and_exit_are_terminal_not_restarted(self):
        code = 'import sys,time; print("{}", flush=True); sys.stdin.readline(); time.sleep(10)'
        child = evaluation.JsonProcess([sys.executable, "-u", "-c", code], subprocess.DEVNULL)
        child.timeout = .05
        try:
            with self.assertRaises(TimeoutError):
                child.request(dict(wait=True))
        finally:
            child.close(force=True)
        self.assertIsNotNone(child.process.poll())
        code = 'print("{}", flush=True)'
        child = evaluation.JsonProcess([sys.executable, "-u", "-c", code], subprocess.DEVNULL)
        try:
            with self.assertRaises(RuntimeError):
                child.read()
        finally:
            child.close()

    @unittest.skipUnless(CORE, "Pass the native helper path for integration checks")
    def test_real_dictionary_okuri_key_timing_and_recovery(self):
        with tempfile.TemporaryDirectory(prefix="評価 space-", dir=ROOT / "out") as folder:
            fixture = Path(folder) / "辞書.skk"
            fixture.write_bytes((ROOT / "tests/fixtures/basic.utf8.skk").read_bytes())
            child = evaluation.JsonProcess([CORE, fixture], subprocess.DEVNULL)
            try:
                self.assertEqual(child.hello["hello"], "nskkzime-eval-core")
                value, _ = child.request(dict(op="lookup", reading="か", okuri_letter="k", okuri="け", repeats=3))
                self.assertEqual(value["candidates"][:2], ["描け", "書け"])
                self.assertEqual(len(value["lookup_ms"]), 3)
                self.assertIn("error", child.request(dict(op="lookup", reading="か", repeats=0))[0])
                self.assertEqual(child.request(dict(op="lookup", reading="ない"))[0]["candidates"], [])
                measured, _ = child.request(dict(op="keys", rounds=2))
                self.assertEqual(measured["rounds"], 2)
                self.assertGreater(len(measured["key_ms"]), len(measured["conversion_key_ms"]))
                self.assertEqual(len(measured["conversion_key_ms"]), 10)
                self.assertGreater(measured["checksum"], 0)
            finally:
                child.close()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
