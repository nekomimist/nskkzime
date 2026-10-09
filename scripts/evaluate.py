#!/usr/bin/env python3
"""Offline quality and component latency evaluation; Python standard library only.

Uses explicit public/fixture files, never live IME settings or input history.
The report contains the supplied evaluation text and model output by design.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import queue
import subprocess
import sys
import threading
import time
import uuid
from collections import defaultdict
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parent.parent
AJIMEE_REVISION = "401666cd56d1a570c2021798b64b6da4396bfd45"
AJIMEE_SHA256 = "e9eb668fd6aa14b1e26436f429b5550108af0a1dfd443b8cea0bcb3ab3028fca"
MODEL_SHA256 = "29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673"


def digest(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def strict_json(text):
    def invalid(value):
        raise ValueError(f"Nonfinite JSON number: {value}")
    return json.loads(text, parse_constant=invalid)


def hiragana(text):
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def katakana(text):
    return "".join(chr(ord(c) + 0x60) if "ぁ" <= c <= "ゖ" else c for c in text)


def allowed_reading(text):
    return 0 < len(text) <= 1024 and all("ぁ" <= c <= "ゖ" or c in "ゝゞー、。・！？" for c in text)


def validate_cases(cases):
    ids = set()
    if not cases:
        raise ValueError("No evaluation cases")
    for case in cases:
        if not isinstance(case.get("id"), str) or not case["id"] or case["id"] in ids:
            raise ValueError("Invalid/duplicate case ID")
        ids.add(case["id"])
        if not isinstance(case.get("kana"), str) or not 0 < len(case["kana"]) <= 1024:
            raise ValueError("Invalid reading")
        if not isinstance(case.get("gold"), list) or not case["gold"] or any(not isinstance(x, str) or not x for x in case["gold"]):
            raise ValueError("Expected output must be a nonempty list of nonempty strings")
        for name in ("left", "right"):
            if not isinstance(case.get(name, ""), str):
                raise ValueError("Invalid context")
        if case.get("okuri") and case.get("lookup", "") + case["okuri"] != case["kana"]:
            raise ValueError("Stem and okuri do not reproduce reading")
    return cases


def load_cases(fixture, ajimee):
    document = strict_json(Path(fixture).read_text(encoding="utf-8"))
    if document.get("version") != 1:
        raise ValueError("Unsupported diagnostic corpus version")
    cases = [dict(case, corpus="diagnostic") for case in document["cases"]]
    if ajimee:
        if digest(ajimee) != AJIMEE_SHA256:
            raise ValueError("AJIMEE checksum differs from the pinned benchmark")
        for item in strict_json(Path(ajimee).read_text(encoding="utf-8")):
            cases.append(dict(id=f"ajimee-{item['index']}", corpus="ajimee", kana=hiragana(item["input"]),
                              left=item["context_text"], right="", gold=item["expected_output"]))
    return validate_cases(cases)


def percentile(values, fraction):
    """Nearest-rank quantile; empty groups are unknown, never zero latency."""
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * fraction) - 1)] if values else None


def latency(values):
    return dict(samples=len(values), p50_ms=percentile(values, .50), p95_ms=percentile(values, .95),
                max_ms=max(values) if values else None)


def rank(gold, candidates):
    return next((i for i, text in enumerate(candidates) if text in gold), None)


def accuracy(pairs):
    ranks = [rank(gold, words) for gold, words in pairs]
    present = [r for r in ranks if r is not None]
    return dict(cases=len(ranks), top1=sum(r == 0 for r in ranks), top5=sum(r is not None and r < 5 for r in ranks),
                found=len(present), mrr=sum(1 / (r + 1) for r in present) / len(ranks) if ranks else None,
                # Costs start with the first candidate already displayed. Missing
                # answers have no invented cost; coverage is reported separately.
                space_enter_mean=sum(r + 1 for r in present) / len(present) if present else None,
                page_digit_mean=sum(r // 7 + 1 for r in present) / len(present) if present else None)


def promote(words, scores, threshold=1.0):
    if not scores:
        return list(words)
    if len(scores) > len(words) or any(not math.isfinite(s) for s in scores):
        raise ValueError("Invalid scores")
    best = max(range(len(scores)), key=lambda i: scores[i])
    order = list(words)
    if scores[best] - scores[0] > threshold:
        order.insert(0, order.pop(best))
    return order


def filtered_generation(kana, words, scores, gap=8.0):
    if len(words) != len(scores):
        raise ValueError("Generation score count mismatch")
    best = max(scores, default=0)
    result = []
    for word, score in zip(words, scores):
        if word not in (kana, katakana(kana)) and "\ufffd" not in word and best - score <= gap and word not in result:
            result.append(word)
    return result


def merge(first, second):
    return list(dict.fromkeys([*first, *second]))


def length_bucket(kana):
    return "1-9" if len(kana) < 10 else "10-19" if len(kana) < 20 else "20-39" if len(kana) < 40 else "40+"


class JsonProcess:
    def __init__(self, command, stderr, timeout=15):
        self.timeout = timeout
        self.lines = queue.Queue()
        self.started = time.perf_counter()
        self.process = subprocess.Popen([str(x) for x in command], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=stderr, encoding="utf-8", errors="strict", bufsize=1,
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        def read():
            try:
                while True:
                    # Native key benchmark replies contain tens of thousands of
                    # samples. The server's own protocol has a much smaller cap.
                    line = self.process.stdout.readline(64 * 1024 * 1024)
                    if not line:
                        raise RuntimeError("Child closed stdout")
                    if not line.endswith("\n"):
                        raise RuntimeError("Oversized/unterminated child response")
                    self.lines.put(line)
            except Exception as error:
                self.lines.put(error)
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
        try:
            self.hello = self.read()
            self.startup_ms = (time.perf_counter() - self.started) * 1000
        except BaseException:
            self.close(force=True)
            raise

    def read(self):
        try:
            value = self.lines.get(timeout=self.timeout)
        except queue.Empty as error:
            raise TimeoutError("Child response deadline exceeded") from error
        if isinstance(value, Exception):
            raise value
        result = strict_json(value)
        if not isinstance(result, dict):
            raise ValueError("Child response is not an object")
        return result

    def request(self, request):
        start = time.perf_counter()
        self.process.stdin.write(json.dumps(request, ensure_ascii=False, allow_nan=False) + "\n")
        self.process.stdin.flush()
        result = self.read()
        return result, (time.perf_counter() - start) * 1000

    def close(self, force=False):
        try:
            if self.process.poll() is None:
                if force:
                    self.process.kill()
                else:
                    self.process.stdin.close()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
            self.reader.join(timeout=5)
            if not force and self.process.returncode:
                raise RuntimeError(f"Child exited with {self.process.returncode}")
        finally:
            for stream in (self.process.stdin, self.process.stdout):
                if stream and not stream.closed:
                    stream.close()


class ModelServer(JsonProcess):
    def __init__(self, program, model, threads, deadline, stderr):
        super().__init__([program, "--model", model, "--stdio", "--threads", str(threads), "--timeout-ms", str(deadline)], stderr)
        self.request_id = 0
        self.deadline = deadline
        if self.hello.get("hello") != "nkkz-ime-server" or self.hello.get("protocol") != 2 or self.hello.get("backend") != "zenz":
            self.close(force=True)
            raise ValueError("Expected the real zenz protocol-2 server")

    def infer(self, operation, kana, left, right, words=None):
        self.request_id += 1
        request = dict(id=self.request_id, op=operation, kana=kana, left=left, right=right)
        request.update(n=5) if operation == "convert" else request.update(candidates=words)
        result, elapsed = self.request(request)
        if type(result.get("id")) is not int or result["id"] != self.request_id:
            raise ValueError("Mismatched inference response ID")
        if "error" not in result:
            if operation == "score" and "candidates" in result:
                raise ValueError("Unexpected candidates in score response")
            scores = result.get("scores")
            candidates = result.get("candidates") if operation == "convert" else words
            if not isinstance(candidates, list) or not isinstance(scores, list) or len(scores) != len(candidates):
                raise ValueError("Malformed inference result")
            if operation == "convert" and (len(candidates) > 5 or any(not isinstance(w, str) or not w or len(w) > 1024 or any(ord(c) < 32 or ord(c) == 127 or c == '\ufffc' for c in w) for w in candidates)):
                raise ValueError("Invalid generated candidate")
            if any(type(x) not in (int, float) or not math.isfinite(x) for x in scores):
                raise ValueError("Invalid inference score")
        return dict(response=result, ms=elapsed, within_deadline=elapsed <= self.deadline, budget_ms=self.deadline)


def summarize(rows):
    groups = defaultdict(list)
    for row in rows:
        key = (row["corpus"], row["operation"], row["context"])
        groups[key].append(row)
    result = []
    for (corpus, operation, context), values in sorted(groups.items()):
        # Repetitions improve latency sampling, not the number of independent
        # labeled cases. Accuracy is from the first measured attempt only.
        first = [x for x in values if x["round"] == 0]
        record = dict(corpus=corpus, operation=operation, context=context, **latency([x["ms"] for x in values]),
                      server_errors=sum("error" in x["response"] for x in values),
                      over_budget=sum(not x["within_deadline"] for x in values))
        if corpus != "stress":
            record["eligible_cases"] = sum(x.get("eligible", True) for x in first)
            record["ineligible_cases"] = sum(not x.get("eligible", True) for x in first)
            record["baseline"] = accuracy([(x["gold"], x["dictionary"]) for x in first])
            record["result"] = accuracy([(x["gold"], x["order"]) for x in first])
            if operation == "convert":
                record["raw_generation"] = accuracy([(x["gold"], x["response"].get("candidates", [])) for x in first])
                record["filtered_generation"] = accuracy([(x["gold"], x["filtered"]) for x in first])
            improved = worsened = unchanged = 0
            for x in first:
                old, new = rank(x["gold"], x["dictionary"]), rank(x["gold"], x["order"])
                old, new = math.inf if old is None else old, math.inf if new is None else new
                improved += new < old
                worsened += new > old
                unchanged += new == old
            record["rank_changes"] = dict(improved=improved, worsened=worsened, unchanged=unchanged)
            comparable = [x for x in first if rank(x["gold"], x["dictionary"]) is not None and rank(x["gold"], x["order"]) is not None]
            record["paired_actions"] = dict(cases=len(comparable),
                space_enter_delta_mean=sum(rank(x["gold"], x["order"]) - rank(x["gold"], x["dictionary"]) for x in comparable) / len(comparable) if comparable else None,
                page_digit_delta_mean=sum(rank(x["gold"], x["order"]) // 7 - rank(x["gold"], x["dictionary"]) // 7 for x in comparable) / len(comparable) if comparable else None)
            record["by_length"] = {}
            for bucket in ("1-9", "10-19", "20-39", "40+"):
                part = [x for x in first if length_bucket(x["kana"]) == bucket]
                timings = [x["ms"] for x in values if length_bucket(x["kana"]) == bucket]
                record["by_length"][bucket] = dict(quality=accuracy([(x["gold"], x["order"]) for x in part]), **latency(timings))
                record["by_length"][bucket]["eligible_cases"] = sum(x.get("eligible", True) for x in part)
        else:
            record["by_case"] = {case: dict(**latency([x["ms"] for x in values if x["id"] == case]),
                server_errors=sum("error" in x["response"] for x in values if x["id"] == case)) for case in sorted({x["id"] for x in values})}
            if operation == "convert":
                for case, metrics in record["by_case"].items():
                    lengths = [len(word) for x in values if x["id"] == case for word in x["response"].get("candidates", [])]
                    metrics["generated_length_min"] = min(lengths) if lengths else None
                    metrics["generated_length_max"] = max(lengths) if lengths else None
        result.append(record)
    return result


def write_report(output, metadata, rows):
    summary = summarize(rows)
    (output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2, allow_nan=False), encoding="utf-8")
    def milliseconds(value):
        return "—" if value is None else f"{value:.4f}"
    def quality(value):
        return "—" if not value or not value["cases"] else f"{value['top1']}/{value['cases']} ({value['top1'] / value['cases']:.1%})"
    lines = ["# nskkzime オフライン評価", "", f"完了: {metadata['complete']} / 入力例: {metadata['cases']} / 要求: {len(rows)}", "",
             "正解率は各例の最初の計測結果。反復は遅延の標本数だけに加算する。対象外の読みや失敗も正解率の分母に残す。", "",
             "| データ | 操作 | 文脈 | モデル生成top1 | 規則適用後top1 | 対象外 | p50 ms | p95 ms | error / 期限超過 |",
             "|---|---|---|---:|---:|---:|---:|---:|---:|"]
    for group in summary:
        lines.append(f"| {group['corpus']} | {group['operation']} | {group['context']} | {quality(group.get('raw_generation'))} | {quality(group.get('result'))} | {group.get('ineligible_cases', '—')} | {milliseconds(group['p50_ms'])} | {milliseconds(group['p95_ms'])} | {group['server_errors']} / {group['over_budget']} |")
    lines += ["", "規則適用後の生成は、10文字以上で先頭へ追加、それ未満で辞書候補の後ろへ追加した到達可能な候補順。採点はpromote単独の比較。", "",
              "| 成分（ネイティブ計測） | 標本数 | p50 ms | p95 ms |", "|---|---:|---:|---:|"]
    for key in ("core_keys", "core_conversion_keys", "dictionary_lookup_all", "dictionary_lookup_hit", "dictionary_lookup_miss"):
        if key in metadata:
            value = metadata[key]
            lines.append(f"| {key} | {value['samples']} | {milliseconds(value['p50_ms'])} | {milliseconds(value['p95_ms'])} |")
    lines += ["", "TSFロック・表示・OSキー配送は含まない。操作回数は最初の候補表示後の理想化した回数。語を入力する打鍵や待ち時間・登録手間は含まない。",
              "失敗、順位改善／悪化、同じ例同士の操作回数差、長さ別の値は summary.json、個別応答は requests.jsonl、版とハッシュは metadata.json を参照。", ""]
    lines += ["## 負荷試験", "", "応答が成功しても、内容の正しさや全文の保持を保証するものではない。", "",
              "| 入力条件 | 要求数 | p95 ms | error | 生成候補の文字数範囲 |", "|---|---:|---:|---:|---|"]
    for group in summary:
        if group["corpus"] == "stress":
            for case, metrics in group["by_case"].items():
                lengths = f"{metrics.get('generated_length_min')}〜{metrics.get('generated_length_max')}" if metrics.get("generated_length_min") is not None else "—"
                lines.append(f"| {case} | {metrics['samples']} | {milliseconds(metrics['p95_ms'])} | {metrics['server_errors']} | {lengths} |")
    lines.append("")
    (output / "report.md").write_text("\n".join(lines), encoding="utf-8")


def evaluate(args):
    cases = load_cases(args.cases, args.ajimee)
    model_hash = digest(args.model)
    if model_hash != MODEL_SHA256:
        raise ValueError("Model differs from the pinned Q5_K_M baseline")
    output = Path(args.output) if args.output else ROOT / "out/reports" / ("evaluation-" + uuid.uuid4().hex)
    output.mkdir(parents=True, exist_ok=False)
    metadata = dict(timestamp=datetime.now(timezone.utc).isoformat(), complete=False,
                    model_sha256=model_hash, dictionary_sha256=digest(args.dictionary),
                    server_sha256=digest(args.server), core_sha256=digest(args.core), script_sha256=digest(__file__),
                    fixture_sha256=digest(args.cases), ajimee_revision=AJIMEE_REVISION if args.ajimee else None,
                    ajimee_sha256=digest(args.ajimee) if args.ajimee else None,
                    platform=platform.platform(), cpu=platform.processor(), logical_processors=os.cpu_count(),
                    threads=args.threads, rounds=args.rounds, stress_rounds=args.stress_rounds,
                    cases=len(cases), generation_budget_ms=1000, score_budget_ms=300,
                    policy=dict(long_min_length=10, rerank_limit=20, promote_threshold=1.0, max_score_gap=8.0,
                                score_mode="promote evaluated independently; does not enable reranking in the installed IME"),
                    notes="Offline, sequential stdio. Times include Python/JSON/stdio, except native core/lookup. Not TSF/UI latency. OS file cache was not flushed. Accuracy uses round zero; errors remain in denominators. Corpus text is saved explicitly for this evaluation.")
    metadata_file = output / "metadata.json"
    metadata_file.write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
    processes, logs, rows = [], [], []
    try:
        def log(name):
            stream = (output / name).open("w", encoding="utf-8")
            logs.append(stream)
            return stream
        core = JsonProcess([args.core, args.dictionary], log("core.stderr.txt"))
        processes.append(core)
        if core.hello.get("hello") != "nskkzime-eval-core":
            raise ValueError("Invalid native core helper")
        metadata["dictionary"] = core.hello
        key_result, _ = core.request(dict(op="keys", rounds=args.key_rounds))
        if "error" in key_result:
            raise ValueError("Core benchmark failed")
        (output / "core.json").write_text(json.dumps(key_result), encoding="utf-8")
        metadata["core_keys"] = latency(key_result["key_ms"])
        metadata["core_conversion_keys"] = latency(key_result["conversion_key_ms"])
        lookups = []
        for case in cases:
            found, _ = core.request(dict(op="lookup", reading=case.get("lookup", case["kana"]),
                                        okuri_letter=case.get("okuri_letter", ""), okuri=case.get("okuri", ""), repeats=20))
            if "error" in found:
                raise ValueError(f"Lookup failed: {case['id']}")
            case["dictionary"] = found["candidates"]
            lookups.append(dict(id=case["id"], **found))
        (output / "lookups.json").write_text(json.dumps(lookups, ensure_ascii=False), encoding="utf-8")
        for label, predicate in (("all", lambda x: True), ("hit", lambda x: bool(x["candidates"])), ("miss", lambda x: not x["candidates"])):
            metadata["dictionary_lookup_" + label] = latency([ms for x in lookups if predicate(x) for ms in x["lookup_ms"]])
        core.close()
        processes.remove(core)
        servers = {}
        for operation, deadline in (("convert", 1000), ("score", 300)):
            server = ModelServer(args.server, args.model, args.threads, deadline, log(operation + ".stderr.txt"))
            processes.append(server)
            servers[operation] = server
            metadata[operation + "_startup_ms"] = server.startup_ms
            server.infer(operation, "かいとう", "試験問題の", "", ["回答", "解答", "解凍"])
        with (output / "requests.jsonl").open("w", encoding="utf-8") as raw:
            def emit(row):
                rows.append(row)
                raw.write(json.dumps(row, ensure_ascii=False, allow_nan=False) + "\n")
                raw.flush()
            for round_id in range(args.rounds):
                for number, case in enumerate(cases):
                    modes = ("none", "left") if case["corpus"] == "ajimee" else ("none", "left", "both")
                    words = case["dictionary"]
                    for mode in modes:
                        left = case.get("left", "")[-40:] if mode != "none" else ""
                        right = case.get("right", "")[:40] if mode == "both" else ""
                        for operation in ("convert", "score"):
                            if operation == "score" and len(words) < 2:
                                continue
                            if operation == "convert" and case.get("okuri"):
                                continue  # The IME generates only okuri-nashi.
                            sample = servers[operation].infer(operation, case["kana"], left, right, words[:20])
                            usable = "error" not in sample["response"] and sample["within_deadline"] and allowed_reading(case["kana"])
                            order, filtered = list(words), []
                            if operation == "score" and usable:
                                order = promote(words, sample["response"]["scores"])
                            elif operation == "convert" and usable:
                                filtered = filtered_generation(case["kana"], sample["response"]["candidates"], sample["response"]["scores"])
                                order = merge(filtered, words) if len(case["kana"]) >= 10 else merge(words, filtered)
                            emit(dict(id=case["id"], corpus=case["corpus"], operation=operation, context=mode, round=round_id,
                                      kana=case["kana"], left=left, right=right, gold=case["gold"], dictionary=words,
                                      eligible=allowed_reading(case["kana"]), order=order, filtered=filtered, **sample))
                    if (number + 1) % 25 == 0:
                        print(f"round {round_id + 1}/{args.rounds}: {number + 1}/{len(cases)} cases", flush=True)
            for round_id in range(args.stress_rounds):
                for length in (32, 64, 128, 256, 512, 1024):
                    kana = ("きょうはいいてんきです。" * 100)[:length]
                    emit(dict(id=f"reading-{length}", corpus="stress", operation="convert", context="none", round=round_id,
                              **servers["convert"].infer("convert", kana, "", "")))
                for size in (2, 20, 64):
                    candidates = ["解答" + str(i) for i in range(size)]
                    emit(dict(id=f"candidates-{size}", corpus="stress", operation="score", context="none", round=round_id,
                              **servers["score"].infer("score", "かいとう", "", "", candidates)))
        for process in processes:
            process.close()
        processes.clear()
        metadata["complete"] = True
    finally:
        for process in processes:
            process.close(force=True)
        for stream in logs:
            stream.close()
        metadata["requests"] = len(rows)
        metadata_file.write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
        write_report(output, metadata, rows)
        print(f"Report: {output}", flush=True)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dictionary", type=Path, help="Required for collection: read-only SKK dictionary; no personal default")
    parser.add_argument("--report", type=Path, help="Recompute derived summaries of a completed run; do not run models")
    parser.add_argument("--ajimee", type=Path, help="Pinned AJIMEE evaluation_items.json")
    parser.add_argument("--cases", type=Path, default=ROOT / "tests/fixtures/quality-cases.json")
    parser.add_argument("--server", type=Path, default=ROOT / "out/build/windows-x64-zenz/Release/nkkz-ime-server.exe")
    parser.add_argument("--core", type=Path, default=ROOT / "out/build/windows-x64-zenz/Release/nskkzime_eval_core.exe")
    parser.add_argument("--model", type=Path, default=ROOT / "out/models/zenz-v3.2-small/ggml-model-Q5_K_M.gguf")
    parser.add_argument("--threads", type=int, choices=range(1, 65), default=4)
    parser.add_argument("--rounds", type=int, choices=range(1, 31), default=3)
    parser.add_argument("--stress-rounds", type=int, choices=range(1, 31), default=5)
    parser.add_argument("--key-rounds", type=int, choices=range(1, 10001), default=1000)
    parser.add_argument("--output", type=Path, help="New directory; existing reports are never overwritten")
    args = parser.parse_args()
    if args.report:
        output = args.report.resolve(strict=True)
        metadata = strict_json((output / "metadata.json").read_text(encoding="utf-8"))
        if not metadata.get("complete"):
            parser.error("Cannot present an incomplete collection as a completed report")
        rows = [strict_json(line) for line in (output / "requests.jsonl").read_text(encoding="utf-8").splitlines()]
        if len(rows) != metadata["requests"]:
            parser.error("Recorded request count mismatch")
        metadata["analysis_script_sha256"] = digest(__file__)
        write_report(output, metadata, rows)
        (output / "metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"Report: {output}")
        return
    if not args.dictionary:
        parser.error("--dictionary is required when collecting")
    for field in ("dictionary", "ajimee", "cases", "server", "core", "model"):
        if getattr(args, field):
            setattr(args, field, getattr(args, field).resolve(strict=True))
    evaluate(args)


if __name__ == "__main__":
    main()
