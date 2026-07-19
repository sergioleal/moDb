# Baseline de desempenho (Fase 10A)

Estado: baseline pré-otimização, coletada pelo runner `modb_bench`.
Os números absolutos variam por máquina; o que importa é a série histórica
comparável (mesmo seed, mesmos parâmetros e mesma classe de build).

## Como coletar

```powershell
cmake --build --preset debug
.\build\debug\modb_bench.exe run --profile smoke --seed 1 --output-dir benchmark-results
.\build\debug\modb_bench.exe run --profile smoke --seed 1 --output-dir benchmark-results
.\build\debug\modb_bench.exe compare <primeiro.jsonl> <segundo.jsonl>
```

Perfil `standard` (campanha oficial mais longa):

```powershell
.\build\debug\modb_bench.exe run --profile standard --seed 1 --output-dir benchmark-results
```

Scripts: `scripts/run-benchmarks.ps1` e `scripts/run-benchmarks.sh`.

## O que o JSONL contém

Cada campanha gera um único arquivo:

`modb-benchmark-YYYYMMDDTHHMMSS.mmmZ-<commit>-<host>.jsonl`

Registros: `run_start`, `environment`, `scenario_start`, `sample`,
`scenario_summary`, `scenario_error` (se houver), `run_note`, `run_end` com
`content_sha256` do conteúdo anterior.

## Cenário inicial migrado

`object_store.lifecycle` — create + flush + delete + flush do ObjectStore,
com verificação de correção fora da região medida. Variantes sequencial
(`stride=1`) e em passada (`stride>1`).

## Limiares do comparador

- alerta: regressão de throughput ≥ 5%
- falha de gate: ≥ 10%
- correção inválida: falha imediata da campanha

Campanhas `smoke`/`diagnostic` em build Debug servem para validar o runner;
gates oficiais usam Release/RelWithDebInfo e perfil `standard`, com máquina
ociosa. Variação alta entre duas smokes Debug na mesma máquina não invalida a
comparabilidade semântica (mesmo `parameters_key`).

Detalhes: [PLANO_BENCHMARKS.md](PLANO_BENCHMARKS.md).
