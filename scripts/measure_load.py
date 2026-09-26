#!/usr/bin/env python3
"""Mede casos do modb_load com a metodologia do plano de profiling e agrega.

Regras aplicadas (docs-process/PLANO_TAREFAS_DESEMPENHO.md, "Regras"):
- um processo por repetição de cada caso (evita M5, contaminação por ordem);
- work dir novo e vazio por execução, apagado depois;
- ordem dos casos alternada entre repetições (A B C / C B A / ...);
- `--no-index`: nada vai para a série histórica permanente.

Saída: `summary.json` e `summary.md` no diretório `--out`, com média e CV por
caso e fase de `ops_per_second`, `engine_ops_per_second`, bytes de WAL por
operação e, quando o binário é do preset `stage-profile`, ns/op, chamadas/op e
unidades/op de cada estágio e envelope.

Uso:
    python scripts/measure_load.py --binary build/stage-profile/modb_load.exe \\
        --case load.mixed_oltp.embedded.10k --case load.snapshot_hold.embedded.10k \\
        --repeat 5 --out <dir> [--label nome] [-- <args extras do modb_load>]

Um `--case` pode levar seletores próprios, e cada um vira um processo à parte:
    --case "load.create_only.embedded.10k --batch 1 --durability disabled_diagnostic"
e pode começar com `@<binário>` para usar outro build só nele (A/B alternado):
    --case "@build/ab-base/modb_load.exe load.mixed_oltp.embedded.10k"
"""

from __future__ import annotations

import argparse
import json
import shutil
import statistics
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def mean_cv(values: list[float]) -> tuple[float, float]:
    if not values:
        return float("nan"), float("nan")
    m = statistics.fmean(values)
    if len(values) < 2 or m == 0:
        return m, 0.0
    return m, statistics.stdev(values) / m


def run_once(binary: Path, spec: str, out_dir: Path, work_root: Path, seed: int, extra: list[str]) -> Path:
    # `spec` é o id do caso, opcionalmente seguido de seletores só dele:
    # "load.create_only.embedded.10k --batch 1 --durability disabled_diagnostic".
    tokens = spec.split()
    # "@caminho/modb_load.exe" no início troca o binário só para este caso --
    # é como um A/B de dois builds roda alternado na mesma sessão.
    if tokens and tokens[0].startswith("@"):
        binary = Path(tokens.pop(0)[1:]).resolve()
    case, *case_args = tokens
    work = work_root / f"w-{time.time_ns()}"
    work.mkdir(parents=True)
    raw = out_dir / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    before = set(raw.glob("*.jsonl"))
    cmd = [
        str(binary), "run", "--profile", "load-local", "--case", case,
        "--output-dir", str(raw), "--work-dir", str(work),
        "--no-index", "--seed", str(seed), "--accept-unknown-budget", *case_args, *extra,
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    produced = sorted(set(raw.glob("*.jsonl")) - before)
    if proc.returncode != 0 or not produced:
        sys.stderr.write(proc.stdout[-2000:] + proc.stderr[-2000:])
        raise SystemExit(f"falhou: {' '.join(cmd)} (código {proc.returncode})")
    return produced[-1]


def parse(path: Path) -> dict[str, dict]:
    """Por fase: {'summary': phase_summary, 'profile': stage_profile | None}."""
    phases: dict[str, dict] = defaultdict(dict)
    status = None
    for line in path.read_text(encoding="utf-8").splitlines():
        rec = json.loads(line)
        kind = rec.get("record")
        if kind == "phase_summary":
            phases[rec["phase"]]["summary"] = rec
        elif kind == "stage_profile":
            phases[rec["phase"]]["profile"] = rec
        elif kind == "case_summary":
            status = rec.get("status")
        elif kind == "case_error":
            # O processo sai com 0 e o JSONL fecha "completed" mesmo quando o
            # caso não roda (ex.: alvo sem implementação): falhar alto aqui.
            raise SystemExit(f"{path}: {rec.get('case_id')}: {rec.get('error')}")
    if status not in (None, "completed"):
        raise SystemExit(f"{path}: caso terminou com status {status}")
    return phases


def aggregate(runs: dict[str, list[dict]]) -> dict:
    result: dict = {}
    for case, reps in runs.items():
        phase_names = list(reps[0].keys())
        case_out = {}
        for phase in phase_names:
            samples = [r[phase] for r in reps if phase in r]
            summ = [s["summary"] for s in samples if "summary" in s]
            ops = [s["ops_per_second"] for s in summ]
            eng = [s["engine_ops_per_second"] for s in summ if s.get("engine_ops_per_second")]
            wal = [s["wal_bytes"] / s["operations"] for s in summ if s.get("operations")]
            entry = {
                "repetitions": len(summ),
                "operations": summ[0]["operations"] if summ else None,
                "ops_per_second": mean_cv(ops),
                "engine_ops_per_second": mean_cv(eng),
                "duration_s": mean_cv([s["duration_ns"] / 1e9 for s in summ]),
                "wal_bytes_per_op": mean_cv(wal),
                "harness_overhead_fraction": mean_cv([s.get("harness_overhead_fraction", 0) for s in summ]),
            }
            profs = [s["profile"] for s in samples if s.get("profile")]
            if profs:
                entry["attributed_fraction"] = mean_cv([p["attributed_fraction"] for p in profs])
                for group in ("stages", "envelopes"):
                    names = sorted({n for p in profs for n in p.get(group, {})})
                    entry[group] = {
                        n: {
                            "ns_per_operation": mean_cv([p[group].get(n, {}).get("ns_per_operation", 0) for p in profs]),
                            "calls_per_operation": mean_cv([p[group].get(n, {}).get("calls_per_operation", 0) for p in profs]),
                            "units_per_operation": mean_cv([p[group].get(n, {}).get("units_per_operation", 0) for p in profs]),
                            "max_ns": max(p[group].get(n, {}).get("max_ns", 0) for p in profs),
                        }
                        for n in names
                    }
            case_out[phase] = entry
        result[case] = case_out
    return result


def fmt(pair: tuple[float, float], digits: int = 0) -> str:
    m, cv = pair
    if m != m:  # nan
        return "—"
    return f"{m:,.{digits}f} ({cv * 100:.1f}%)".replace(",", ".") if digits == 0 else f"{m:.{digits}f} ({cv * 100:.1f}%)"


def markdown(summary: dict, label: str) -> str:
    lines = [f"# {label}", ""]
    for case, phases in summary.items():
        lines += [f"## {case}", "", "| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |",
                  "|---|---|---|---|---|---|---|"]
        for phase, e in phases.items():
            attr = f"{e['attributed_fraction'][0] * 100:.1f}%" if "attributed_fraction" in e else "—"
            lines.append(
                f"| {phase} | {e['repetitions']} | {fmt(e['ops_per_second'])} | {fmt(e['engine_ops_per_second'])} | "
                f"{e['wal_bytes_per_op'][0]:,.0f} | {e['harness_overhead_fraction'][0] * 100:.1f}% | {attr} |".replace(",", ".")
            )
        lines.append("")
        for phase, e in phases.items():
            if "stages" not in e:
                continue
            rows = [(n, v, "folha") for n, v in e["stages"].items()] + [(n, v, "envelope") for n, v in e["envelopes"].items()]
            rows = [r for r in rows if r[1]["ns_per_operation"][0] > 0]
            rows.sort(key=lambda r: -r[1]["ns_per_operation"][0])
            lines += [f"### {case} · {phase} — estágios", "",
                      "| estágio | tipo | ns/op (CV) | chamadas/op | unidades/op | max_ns |", "|---|---|---|---|---|---|"]
            for n, v, kind in rows:
                lines.append(
                    f"| `{n}` | {kind} | {fmt(v['ns_per_operation'])} | {v['calls_per_operation'][0]:.3f} | "
                    f"{v['units_per_operation'][0]:.1f} | {v['max_ns']:,} |".replace(",", ".")
                )
            lines.append("")
    return "\n".join(lines) + "\n"


def main() -> int:
    argv = sys.argv[1:]
    extra: list[str] = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--case", action="append", required=True)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--label", default="medição")
    args = ap.parse_args(argv)
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8")

    binary = args.binary.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    runs: dict[str, list[dict]] = defaultdict(list)
    files: dict[str, list[str]] = defaultdict(list)
    for rep in range(args.repeat):
        order = args.case if rep % 2 == 0 else list(reversed(args.case))
        for case in order:
            t0 = time.time()
            path = run_once(binary, case, out, out / "work", args.seed, extra)
            runs[case].append(parse(path))
            files[case].append(path.name)
            print(f"[rep {rep + 1}/{args.repeat}] {case}: {time.time() - t0:.1f}s", flush=True)

    summary = aggregate(runs)
    meta = {"label": args.label, "binary": str(binary), "repeat": args.repeat, "seed": args.seed,
            "extra_args": extra, "raw_files": files}
    (out / "summary.json").write_text(json.dumps({"meta": meta, "cases": summary}, indent=1), encoding="utf-8")
    md = markdown(summary, args.label)
    (out / "summary.md").write_text(md, encoding="utf-8")
    print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
