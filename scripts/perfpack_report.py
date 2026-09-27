#!/usr/bin/env python3
"""Relatório visual de rodadas do pacote de performance: desempenho + uso de hardware.

Lê rodadas já trazidas por `perfpack.py fetch` (load-results/remote/<ambiente>/<rodada>/)
e gera uma página HTML autocontida com gráficos numa mesma linha do tempo:
ops/s do motor por fase, CPU do processo, iowait, steal, disco e memória, uma
cor por rodada. Um seletor alterna entre o tempo real (rodadas em sequência) e
as rodadas sobrepostas a partir de t=0. Sem dependência externa: SVG e JS
inline, abre offline.

Uso:
    python scripts/perfpack_report.py -e do-cpuopt-4-nyc3 RODADA [RODADA...] [--out ARQ.html]
    python scripts/perfpack_report.py -e do-cpuopt-4-nyc3 --last 2
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import html
import json
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FETCHED = ROOT / "load-results" / "remote"


def parse_stamp(stamp: str) -> float:
    """'20260927T114808.123Z' (modb_load) -> epoch em segundos."""
    fmt = "%Y%m%dT%H%M%S.%fZ" if "." in stamp else "%Y%m%dT%H%M%SZ"
    return dt.datetime.strptime(stamp, fmt).replace(tzinfo=dt.timezone.utc).timestamp()


def read_hw(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with path.open(encoding="utf-8") as f:
        return [{k: float(v) for k, v in row.items()} for row in csv.DictReader(f, delimiter="\t")]


def load_run(run_dir: Path) -> dict:
    manifest = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8-sig"))
    with (run_dir / "executions.tsv").open(encoding="utf-8") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    executions = []
    prev_end = parse_stamp(manifest["started_at"].replace("-", "").replace(":", ""))
    for row in rows:
        if row["status"] != "completed" or not row["file"]:
            # Execução que falhou (ex.: morta por falta de memória): sem fases, mas
            # com o uso de hardware até o fim — muitas vezes é o que explica a falha.
            case_id = row["case"]
            samples = read_hw(run_dir / "logs" / f"hw-r{row['repetition']}-c{row['index']}-{case_id}.tsv")
            start = fnum(row.get("start_epoch")) or prev_end  # rodadas antigas não gravam o início
            for s in samples:
                s["t"] = start + s.pop("t_s")
            end = fnum(row.get("end_epoch")) or (samples[-1]["t"] if samples else start)
            executions.append({"case": case_id, "rep": int(row["repetition"]), "start": start, "end": end,
                               "phases": [], "hw": samples, "case_info": {}, "failed": row["status"],
                               "summary": {k: row.get(k, "-") for k in ("proc_cpu_avg_pct", "rss_max_MiB",
                                                                        "mem_avail_min_MiB")}})
            prev_end = end
            continue
        start = end = None
        phases = []
        case_info: dict = {}
        case_id = row["case"]
        prev_wal = 0
        for line in (run_dir / row["file"]).read_text(encoding="utf-8").splitlines():
            rec = json.loads(line)
            kind = rec.get("record")
            if kind == "run_start":
                start = parse_stamp(rec["started_at"])
            elif kind == "phase_summary":
                ops_n = rec.get("operations") or 0
                lat = rec.get("latency_ns") or {}
                wal = rec.get("wal_bytes") or 0
                # wal_bytes é acumulado no caso: o da fase é a diferença.
                phases.append({"phase": rec["phase"], "duration": rec["duration_ns"] / 1e9,
                               "ops": rec.get("ops_per_second"),
                               # Nem todo workload separa o tempo do motor: sem ele, vale o total.
                               "engine": rec.get("engine_ops_per_second") or rec.get("ops_per_second"),
                               "operations": ops_n,
                               "p50_us": lat.get("p50", 0) / 1e3, "p95_us": lat.get("p95", 0) / 1e3,
                               "p99_us": lat.get("p99", 0) / 1e3, "p999_us": lat.get("p999", 0) / 1e3,
                               "wal_per_op": (wal - prev_wal) / ops_n if ops_n else 0,
                               "db_mib": (rec.get("db_bytes") or 0) / 2**20,
                               "bytes_per_object": rec.get("bytes_per_object") or 0,
                               "rss_mib": (rec.get("peak_rss_bytes") or 0) / 2**20,
                               "overhead_pct": (rec.get("harness_overhead_fraction") or 0) * 100,
                               "pages_written": rec.get("pages_written_estimated") or 0})
                prev_wal = wal
            elif kind == "case_summary":
                end = start + rec["total_duration_ns"] / 1e9 if start is not None else None
                case_info = {"write_amplification": rec.get("write_amplification"),
                             "peak_disk_mib": (rec.get("peak_disk_bytes") or 0) / 2**20,
                             # Workloads que apagam tudo não calculam hash (vem vazio): a
                             # conferência deles é não sobrar nenhum objeto resolvível.
                             "correct": (rec.get("hash_match") is True) if rec.get("expected_hash")
                             else (rec.get("all_deleted") is not False and not rec.get("still_resolving"))}
        if start is None:
            continue
        # As fases rodam em sequência a partir do início do caso; o que o harness
        # faz fora delas (dataset, conferência de hash) fica no fim do intervalo.
        cursor = start
        for p in phases:
            p["start"], p["end"] = cursor, cursor + p["duration"]
            cursor = p["end"]
        hw_name = f"hw-r{row['repetition']}-c{row['index']}-{case_id}.tsv"
        samples = read_hw(run_dir / "logs" / hw_name)
        for s in samples:
            s["t"] = start + s.pop("t_s")
        # O processo continua depois das fases medidas (conferência do estado final,
        # fechamento): a execução vai até o fim dele, não até o fim da última fase.
        phases_end = end or cursor
        proc_end = fnum(row.get("end_epoch")) or (samples[-1]["t"] if samples else phases_end)
        executions.append({"case": case_id, "rep": int(row["repetition"]), "start": start,
                           "end": max(phases_end, proc_end), "phases_end": cursor,
                           "phases": phases, "hw": samples, "case_info": case_info,
                           "summary": {k: row.get(k, "-") for k in (
                               "proc_cpu_avg_pct", "proc_cpu_max_pct", "steal_max_pct", "iowait_avg_pct",
                               "disk_read_avg_MBps", "disk_write_avg_MBps", "rss_max_MiB")}})
        prev_end = end or cursor
    executions.sort(key=lambda e: e["start"])
    # A rodada é chamada pela escala que domina seus casos (100k, 1Mi), não pelo
    # nome da suíte: é o que o leitor compara.
    scales = [e["case"].split(".")[-1] for e in executions]
    label = max(set(scales), key=scales.count) if scales else manifest["suite"]
    label = {"1M": "1Mi", "5M": "5Mi"}.get(label, label)
    return {"name": manifest["run_name"], "suite": label, "suite_name": manifest["suite"], "repeat": manifest["repeat"],
            "commit": manifest["git_commit"][:12], "environment": manifest["environment"],
            "started_at": manifest["started_at"], "finished_at": manifest["finished_at"],
            "steal_max": (manifest.get("hardware_monitor") or {}).get("steal_max_pct"),
            "executions": executions}


def mean_cv(values: list[float]) -> tuple[float, float]:
    values = [v for v in values if v]
    if not values:
        return 0.0, 0.0
    m = statistics.fmean(values)
    return m, (statistics.stdev(values) / m * 100 if len(values) > 1 and m else 0.0)


PHASE_METRICS = ("engine", "ops", "duration", "p50_us", "p95_us", "p99_us", "p999_us", "wal_per_op", "db_mib",
                 "bytes_per_object", "rss_mib", "overhead_pct", "pages_written")


# O que cada teste faz, para o ícone (i) de cada caso (docs/PLANO_TESTES_DE_CARGA.md §4.2).
WORKLOAD_INFO = {
    "create_only": ("Ingestão pura: cria N usuários em lotes de 1000 por commit.",
                    "create", "Crescimento do arquivo e do WAL e o custo de cada commit.",
                    "ops/s = objetos criados por segundo. Conferência: contagem e hash do conjunto."),
    "crud_full": ("Ciclo completo sobre os N objetos, uma fase de cada vez.",
                  "create → read → update_inplace (mesmo tamanho) → update_grow (registro maior) → "
                  "update_shrink (menor) → delete",
                  "Leitura, reescrita de registros, movimentação entre páginas e remoção.",
                  "ops/s por fase = objetos processados por segundo. Conferência: valores lidos e contagem final zero."),
    "create_delete_forward": ("Cria N e apaga na ordem de criação (FIFO).", "create → delete",
                              "Remoção com localidade perfeita.", "Conferência: nenhum id restante."),
    "create_delete_reverse": ("Cria N e apaga na ordem inversa (LIFO).", "create → delete",
                              "Compactação da última página.", "Conferência: nenhum id restante."),
    "create_delete_interleaved": ("Cria N e apaga em ordem espalhada (passo/Zipf com semente).", "create → delete",
                                  "Fragmentação real, lista de páginas livres e reuso parcial.",
                                  "Conferência: nenhum objeto restante (não há hash: tudo é apagado)."),
    "read_hotspot": ("Cria N e lê repetidamente um conjunto quente (distribuição Zipf).", "create → read_hotspot",
                     "Buffer pool e cache de páginas sob leituras concentradas.",
                     "ops/s = leituras por segundo (3 leituras por objeto). Conferência: hash na ordem lida."),
    "range_scan_sweep": ("Cria N com índice em User.id e faz buscas por faixa de seletividade crescente.",
                         "create → scan 0,01% → 0,1% → 1% → 10% → 100%",
                         "Índice contra varredura: o nome da fase traz o plano escolhido (index_scan).",
                         "ops/s = objetos devolvidos por segundo em cada seletividade."),
    "mixed_oltp": ("Operações misturadas numa fase só: 10 leituras por escrita; escritas divididas "
                   "25% create / 50% update / 25% delete, cada uma na sua transação.",
                   "create (carga inicial) → mixed_oltp",
                   "Commit pequeno e frequente: um fsync do WAL por operação de escrita; latência de cauda.",
                   "ops/s = operações da mistura por segundo. É limitado pelo disco, não pela CPU."),
    "snapshot_hold": ("Abre um snapshot, relê tudo por ele, altera o estado vivo (update/delete/create) e relê "
                      "pelo mesmo snapshot, que precisa continuar idêntico; depois fecha e roda o GC.",
                      "create → snapshot_read_fresh → hold (churn com o snapshot aberto) → snapshot_read_retained",
                      "Retenção de versões do MVCC com volume real e o GC ao fechar.",
                      "hold é churn com commit por operação (limitado pelo fsync); as leituras pelo snapshot "
                      "medem o custo de resolver versões retidas."),
    "blob_lifecycle": ("Ciclo de blobs grandes no BlobStore (64 KiB, 1 MiB e 16 MiB), conferidos byte a byte.",
                       "create → read → update_grow → update_shrink → delete",
                       "Cadeia de páginas de blob, leitura por streaming.",
                       "ops/s = blobs por segundo (por isso números pequenos). O BlobStore não recupera espaço "
                       "ao apagar (sem lista livre) — achado conhecido."),
    "cascade_delete": ("Cria uma árvore de posse (profundidade 4, largura ≈ N^¼) e remove a raiz: o motor apaga "
                       "a árvore inteira em cascata, numa única transação.",
                       "create_hierarchy → cascade_delete",
                       "Integridade referencial e custo da remoção em cascata com o número de descendentes.",
                       "ops/s = nós removidos por segundo. A remoção é UMA transação com todos os nós."),
    "oversubscribed_churn": ("Igual a create_delete_interleaved, mas com o cache do banco em ~10% das páginas "
                             "do conjunto: força despejo e releitura.", "create → delete",
                             "Degradação quando o volume passa do cache.",
                             "Compare com create_delete_interleaved: a diferença é o custo de não caber no cache."),
    "restart_recovery": ("Churn normal, depois um commit interrompido de propósito (durável no WAL, páginas não "
                         "aplicadas); fecha e reabre o banco, forçando o replay do WAL.",
                         "create → restart_recovery (reabertura com replay)",
                         "Custo e correção da recuperação conforme o volume.",
                         "A fase de recuperação é uma operação só: olhe a duração, não ops/s. "
                         "Conferência: hash após recuperar == hash do último commit durável."),
}


def split_case(case: str) -> tuple[str, str]:
    """'load.crud_full.embedded.100k' -> ('crud_full', '100k')."""
    parts = case.split(".")
    return (parts[1], parts[-1]) if len(parts) >= 4 else (case, "")


def fnum(text: str) -> float | None:
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def phase_table(run: dict) -> list[dict]:
    groups: dict[tuple[str, str], list[dict]] = {}
    per_case: dict[str, list[dict]] = {}
    for e in run["executions"]:
        per_case.setdefault(e["case"], []).append(e)
        for p in e["phases"]:
            groups.setdefault((e["case"], p["phase"]), []).append(p)
    rows = []
    for (case, phase), ps in groups.items():
        workload, scale = split_case(case)
        row = {"case": case, "workload": workload, "scale": scale, "phase": phase, "n": len(ps)}
        for metric in PHASE_METRICS:
            row[metric], row[metric + "_cv"] = mean_cv([p[metric] for p in ps])
        row["cv"] = row["engine_cv"]
        # Uso de hardware e amplificação são do caso inteiro (todas as fases juntas).
        execs = per_case[case]
        for key, col in (("cpu", "proc_cpu_avg_pct"), ("iowait", "iowait_avg_pct"), ("wmb", "disk_write_avg_MBps")):
            vals = [v for v in (fnum(e["summary"].get(col)) for e in execs) if v is not None]
            row[key] = statistics.fmean(vals) if vals else None
        wa = [e["case_info"].get("write_amplification") for e in execs if e["case_info"].get("write_amplification")]
        row["write_amplification"] = statistics.fmean(wa) if wa else None
        row["hash_ok"] = all(e["case_info"].get("correct", True) for e in execs)
        rows.append(row)
    return rows


TEMPLATE = r"""<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Rodadas moDb</title>
<style>
.viz-root{color-scheme:light;--surface-0:#f4f3f0;--surface-1:#fcfcfb;--text-primary:#0b0b0b;--text-secondary:#52514e;
--text-muted:#7a7974;--grid:#e6e5e1;--axis:#bdbcb7;--band-a:#e9e8e4;--band-b:#f1f0ec;--series-1:#2a78d6;--series-2:#eb6834}
@media (prefers-color-scheme:dark){:root:where(:not([data-theme="light"])) .viz-root{color-scheme:dark;--surface-0:#121211;
--surface-1:#1a1a19;--text-primary:#fff;--text-secondary:#c3c2b7;--text-muted:#8f8e86;--grid:#2c2c2a;--axis:#4a4a46;
--band-a:#2a2a28;--band-b:#222220;--series-1:#3987e5;--series-2:#d95926}}
:root[data-theme="dark"] .viz-root{color-scheme:dark;--surface-0:#121211;--surface-1:#1a1a19;--text-primary:#fff;
--text-secondary:#c3c2b7;--text-muted:#8f8e86;--grid:#2c2c2a;--axis:#4a4a46;--band-a:#2a2a28;--band-b:#222220;
--series-1:#3987e5;--series-2:#d95926}
*{box-sizing:border-box}html,body{margin:0}
body{background:var(--surface-0);font:14px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
.viz-root{background:var(--surface-0);color:var(--text-primary);min-height:100vh;padding:24px 16px 48px}
.wrap{max-width:1180px;margin:0 auto}
h1{font-size:20px;margin:0 0 4px}h2{font-size:16px;margin:32px 0 4px}
.lead{color:var(--text-secondary);margin:0 0 14px;max-width:820px}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:10px;margin:0 0 16px}
.tile{background:var(--surface-1);border-radius:10px;padding:12px 14px}
.tile .who{display:flex;align-items:center;gap:8px;color:var(--text-secondary);font-size:12.5px;margin-bottom:6px}
.tile .nums{display:grid;grid-template-columns:repeat(3,1fr);gap:6px}
.tile .num{font-size:18px;font-weight:650;font-variant-numeric:tabular-nums}
.tile .lbl{font-size:11.5px;color:var(--text-muted)}
.controls{display:flex;flex-wrap:wrap;gap:16px;align-items:center;margin:0 0 10px}
.seg{display:inline-flex;border:1px solid var(--axis);border-radius:8px;overflow:hidden}
.seg button{background:var(--surface-1);color:var(--text-secondary);border:0;padding:6px 12px;font:inherit;cursor:pointer}
.seg button[aria-pressed="true"]{background:var(--text-primary);color:var(--surface-1)}
.legend{display:flex;gap:18px;flex-wrap:wrap;color:var(--text-secondary);font-size:13px}
.legend span{display:inline-flex;align-items:center;gap:6px}
.key{display:inline-block;width:18px;height:3px;border-radius:2px}
.swatch{display:inline-block;width:12px;height:12px;border-radius:3px}
.card{background:var(--surface-1);border-radius:10px;padding:10px 12px 6px;margin:0 0 10px}
.card h3{font-size:13.5px;font-weight:600;margin:0}
.bar .grow{flex:1}
.bar{display:flex;align-items:center;gap:4px}
.grip{cursor:grab;color:var(--text-muted);user-select:none;padding:0 4px;font-size:15px;line-height:1}
.ib{background:none;border:1px solid transparent;border-radius:6px;color:var(--text-secondary);cursor:pointer;
padding:1px 7px;font:inherit;line-height:1.3}
.ib:hover,.ib:focus-visible{border-color:var(--axis)}
.card.dragging{opacity:.45}
.body{margin-top:2px}
.ib.info{font-weight:700;font-style:italic;font-family:Georgia,serif;width:22px;height:22px;padding:0;border-radius:50%;
border-color:var(--axis);color:var(--text-secondary);font-size:12px}
.ib.info[aria-expanded="true"]{background:var(--text-primary);color:var(--surface-1)}
.infobox{background:var(--surface-0);border-radius:8px;padding:8px 12px;margin:6px 0 8px;font-size:12.5px}
.infobox div{margin:2px 0}.infobox b{color:var(--text-secondary);font-weight:600;margin-right:4px}
.reset{background:var(--surface-1);color:var(--text-secondary);border:1px solid var(--axis);border-radius:8px;
padding:5px 10px;font:inherit;cursor:pointer}
.card p{font-size:12px;color:var(--text-muted);margin:0 0 4px}
.empty{font-size:12.5px;color:var(--text-muted);padding:18px 4px}
svg{display:block;width:100%;overflow:visible}
.tick{fill:var(--text-muted);font-size:11px}
.lane{fill:var(--text-secondary);font-size:11px}
.blabel{fill:var(--text-primary);font-size:11px;font-variant-numeric:tabular-nums}
.glabel{fill:var(--text-secondary);font-size:12px}
.tip{position:fixed;pointer-events:none;background:var(--surface-1);color:var(--text-primary);border:1px solid var(--axis);
border-radius:8px;padding:8px 10px;font-size:12px;box-shadow:0 4px 16px rgba(0,0,0,.18);display:none;z-index:10;max-width:360px}
.tip .v{font-weight:650}.tip .n{color:var(--text-secondary)}.tip .row{display:flex;gap:8px;align-items:center;white-space:nowrap}
.tip .hd{color:var(--text-secondary);margin:4px 0 2px}
table{border-collapse:collapse;width:100%;background:var(--surface-1);border-radius:10px;overflow:hidden;font-size:12.5px}
th,td{padding:6px 10px;text-align:right;border-bottom:1px solid var(--grid);font-variant-numeric:tabular-nums}
th:nth-child(-n+2),td:nth-child(-n+2){text-align:left}th{color:var(--text-secondary);font-weight:600}
.scroll{overflow-x:auto}
.nav{position:sticky;top:0;z-index:5;background:var(--surface-0);padding:8px 0;margin:0 0 14px;
border-bottom:1px solid var(--grid);display:flex;flex-wrap:wrap;gap:4px 14px;align-items:center;font-size:13px}
.nav a{color:var(--text-secondary);text-decoration:none}
.nav a:hover,.nav a:focus-visible{color:var(--text-primary);text-decoration:underline}
.nav .grp{color:var(--text-muted)}
[id]{scroll-margin-top:52px}
.insights{background:var(--surface-1);border-radius:10px;padding:10px 16px 10px 32px;margin:0 0 8px}
.insights li{margin:5px 0}.insights .topic{font-weight:650;margin-right:4px}
.infer{font-size:12.5px;color:var(--text-secondary);margin:6px 0 2px}.note{color:var(--text-muted);font-size:12px;margin-top:8px}
</style></head>
<body><div class="viz-root"><div class="wrap">
<nav class="nav" id="nav" aria-label="Seções do relatório"></nav>
<h1>Rodadas do pacote de performance</h1>
<p class="lead" id="sub"></p>
<div class="tiles" id="tiles"></div>

<h2 id="sec-insights">Leituras</h2>
<p class="lead">O que os números sustentam, calculado a partir deles (gargalo, efeito da escala, confiabilidade).</p>
<ul class="insights" id="insights"></ul>

<h2 id="sec-hw">1. O que a máquina fez durante os testes</h2>
<p class="lead">A faixa de cima diz qual caso estava rodando; os gráficos embaixo, o uso de hardware no mesmo instante.
Passe o mouse para ver o caso, a fase e os valores.</p>
<div class="controls">
  <div class="seg" role="group" aria-label="Eixo do tempo">
    <button id="m-zero" aria-pressed="true">Sobrepostas (t=0)</button><button id="m-real" aria-pressed="false">Tempo real</button>
  </div>
  <div class="legend" id="legend"></div>
  <button class="reset" id="reset" title="Volta à ordem original e expande todos os gráficos">Restaurar layout</button>
</div>
<div id="timeline"></div>

<h2 id="sec-cases">2. Cada caso ao longo da execução</h2>
<p class="lead">Um cartão por caso. Eixo x: tempo desde o início da execução. Cada degrau é uma fase, na altura da
métrica escolhida (o harness mede por fase, não segundo a segundo); embaixo, CPU e memória no mesmo instante.
Linhas finas: cada repetição.</p>
<div class="controls">
  <div class="seg" role="group" aria-label="Suítes mostradas" id="suite-seg"></div>
  <div class="seg" role="group" aria-label="Eixo x dos casos">
    <button id="x-sec" aria-pressed="true">Segundos</button><button id="x-pct" aria-pressed="false">% da execução</button>
  </div>
  <label class="legend" for="metric">Métrica&nbsp;<select id="metric"></select></label>
</div>
<div id="compare"></div>

<h2 id="sec-table">3. Tabela</h2>
<div class="scroll"><table id="tbl"><thead></thead><tbody></tbody></table></div>
<p class="note">Fases posicionadas em sequência a partir do início de cada execução. Uso de hardware: monitor do runner
(/proc, 2 amostras/s, sem carga na máquina). Execuções mais curtas que 0,5 s não têm amostras.</p>
</div><div class="tip" id="tip"></div></div>
<script>
const DATA = __DATA__;
const COLORS = ["var(--series-1)", "var(--series-2)"];
const HW = [
  {field:"proc_cpu_pct", title:"CPU do processo", unit:"%", note:"100% = um núcleo inteiro. Perto de 100%: o limite é o código.", floor:100},
  {field:"cpu_all_pct", title:"CPU da máquina", unit:"%", note:"% de todas as vCPUs juntas — a mesma medida do painel da Digital Ocean (lá em médias por minuto e hora local). Um núcleo cheio numa máquina de 4 vCPUs = 25%.", floor:100},
  {field:"iowait_pct", title:"iowait", unit:"%", note:"CPU parada esperando o disco. Subindo: o caso está esperando o fsync.", floor:5},
  {field:"disk_write_MBps", title:"Escrita em disco", unit:"MB/s", note:"No dispositivo do diretório de trabalho.", floor:1},
  {field:"rss_MiB", title:"Memória do processo (RSS)", unit:"MiB", floor:1},
];
const METRICS = [
  {key:"engine", label:"Motor: ops/s", unit:"ops/s", better:"maior"},
  {key:"ops", label:"Total: ops/s (com o harness)", unit:"ops/s", better:"maior"},
  {key:"p50_us", label:"Latência p50", unit:"µs", better:"menor"},
  {key:"p95_us", label:"Latência p95", unit:"µs", better:"menor"},
  {key:"p99_us", label:"Latência p99", unit:"µs", better:"menor"},
  {key:"p999_us", label:"Latência p99,9", unit:"µs", better:"menor"},
  {key:"wal_per_op", label:"WAL por operação", unit:"B/op", better:"menor"},
  {key:"db_mib", label:"Tamanho do banco ao fim da fase", unit:"MiB", better:"menor"},
  {key:"bytes_per_object", label:"Bytes por objeto", unit:"B", better:"menor"},
  {key:"rss_mib", label:"Memória de pico (RSS)", unit:"MiB", better:"menor"},
  {key:"overhead_pct", label:"Overhead do harness", unit:"%", better:"menor"},
  {key:"duration", label:"Duração da fase", unit:"s", better:"menor"},
];
let metric = METRICS[0];
// --- cartões: reordenar (botões ou arrastando a alça) e recolher. A ordem e os
// recolhidos ficam no navegador (localStorage) e sobrevivem a novos renders.
const STORE = "modb-perfpack-report-layout";
let layout = {order: {}, collapsed: {}};
try { layout = Object.assign(layout, JSON.parse(localStorage.getItem(STORE) || "{}")); } catch (e) {}
const saveLayout = () => { try { localStorage.setItem(STORE, JSON.stringify(layout)); } catch (e) {} };
let dragged = null;
function saveOrder(root){ layout.order[root.id] = [...root.children].map(c => c.dataset.id); saveLayout(); }
function applyOrder(root){
  const o = layout.order[root.id]; if (!o) return;
  const kids = [...root.children];
  kids.map((c, i) => [o.indexOf(c.dataset.id) < 0 ? o.length + i : o.indexOf(c.dataset.id), c])
      .sort((a, b) => a[0] - b[0]).forEach(([, c]) => root.appendChild(c));
}
function iconButton(bar, text, label){
  const b = document.createElement("button"); b.className = "ib"; b.textContent = text; b.setAttribute("aria-label", label); b.title = label;
  bar.appendChild(b); return b;
}
function makeCard(root, id, title, note, info){
  const card = document.createElement("div"); card.className = "card"; card.dataset.id = id; card.id = "card-" + id; root.appendChild(card);
  const bar = document.createElement("div"); bar.className = "bar"; card.appendChild(bar);
  const grip = document.createElement("span"); grip.className = "grip"; grip.textContent = "⠿"; grip.title = "Arraste para reordenar";
  grip.draggable = true; bar.appendChild(grip);
  const tog = iconButton(bar, "▾", "Recolher " + title);
  const h = document.createElement("h3"); h.textContent = title; bar.appendChild(h);
  let infoBtn = null;
  if (info) { infoBtn = iconButton(bar, "i", "O que este teste faz"); infoBtn.className = "ib info"; infoBtn.setAttribute("aria-expanded", "false"); }
  const grow = document.createElement("span"); grow.className = "grow"; bar.appendChild(grow);
  const up = iconButton(bar, "↑", "Mover " + title + " para cima");
  const down = iconButton(bar, "↓", "Mover " + title + " para baixo");
  const body = document.createElement("div"); body.className = "body"; card.appendChild(body);
  if (info) {
    const box = document.createElement("div"); box.className = "infobox"; box.hidden = true;
    [["O que faz", info.what], ["Fases", info.phases], ["O que exercita", info.stresses], ["Como ler", info.read]].forEach(([k, v]) => {
      const d = document.createElement("div"); const b = document.createElement("b"); b.textContent = k + ":"; d.appendChild(b); d.appendChild(document.createTextNode(" " + v)); box.appendChild(d);
    });
    body.appendChild(box);
    infoBtn.onclick = () => { box.hidden = !box.hidden; infoBtn.setAttribute("aria-expanded", String(!box.hidden)); if (!box.hidden && body.hidden) tog.onclick(); };
  }
  if (note) { const p = document.createElement("p"); p.textContent = note; body.appendChild(p); }
  const setCollapsed = c => {
    body.hidden = c; tog.textContent = c ? "▸" : "▾";
    tog.setAttribute("aria-expanded", String(!c)); tog.setAttribute("aria-label", (c ? "Expandir " : "Recolher ") + title); tog.title = tog.getAttribute("aria-label");
  };
  setCollapsed(!!layout.collapsed[id]);
  card.expand = () => { if (layout.collapsed[id]) { layout.collapsed[id] = false; setCollapsed(false); saveLayout(); } };
  tog.onclick = () => { layout.collapsed[id] = !layout.collapsed[id]; setCollapsed(layout.collapsed[id]); saveLayout(); };
  up.onclick = () => { const prev = card.previousElementSibling; if (prev) { root.insertBefore(card, prev); saveOrder(root); up.focus(); } };
  down.onclick = () => { const next = card.nextElementSibling; if (next) { root.insertBefore(next, card); saveOrder(root); down.focus(); } };
  grip.addEventListener("dragstart", ev => { dragged = card; card.classList.add("dragging"); ev.dataTransfer.effectAllowed = "move"; ev.dataTransfer.setData("text/plain", id); });
  grip.addEventListener("dragend", () => { card.classList.remove("dragging"); dragged = null; saveOrder(root); });
  card.addEventListener("dragover", ev => {
    if (!dragged || dragged === card || dragged.parentNode !== root) return;
    ev.preventDefault();
    const r = card.getBoundingClientRect();
    root.insertBefore(dragged, ev.clientY < r.top + r.height / 2 ? card : card.nextSibling);
  });
  card.addEventListener("drop", ev => ev.preventDefault());
  return body;
}
let mode = "zero";
const trim = x => x.replace(/\.0+$/, "").replace(/(\.\d*?)0+$/, "$1");
const fmt = (v, u) => {
  if (v == null || isNaN(v)) return "—";
  const a = Math.abs(v);
  const s = a >= 1e6 ? trim((v/1e6).toFixed(2))+" M" : a >= 1e4 ? trim((v/1e3).toFixed(1))+" mil" : a >= 100 ? v.toFixed(0) : a >= 10 ? trim(v.toFixed(1)) : trim(v.toFixed(2));
  return s + (u ? " " + u : "");
};
const short = c => c.replace("load.", "").replace(".embedded", "");
const hhmm = t => new Date(t*1000).toISOString().slice(11,19);
const mmss = t => { const s = Math.max(0, Math.round(t)); return Math.floor(s/60) + ":" + String(s%60).padStart(2,"0"); };
const base = r => mode === "zero" && r.executions.length ? r.executions[0].start : 0;
const NS = "http://www.w3.org/2000/svg";
function el(tag, attrs, parent){ const n = document.createElementNS(NS, tag); for (const k in attrs) n.setAttribute(k, attrs[k]); if (parent) parent.appendChild(n); return n; }
let clipSeq = 0;
// Grupo recortado na área de plotagem [ML, W-MR] x [top, bottom]: nenhuma linha sai do quadro.
function plotArea(svg, top, bottom){
  const id = "clip" + (++clipSeq);
  const defs = el("defs", {}, svg);
  const cp = el("clipPath", {id}, defs);
  el("rect", {x:ML, y:top - 2, width:W - ML - MR, height:bottom - top + 4}, cp);
  return el("g", {"clip-path":`url(#${id})`}, svg);
}
function txt(parent, attrs, s){ const t = el("text", attrs, parent); t.textContent = s; return t; }
const W = 1100, ML = 64, MR = 12;
const views = [];
const tip = document.getElementById("tip");

function domain(){
  let lo = Infinity, hi = -Infinity;
  DATA.runs.forEach(r => r.executions.forEach(e => { lo = Math.min(lo, e.start - base(r)); hi = Math.max(hi, e.end - base(r)); }));
  return [lo, hi];
}
function axisX(svg, sx, x0, x1, y){
  el("line", {x1:ML, x2:W - MR, y1:y, y2:y, stroke:"var(--axis)"}, svg);
  for (let i = 0; i <= 8; i++) {
    const t = x0 + i / 8 * (x1 - x0);
    txt(svg, {x:sx(t), y:y + 14, "text-anchor": i === 0 ? "start" : i === 8 ? "end" : "middle", class:"tick"}, mode === "zero" ? mmss(t) : hhmm(t));
  }
}
function renderTimeline(){
  const root = document.getElementById("timeline"); root.textContent = ""; views.length = 0;
  const [x0, x1] = domain();
  const sx = t => ML + (t - x0) / ((x1 - x0) || 1) * (W - ML - MR);
  // faixa de casos: uma linha por suíte
  const laneH = 26, H0 = DATA.runs.length * (laneH + 6) + 22;
  const body = makeCard(root, "t-cases", "Caso em execução");
  const svg = el("svg", {viewBox:`0 0 ${W} ${H0}`, role:"img", "aria-label":"Casos em execução ao longo do tempo"}, body);
  DATA.runs.forEach((r, ri) => {
    const y = 4 + ri * (laneH + 6), b = base(r);
    el("rect", {x:ML - 12, y:y + 4, width:4, height:laneH - 8, rx:2, fill:COLORS[ri]}, svg);
    txt(svg, {x:ML - 16, y:y + laneH / 2 + 4, "text-anchor":"end", class:"lane"}, r.suite);
    r.executions.forEach((e, ei) => {
      const a = sx(e.start - b), w = Math.max(1, sx(e.end - b) - a);
      el("rect", {x:a, y, width:Math.max(1, w - 2), height:laneH, rx:3, fill: ei % 2 ? "var(--band-b)" : "var(--band-a)",
                  stroke: e.failed ? "var(--text-primary)" : "none", "stroke-width": e.failed ? 1.5 : 0,
                  "stroke-dasharray": e.failed ? "4 3" : "none"}, svg);
      const label = (e.failed ? "✕ " : "") + short(e.case) + (e.failed ? " (falhou)" : "");
      if (w > label.length * 6.2 + 8) txt(svg, {x:a + 5, y:y + laneH / 2 + 4, class:"lane"}, label);
    });
  });
  axisX(svg, sx, x0, x1, H0 - 18);
  views.push({svg, sx, x0, x1, top:0, bottom:H0 - 18});
  // gráficos de hardware
  HW.forEach(c => {
    const body = makeCard(root, "t-" + c.field, c.title + " (" + c.unit + ")", c.note);
    const has = DATA.runs.some(r => r.executions.some(e => e.hw.length > 1));
    if (!has) { const d = document.createElement("div"); d.className = "empty"; d.textContent = "Sem dados de hardware nestas rodadas (monitor ausente ou execuções curtas demais)."; body.appendChild(d); return; }
    const H = 140, MT = 8, MB = 22;
    const avgs = DATA.runs.map(r => { let sum = 0, n = 0, mx = 0; r.executions.forEach(e => e.hw.forEach(x => { const v = x[c.field] || 0; sum += v; n++; mx = Math.max(mx, v); })); return {suite:r.suite, avg:n ? sum / n : null, max:mx}; });
    const inf = document.createElement("p"); inf.className = "infer";
    inf.textContent = "Média (pico): " + avgs.map(a => a.suite + " " + fmt(a.avg, c.unit) + " (" + fmt(a.max, c.unit) + ")").join(" · ") + ".";
    body.appendChild(inf);
    const svg = el("svg", {viewBox:`0 0 ${W} ${H}`, role:"img", "aria-label":c.title}, body);
    let hi = 0; DATA.runs.forEach(r => r.executions.forEach(e => e.hw.forEach(s => { hi = Math.max(hi, s[c.field] || 0); })));
    hi = Math.max(hi * 1.08, c.floor);
    const sy = v => MT + (1 - v / hi) * (H - MT - MB);
    [0, .25, .5, .75, 1].forEach(f => {
      const y = sy(f * hi);
      el("line", {x1:ML, x2:W - MR, y1:y, y2:y, stroke:"var(--grid)"}, svg);
      txt(svg, {x:ML - 8, y:y + 4, "text-anchor":"end", class:"tick"}, fmt(f * hi));
    });
    const g = plotArea(svg, MT, H - MB);
    DATA.runs.forEach((r, ri) => {
      const b = base(r);
      r.executions.forEach(e => {
        if (e.hw.length < 2) return;
        const d = e.hw.map((s, i) => (i ? "L" : "M") + sx(s.t - b).toFixed(1) + " " + sy(s[c.field] || 0).toFixed(1)).join(" ");
        el("path", {d, fill:"none", stroke:COLORS[ri], "stroke-width":2, "stroke-linejoin":"round", opacity:.95}, g);
      });
    });
    axisX(svg, sx, x0, x1, H - MB);
    views.push({svg, sx, x0, x1, top:MT, bottom:H - MB});
  });
  applyOrder(root);
  views.forEach(v => {
    v.cross = el("line", {x1:0, x2:0, y1:v.top, y2:v.bottom, stroke:"var(--text-muted)", "stroke-width":1, visibility:"hidden"}, v.svg);
    const hit = el("rect", {x:ML, y:0, width:W - ML - MR, height:v.bottom + 4, fill:"transparent"}, v.svg);
    hit.addEventListener("pointermove", ev => hover(ev, v));
    hit.addEventListener("pointerleave", () => { views.forEach(w => w.cross.setAttribute("visibility", "hidden")); tip.style.display = "none"; });
  });
}
function row(parent, value, name, color){
  const r = document.createElement("div"); r.className = "row";
  if (color) { const k = document.createElement("span"); k.className = "key"; k.style.background = color; r.appendChild(k); }
  const a = document.createElement("span"); a.className = "v"; a.textContent = value; r.appendChild(a);
  const b = document.createElement("span"); b.className = "n"; b.textContent = name; r.appendChild(b);
  parent.appendChild(r);
}
function hover(ev, v){
  const pt = v.svg.createSVGPoint(); pt.x = ev.clientX; pt.y = ev.clientY;
  const p = pt.matrixTransform(v.svg.getScreenCTM().inverse());
  const t = v.x0 + (p.x - ML) / (W - ML - MR) * (v.x1 - v.x0);
  views.forEach(w => { w.cross.setAttribute("x1", p.x); w.cross.setAttribute("x2", p.x); w.cross.setAttribute("visibility", "visible"); });
  tip.textContent = "";
  const top = document.createElement("div"); top.className = "n"; top.textContent = mode === "zero" ? "t = " + mmss(t) : hhmm(t) + " UTC"; tip.appendChild(top);
  let any = false;
  DATA.runs.forEach((r, ri) => {
    const at = t + base(r);
    const e = r.executions.find(e => at >= e.start && at <= e.end);
    if (!e) return;
    any = true;
    const hd = document.createElement("div"); hd.className = "hd"; hd.textContent = r.suite + ": " + short(e.case) + " (rep " + e.rep + ")" + (e.failed ? " — FALHOU: " + e.failed : ""); tip.appendChild(hd);
    const ph = e.phases.find(q => at >= q.start && at <= q.end);
    if (ph) row(tip, fmt(ph.engine, "ops/s"), "motor, fase " + ph.phase, COLORS[ri]);
    let s = null, best = Infinity;
    e.hw.forEach(x => { const d = Math.abs(x.t - at); if (d < best) { best = d; s = x; } });
    if (s && best < 1) {
      row(tip, fmt(s.proc_cpu_pct, "%"), "CPU do processo");
      row(tip, fmt(s.iowait_pct, "%"), "iowait");
      row(tip, fmt(s.disk_write_MBps, "MB/s"), "escrita em disco");
      row(tip, fmt(s.rss_MiB, "MiB"), "RSS");
    }
  });
  if (!any) { tip.style.display = "none"; return; }
  tip.style.display = "block";
  const rr = tip.getBoundingClientRect();
  tip.style.left = Math.min(window.innerWidth - rr.width - 12, ev.clientX + 14) + "px";
  tip.style.top = Math.min(window.innerHeight - rr.height - 12, ev.clientY + 14) + "px";
}
let caseSuites = "all", caseX = "sec";
function renderCompare(){
  const root = document.getElementById("compare"); root.textContent = "";
  const k = metric.key;
  const workloads = [...new Set(DATA.runs.flatMap(r => r.executions.map(e => e.case.split(".")[1])))];
  const shown = DATA.runs.map((r, ri) => ({r, ri})).filter(({r}) => caseSuites === "all" || r.suite === caseSuites);
  workloads.forEach(workload => {
    const series = shown.map(({r, ri}) => ({r, ri, execs: r.executions.filter(e => e.case.split(".")[1] === workload)}))
                        .filter(x => x.execs.length);
    const scales = DATA.runs.map(r => { const e = r.executions.find(e => e.case.split(".")[1] === workload); return r.suite + ": " + (e ? e.case.split(".").pop() : "—"); });
    const body = makeCard(root, "c-" + workload, workload + " (" + scales.join(" · ") + ")", null, (DATA.info || {})[workload]);
    if (!series.length) { const d = document.createElement("div"); d.className = "empty"; d.textContent = "Caso não medido na suíte escolhida."; body.appendChild(d); return; }
    // eixo x: segundos desde o início da execução, ou % dela
    const rel = (e, t) => caseX === "pct" ? (t - e.start) / ((e.end - e.start) || 1) * 100 : t - e.start;
    let xmax = 0; series.forEach(x => x.execs.forEach(e => { xmax = Math.max(xmax, rel(e, e.end)); }));
    const sx = v => ML + v / (xmax || 1) * (W - ML - MR);
    const tickX = v => caseX === "pct" ? v.toFixed(0) + "%" : xmax > 120 ? mmss(v) : v.toFixed(xmax < 2 ? 2 : xmax < 20 ? 1 : 0) + " s";
    // rótulos das fases: só na execução mais longa (onde há espaço), uma vez
    let labelExec = null; series.forEach(x => x.execs.forEach(e => { if (!labelExec || rel(e, e.end) > rel(labelExec, labelExec.end)) labelExec = e; }));
    const panels = [
      {title: metric.label + " (" + metric.unit + ")", h: 150, kind: "phase"},
      {title: "CPU do processo (%)", h: 80, field: "proc_cpu_pct", floor: 100},
      {title: "Memória do processo, RSS (MiB)", h: 80, field: "rss_MiB", floor: 1},
    ];
    const failed = series.flatMap(x => x.execs.filter(e => e.failed).map(e => x.r.suite + " rep " + e.rep + " (" + e.failed + ")"));
    if (failed.length) { const f = document.createElement("p"); f.className = "infer"; f.textContent = "✕ Execuções que falharam (sem fases; só o uso de hardware até o fim): " + failed.join(", ") + "."; body.appendChild(f); }
    const cardViews = [];
    panels.forEach((pn, pi) => {
      const MT = 14, MB = 20, H = pn.h;
      const cap = document.createElement("p"); cap.textContent = pn.title; cap.style.margin = pi ? "6px 0 0" : "2px 0 0"; body.appendChild(cap);
      const svg = el("svg", {viewBox:`0 0 ${W} ${H}`, role:"img", "aria-label": pn.title + " de " + workload}, body);
      let hi = 0;
      series.forEach(x => x.execs.forEach(e => {
        if (pn.kind === "phase") e.phases.forEach(p => { hi = Math.max(hi, p[k] || 0); });
        else e.hw.forEach(sm => { hi = Math.max(hi, sm[pn.field] || 0); });
      }));
      hi = Math.max(hi * 1.1, pn.floor || 0) || 1;
      const sy = v => MT + (1 - v / hi) * (H - MT - MB);
      [0, .5, 1].forEach(f => { const y = sy(f * hi); el("line", {x1:ML, x2:W - MR, y1:y, y2:y, stroke:"var(--grid)"}, svg); txt(svg, {x:ML - 8, y:y + 4, "text-anchor":"end", class:"tick"}, fmt(f * hi)); });
      el("line", {x1:ML, x2:W - MR, y1:H - MB, y2:H - MB, stroke:"var(--axis)"}, svg);
      for (let i = 0; i <= 8; i++) { const v = i / 8 * xmax; txt(svg, {x:sx(v), y:H - 5, "text-anchor": i === 0 ? "start" : i === 8 ? "end" : "middle", class:"tick"}, tickX(v)); }
      const g = plotArea(svg, MT, H - MB);
      // depois da última fase o processo ainda trabalha (conferência, fechamento)
      if (pn.kind === "phase") series.forEach(x => x.execs.slice(0, 1).forEach(e => {
        const a = sx(rel(e, e.phases_end || e.end)), b2 = sx(rel(e, e.end));
        if (b2 - a > 40) { el("rect", {x:a, y:MT, width:b2 - a, height:H - MT - MB, fill:"var(--band-b)"}, g);
          txt(g, {x:(a + b2) / 2, y:MT + 12, "text-anchor":"middle", class:"tick"}, "pós-fases (verificação)"); }
      }));
      series.forEach(x => {
        const color = COLORS[x.ri];
        x.execs.forEach((e, ei) => {
          const op = x.execs.length > 1 ? .55 : 1;
          if (pn.kind === "phase") {
            let d = "";
            e.phases.forEach((p, pi2) => {
              if (!(p[k] > 0)) return;
              const x1 = sx(rel(e, p.start)), x2 = sx(rel(e, p.end)), y = sy(p[k]);
              d += (d ? "L" : "M") + x1.toFixed(1) + " " + y.toFixed(1) + "L" + x2.toFixed(1) + " " + y.toFixed(1);
              // nome da fase sobre o degrau, uma vez por suíte
              if (e === labelExec && x2 - x1 > p.phase.length * 6 + 6) txt(g, {x:(x1 + x2) / 2, y:y - 4, "text-anchor":"middle", class:"tick"}, p.phase);
            });
            if (d) el("path", {d, fill:"none", stroke:color, "stroke-width":2.5, "stroke-linejoin":"round", opacity:op}, g);
          } else if (e.hw.length > 1) {
            // o monitor segue alguns instantes após o fim do caso: corta no fim da execução
            const d = e.hw.map((sm, i) => (i ? "L" : "M") + sx(rel(e, sm.t)).toFixed(1) + " " + sy(sm[pn.field] || 0).toFixed(1)).join(" ");
            el("path", {d, fill:"none", stroke:color, "stroke-width":1.8, opacity:op, "stroke-dasharray": e.failed ? "4 3" : "none"}, g);
          }
        });
      });
      const cross = el("line", {x1:0, x2:0, y1:MT, y2:H - MB, stroke:"var(--text-muted)", visibility:"hidden"}, svg);
      const hit = el("rect", {x:ML, y:0, width:W - ML - MR, height:H - MB + 2, fill:"transparent"}, svg);
      cardViews.push({svg, cross, hit});
    });
    // leitura: maior mudança entre as suítes, fase a fase (média das repetições)
    if (DATA.tables.length > 1) {
      const rowsA = DATA.tables[0].rows.filter(r => r.workload === workload), rowsB = DATA.tables[1].rows.filter(r => r.workload === workload);
      let worst = null;
      rowsA.forEach(a => { const b = rowsB.find(r => r.phase === a.phase); if (b && a[k] > 0 && b[k] > 0) { const d = (b[k] / a[k] - 1) * 100; if (!worst || Math.abs(d) > Math.abs(worst.d)) worst = {phase:a.phase, d, a:a[k], b:b[k]}; } });
      if (worst) {
        const bad = metric.better === "maior" ? -worst.d : worst.d;
        const inf = document.createElement("p"); inf.className = "infer";
        inf.textContent = Math.abs(worst.d) < 10
          ? "Estável: nenhuma fase muda mais de 10% em " + metric.label.toLowerCase() + " de " + DATA.tables[0].suite + " para " + DATA.tables[1].suite + "."
          : "Maior mudança de " + DATA.tables[0].suite + " para " + DATA.tables[1].suite + ": " + worst.phase + " " + fmt(worst.a, metric.unit) + " → " + fmt(worst.b, metric.unit) + " (" + (worst.d >= 0 ? "+" : "") + worst.d.toFixed(0) + "%, " + (bad > 0 ? "pior" : "melhor") + ").";
        body.insertBefore(inf, body.firstChild.nextSibling);
      }
    }
    // cursor e tooltip do cartão
    const at = (e, v) => caseX === "pct" ? e.start + v / 100 * (e.end - e.start) : e.start + v;
    cardViews.forEach(cv => {
      cv.hit.addEventListener("pointermove", ev => {
        const pt = cv.svg.createSVGPoint(); pt.x = ev.clientX; pt.y = ev.clientY;
        const px = pt.matrixTransform(cv.svg.getScreenCTM().inverse()).x;
        const v = (px - ML) / (W - ML - MR) * xmax;
        cardViews.forEach(o => { o.cross.setAttribute("x1", px); o.cross.setAttribute("x2", px); o.cross.setAttribute("visibility", "visible"); });
        tip.textContent = "";
        const top = document.createElement("div"); top.className = "n"; top.textContent = workload + " · " + tickX(v); tip.appendChild(top);
        series.forEach(x => {
          const e = x.execs[0]; const t = at(e, v);
          if (t > e.end) return;
          const hd = document.createElement("div"); hd.className = "hd"; hd.textContent = x.r.suite + " (" + e.case.split(".").pop() + ", rep " + e.rep + ")"; tip.appendChild(hd);
          const ph = e.phases.find(q => t >= q.start && t <= q.end);
          if (ph) row(tip, fmt(ph[k], metric.unit), ph.phase, COLORS[x.ri]);
          let sm = null, best = Infinity; e.hw.forEach(h => { const dd = Math.abs(h.t - t); if (dd < best) { best = dd; sm = h; } });
          if (sm && best < 1) { row(tip, fmt(sm.proc_cpu_pct, "%"), "CPU"); row(tip, fmt(sm.rss_MiB, "MiB"), "RSS"); }
        });
        tip.style.display = "block";
        const rr = tip.getBoundingClientRect();
        tip.style.left = Math.min(window.innerWidth - rr.width - 12, ev.clientX + 14) + "px";
        tip.style.top = Math.min(window.innerHeight - rr.height - 12, ev.clientY + 14) + "px";
      });
      cv.hit.addEventListener("pointerleave", () => { cardViews.forEach(o => o.cross.setAttribute("visibility", "hidden")); tip.style.display = "none"; });
    });
  });
  applyOrder(root);
}
const suiteSeg = document.getElementById("suite-seg");
[["all", "Ambas"], ...DATA.runs.map(r => [r.suite, r.suite])].forEach(([v, label]) => {
  const b = document.createElement("button"); b.textContent = label; b.setAttribute("aria-pressed", String(v === caseSuites));
  b.onclick = () => { caseSuites = v; [...suiteSeg.children].forEach(c => c.setAttribute("aria-pressed", String(c === b))); renderCompare(); };
  suiteSeg.appendChild(b);
});
document.getElementById("x-sec").onclick = () => { caseX = "sec"; document.getElementById("x-sec").setAttribute("aria-pressed", "true"); document.getElementById("x-pct").setAttribute("aria-pressed", "false"); renderCompare(); };
document.getElementById("x-pct").onclick = () => { caseX = "pct"; document.getElementById("x-pct").setAttribute("aria-pressed", "true"); document.getElementById("x-sec").setAttribute("aria-pressed", "false"); renderCompare(); };
const sel = document.getElementById("metric");
METRICS.forEach((m, i) => { const o = document.createElement("option"); o.value = i; o.textContent = m.label + " (" + m.unit + ", " + m.better + " é melhor)"; sel.appendChild(o); });
sel.onchange = () => { metric = METRICS[+sel.value]; renderCompare(); };
document.getElementById("reset").onclick = () => { layout = {order: {}, collapsed: {}}; saveLayout(); renderTimeline(); renderCompare(); };
function setMode(m){ mode = m; document.getElementById("m-real").setAttribute("aria-pressed", m === "real"); document.getElementById("m-zero").setAttribute("aria-pressed", m === "zero"); renderTimeline(); }
document.getElementById("m-real").onclick = () => setMode("real");
document.getElementById("m-zero").onclick = () => setMode("zero");

document.getElementById("sub").textContent = "Ambiente " + DATA.environment + " (" + DATA.label + "). " +
  "Commit " + [...new Set(DATA.runs.map(r => r.commit))].join(", ") + ".";
const tiles = document.getElementById("tiles"), lg = document.getElementById("legend");
DATA.runs.forEach((r, ri) => {
  const t = document.createElement("div"); t.className = "tile";
  const who = document.createElement("div"); who.className = "who";
  const sw = document.createElement("span"); sw.className = "swatch"; sw.style.background = COLORS[ri]; who.appendChild(sw);
  who.appendChild(document.createTextNode(r.suite + " × " + r.repeat + " · início " + r.started_at.slice(11, 16) + " UTC"));
  t.appendChild(who);
  const nums = document.createElement("div"); nums.className = "nums";
  const dur = r.executions.length ? r.executions[r.executions.length - 1].end - r.executions[0].start : 0;
  [[mmss(dur), "duração"], [r.executions.length + (r.executions.some(e => e.failed) ? " (" + r.executions.filter(e => e.failed).length + " falha)" : ""), "execuções"], [r.steal_max == null ? "—" : r.steal_max + "%", "steal máximo"]].forEach(([v, l]) => {
    const d = document.createElement("div"); const a = document.createElement("div"); a.className = "num"; a.textContent = v; d.appendChild(a);
    const b = document.createElement("div"); b.className = "lbl"; b.textContent = l; d.appendChild(b); nums.appendChild(d);
  });
  t.appendChild(nums); tiles.appendChild(t);
  const s = document.createElement("span"); const k = document.createElement("span"); k.className = "key"; k.style.background = COLORS[ri];
  s.appendChild(k); s.appendChild(document.createTextNode(r.suite)); lg.appendChild(s);
});
const thead = document.querySelector("#tbl thead"), tbody = document.querySelector("#tbl tbody");
const hr = document.createElement("tr");
["Suíte", "Caso · fase", "n", "Motor ops/s", "CV", "Total ops/s", "p50 µs", "p99 µs", "p99,9 µs", "WAL B/op",
 "Banco MiB", "B/obj", "RSS MiB", "Overhead", "Duração s", "CPU (caso)", "iowait (caso)", "Escrita MB/s (caso)", "Amplif. escrita", "Hash"]
  .forEach(x => { const th = document.createElement("th"); th.textContent = x; hr.appendChild(th); });
thead.appendChild(hr);
const opt = (v, u) => v == null ? "—" : fmt(v, u);
DATA.tables.forEach(t => t.rows.forEach(r => {
  const tr = document.createElement("tr");
  [t.suite, short(r.case) + " · " + r.phase, r.n, fmt(r.engine), r.cv.toFixed(1) + "%", fmt(r.ops), fmt(r.p50_us), fmt(r.p99_us),
   fmt(r.p999_us), fmt(r.wal_per_op), fmt(r.db_mib), fmt(r.bytes_per_object), fmt(r.rss_mib), fmt(r.overhead_pct, "%"),
   r.duration.toFixed(2), opt(r.cpu, "%"), opt(r.iowait, "%"), opt(r.wmb), opt(r.write_amplification), r.hash_ok ? "ok" : "DIVERGE"]
    .forEach(v => { const td = document.createElement("td"); td.textContent = v; tr.appendChild(td); });
  tbody.appendChild(tr);
}));
const ins = document.getElementById("insights");
(DATA.insights || []).forEach(x => { const li = document.createElement("li"); const t = document.createElement("span"); t.className = "topic"; t.textContent = x.topic + ":"; li.appendChild(t); li.appendChild(document.createTextNode(" " + x.text)); ins.appendChild(li); });
const nav = document.getElementById("nav");
function navLink(text, href, onClick){ const a = document.createElement("a"); a.href = href; a.textContent = text; if (onClick) a.addEventListener("click", onClick); nav.appendChild(a); }
function navGroup(text){ const s = document.createElement("span"); s.className = "grp"; s.textContent = text; nav.appendChild(s); }
navLink("Leituras", "#sec-insights");
navLink("Hardware", "#sec-hw");
navGroup("Casos:");
[...new Set(DATA.tables.flatMap(t => t.rows.map(r => r.workload)))].forEach(w =>
  navLink(w, "#card-c-" + w, () => { const c = document.getElementById("card-c-" + w); if (c && c.expand) c.expand(); }));
navLink("Tabela", "#sec-table");
renderTimeline(); renderCompare();
</script></body></html>
"""


def bottleneck(cpu: float | None, iowait: float | None, wmb: float | None) -> str | None:
    """Classifica o que limita um caso pelo uso de hardware medido durante ele."""
    if cpu is None or iowait is None:
        return None
    if cpu >= 85 and iowait < 3:
        return "CPU"
    if iowait >= 5 or (cpu < 60 and (wmb or 0) > 20):
        return "disco (fsync)"
    return "misto"


def fmt_rate(v: float) -> str:
    return f"{v / 1e6:.2f} M" if v >= 1e6 else f"{v / 1e3:.1f} mil" if v >= 1e4 else f"{v:.0f}"


def insights(runs: list[dict], tables: list[dict]) -> list[dict]:
    """Conclusões que os números sustentam, em frases curtas (renderizadas no topo)."""
    out: list[dict] = []
    # 1. Gargalo de cada caso, por suíte.
    for t in tables:
        by_case: dict[str, dict] = {}
        for r in t["rows"]:
            by_case.setdefault(r["workload"], r)
        groups: dict[str, list[str]] = {}
        for workload, r in by_case.items():
            kind = bottleneck(r["cpu"], r["iowait"], r["wmb"])
            if kind:
                groups.setdefault(kind, []).append(
                    f"{workload} {r['scale']} (CPU {r['cpu']:.0f}%, iowait {r['iowait']:.1f}%)")
        for kind in ("CPU", "disco (fsync)", "misto"):
            if kind in groups:
                meaning = {"CPU": "o limite é o código do motor — otimizar CPU rende aqui",
                           "disco (fsync)": "o limite é esperar o disco confirmar o commit — CPU mais rápida não ajuda",
                           "misto": "CPU e espera pelo disco se alternam"}[kind]
                out.append({"topic": "gargalo", "suite": t["suite"],
                            "text": f"{t['suite']}: limitados por {kind} — {meaning}: " + "; ".join(groups[kind]) + "."})
    # 2. O que muda de escala (primeira suíte contra a segunda).
    if len(tables) >= 2:
        a_rows = {(r["workload"], r["phase"]): r for r in tables[0]["rows"]}
        deltas = []
        for r in tables[1]["rows"]:
            a = a_rows.get((r["workload"], r["phase"]))
            if a and a["engine"] > 0 and r["engine"] > 0:
                deltas.append(((r["engine"] / a["engine"] - 1) * 100, r["workload"], r["phase"], a, r))
        if deltas:
            deltas.sort()
            worst = [d for d in deltas if d[0] <= -15][:5]
            stable = [d for d in deltas if abs(d[0]) < 10]
            s0, s1 = tables[0]["suite"], tables[1]["suite"]
            if worst:
                out.append({"topic": "escala", "text": f"De {s0} para {s1}, as fases que mais perdem velocidade: " + "; ".join(
                    f"{w} · {p} {d:+.0f}% ({fmt_rate(a['engine'])} → {fmt_rate(b['engine'])} ops/s, {a['scale']}→{b['scale']})"
                    for d, w, p, a, b in worst) + ". O custo por operação cresce com o tamanho do banco nessas fases."})
            if stable:
                out.append({"topic": "escala", "text": f"Estáveis entre {s0} e {s1} (±10%): " + ", ".join(
                    f"{w} · {p}" for _, w, p, _, _ in stable) + ". Essas operações não dependem do tamanho do banco."})
            gains = [d for d in deltas if d[0] >= 15]
            if gains:
                out.append({"topic": "escala", "text": f"Mais rápidas em {s1}: " + "; ".join(
                    f"{w} · {p} {d:+.0f}%" for d, w, p, _, _ in gains[-4:])
                    + ". Em geral é efeito de lote maior ou cache mais quente, não do motor — confira antes de concluir."})
        # memória
        def peak_rss(rows: list[dict]) -> dict[str, float]:
            peaks: dict[str, float] = {}
            for x in rows:
                peaks[x["workload"]] = max(peaks.get(x["workload"], 0), x["rss_mib"])
            return peaks
        a_rss, b_rss = peak_rss(tables[0]["rows"]), peak_rss(tables[1]["rows"])
        mem = [(b_rss[w] / a_rss[w], w, a_rss[w], b_rss[w]) for w in b_rss if a_rss.get(w)]
        if mem:
            mem.sort(reverse=True)
            ratio, w, ra, rb = mem[0]
            out.append({"topic": "memória", "text": f"Memória: o maior crescimento é {w}, {ra:.0f} → {rb:.0f} MiB de RSS "
                        f"({ratio:.1f}×). Com 8 GB de RAM, nenhum caso chega perto de pressionar a máquina."
                        if rb < 4096 else f"Memória: {w} chega a {rb:.0f} MiB de RSS ({ratio:.1f}×)."})
    # 2b. Execuções que falharam, com a causa que o uso de hardware aponta.
    for r in runs:
        for e in r["executions"]:
            if not e.get("failed"):
                continue
            workload, scale = split_case(e["case"])
            rss = max((s.get("rss_MiB", 0) for s in e["hw"]), default=0)
            avail = min((s.get("mem_avail_MiB", 1e9) for s in e["hw"]), default=None)
            secs = e["end"] - e["start"]
            oom = e["failed"] == "exit_137" and avail is not None and avail < 256
            cause = (f"morta por falta de memória (OOM): o RSS chegou a {rss / 1024:.1f} GiB e a memória livre a "
                     f"{avail:.0f} MiB" if oom else f"falhou com {e['failed']} (RSS máximo {rss:.0f} MiB)")
            out.append({"topic": "falha", "text": f"{r['suite']}: {workload} {scale} (rep {e['rep']}) {cause}, após "
                        f"{secs:.0f} s. Sem números de desempenho para esse caso nesta rodada."})
    # 3. Confiabilidade.
    for t in tables:
        noisy = [f"{r['workload']} · {r['phase']} ({r['cv']:.0f}%)" for r in t["rows"] if r["n"] > 1 and r["cv"] > 5]
        if noisy:
            out.append({"topic": "ruído", "text": f"{t['suite']}: variação entre repetições acima de 5% em "
                        + ", ".join(noisy[:6]) + ". Diferenças menores que isso nessas fases são ruído."})
        elif t["rows"] and all(r["n"] > 1 for r in t["rows"]):
            out.append({"topic": "ruído", "text": f"{t['suite']}: todas as fases com variação entre repetições ≤ 5% — "
                        "resultados estáveis."})
        if any(r["n"] == 1 for r in t["rows"]):
            out.append({"topic": "ruído", "text": f"{t['suite']}: uma repetição só — os números indicam a ordem de "
                        "grandeza, mas não dá para separar sinal de ruído."})
    for r in runs:
        st = r.get("steal_max")
        if st is not None:
            out.append({"topic": "máquina", "text": f"{r['suite']}: steal máximo {st}% — "
                        + ("CPU dedicada de fato; nenhum vizinho interferiu." if float(st) < 2
                           else "houve interferência do hipervisor; trate a rodada com cautela.")})
    bad = [f"{t['suite']}: {r['workload']}" for t in tables for r in t["rows"] if not r["hash_ok"]]
    if bad:
        out.append({"topic": "correção", "text": "Estado final incorreto em " + ", ".join(sorted(set(bad)))
                    + " — resultado inválido, investigar antes de usar os números."})
    else:
        out.append({"topic": "correção", "text": "Estado final conferido em todas as execuções (hash dos objetos, ou "
                    "nenhum objeto restante nos casos que apagam tudo): os números vêm de execuções corretas."})
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-e", "--environment", required=True)
    parser.add_argument("runs", nargs="*", help="nomes das rodadas (em load-results/remote/<ambiente>/)")
    parser.add_argument("--last", type=int, help="as N rodadas mais recentes do ambiente")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    base = FETCHED / args.environment
    names = list(args.runs)
    if args.last:
        names += [p.parent.name for p in sorted(base.glob("*/manifest.json"))][-args.last:]
    if not names:
        sys.exit("informe rodadas ou --last N")
    runs = [load_run(base / n) for n in names]
    catalog = json.loads((ROOT / "loadtests" / "environments.json").read_text(encoding="utf-8"))
    label = next((e.get("label", "") for e in catalog["environments"] if e["id"] == args.environment), "")
    tables = [{"suite": r["suite"], "rows": phase_table(r)} for r in runs]
    data = {"environment": args.environment, "label": label, "runs": runs, "tables": tables,
            "insights": insights(runs, tables),
            "info": {k: {"what": v[0], "phases": v[1], "stresses": v[2], "read": v[3]} for k, v in WORKLOAD_INFO.items()}}
    out = args.out or base / f"report-{'-vs-'.join(r['name'][:16] for r in runs)}.html"
    payload = json.dumps(data, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")
    out.write_text(TEMPLATE.replace("__DATA__", payload), encoding="utf-8")
    print(f"perfpack: relatório em {out}")
    for r in runs:
        samples = sum(len(e["hw"]) for e in r["executions"])
        print(f"  {r['name']}: {len(r['executions'])} execuções, {samples} amostras de hardware")


if __name__ == "__main__":
    main()
