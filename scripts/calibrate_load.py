#!/usr/bin/env python3
"""Gera a tabela de calibração do modb_load medindo de verdade cada escala.

A calibração é o que `--max-duration`/`--max-disk-gb`/`--dry-run` usam para
estimar um caso antes de rodá-lo (docs/PLANO_TESTES_DE_CARGA.md §10). Um arquivo
por plataforma e tipo de build (T22 de docs-process/PLANO_TAREFAS_DESEMPENHO.md):
a tabela medida em Debug superestimava um binário otimizado em ~2,5x.

Cada ponto é uma execução real, num processo próprio e num work dir novo --
nada é extrapolado. O `modb_load` já faz o opt-out de power throttling (T11).

Uso:
    python scripts/calibrate_load.py --binary build/relwithdebinfo/modb_load.exe \\
        --build-type RelWithDebInfo --out loadtests/calibration/windows-x86_64-RelWithDebInfo.json
"""

from __future__ import annotations

import argparse
import datetime
import json
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

WORKLOADS = ["create_only", "create_delete_forward", "create_delete_reverse",
             "create_delete_interleaved", "crud_full"]
SCALES = ["1k", "10k", "100k", "250k", "500k", "1M"]


def run_case(binary: Path, case: str, scratch: Path) -> dict:
    work = scratch / f"w-{time.time_ns()}"
    raw = scratch / f"raw-{time.time_ns()}"
    work.mkdir(parents=True)
    raw.mkdir(parents=True)
    try:
        proc = subprocess.run(
            [str(binary), "run", "--profile", "load-local", "--case", case, "--no-index",
             "--seed", "1", "--accept-unknown-budget", "--work-dir", str(work), "--output-dir", str(raw)],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        files = list(raw.glob("*.jsonl"))
        if proc.returncode != 0 or not files:
            raise SystemExit(f"{case}: falhou ({proc.returncode})\n{proc.stdout[-1500:]}{proc.stderr[-1500:]}")
        summary, peak_rss, errors = None, 0, []
        for line in files[0].read_text(encoding="utf-8").splitlines():
            rec = json.loads(line)
            if rec.get("record") == "case_summary":
                summary = rec
            elif rec.get("record") == "phase_summary":
                peak_rss = max(peak_rss, int(rec.get("peak_rss_bytes", 0)))
            elif rec.get("record") == "case_error":
                errors.append(rec.get("error"))
        if errors or summary is None or summary.get("status") != "completed":
            raise SystemExit(f"{case}: não completou: {errors or summary}")
        return {"source": "measured", "duration_ns": int(summary["total_duration_ns"]),
                "disk_peak_bytes": int(summary["peak_disk_bytes"]), "peak_rss_bytes": peak_rss}
    finally:
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(raw, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--build-type", required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--scales", default=",".join(SCALES))
    args = ap.parse_args()
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8")

    scales = args.scales.split(",")
    entries = []
    with tempfile.TemporaryDirectory(prefix="modb-calibrate-") as tmp:
        for workload in WORKLOADS:
            points = {}
            for scale in scales:
                t0 = time.time()
                points[scale] = run_case(args.binary.resolve(), f"load.{workload}.embedded.{scale}", Path(tmp))
                print(f"{workload:28s} {scale:>5s}: {points[scale]['duration_ns'] / 1e9:8.2f} s "
                      f"(parede {time.time() - t0:.1f} s)", flush=True)
            entries.append({"workload": workload, "payload": "normal", "measured_scales": scales,
                            "scales": points})

    doc = {
        "schema": "modb.loadtest.calibration",
        "schema_version": 1,
        "platform": "windows" if platform.system() == "Windows" else platform.system().lower(),
        "arch": "x86_64",
        "build_type": args.build_type,
        "measured_at": datetime.date.today().isoformat(),
        "notes": ("Gerado por scripts/calibrate_load.py: cada escala medida de verdade, uma execução "
                  "por ponto, processo e work dir próprios, com opt-out de power throttling (T11). "
                  "Nenhum ponto extrapolado. Ver docs-process/PROFILING_2026-09.md, T22."),
        "entries": entries,
    }
    args.out.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"gravado: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
