# Fechamento da Fase 10 — matriz final (10F)

Tag: `0.0.10f`  
Commit de entrega: `8d6a7a5` (merge em `master`).

## Matriz de build/teste

| Preset | Resultado |
|---|---|
| `debug` + `ctest --preset debug` | suíte completa verde |
| `sanitizers` + `ctest --preset sanitizers` | suíte completa verde (MinGW: hardening) |
| Corpus fuzz (`modb.fuzz.*`) | verde (replay; libFuzzer documentado na 10D) |
| Consumidor instalado (`modb.consumer`) | verde (10E) |

## Demo operacional (backup/restore/`db check`)

Validado conforme [OPERACAO.md](OPERACAO.md):

1. `modb oo employee demo` + `index`
2. `modb db check` verde (reconhece DBRT/IDMD/IDMP/BLBP/IXDR/BTLF/BTIN)
3. Cópia quiescente de `<db>` (+ WAL se houver)
4. Remoção do original, restauração, `db check` verde de novo

## Benchmark vs baseline 10A

Método: mesmo seed/perfil do runner 10A (`modb_bench`, smoke, seed 1).
Números absolutos são da máquina de desenvolvimento (Debug); o valor está na
comparabilidade de `parameters_key` e na série histórica.

Campanha 10F (Debug, smoke, seed 1), mediana ops/s aproximada:

| Cenário | ops/s (mediana) |
|---|---:|
| `object_store.lifecycle` count=200 stride=1 | ~59k |
| `object_store.lifecycle` count=200 stride=7 | ~67k |
| `storage.buffer_pool.oversubscribed` | ~35k |
| `object_store.read_hotpath` count=100 | ~111k |

Relação com a Fase 10:

- **10A** — runner + JSONL + cenário lifecycle
- **10B** — BufferPool + cenário oversubscribed
- **10C** — otimizações medidas no `read_hotpath` ([OTIMIZACOES_10C.md](OTIMIZACOES_10C.md))
- **10D** — fuzz/corpus
- **10E** — compatibilidade + API instalável
- **10F** — docs/operação + esta matriz

Gates oficiais futuros devem usar Release/`standard` em máquina ociosa;
Debug/smoke serve para fechar a documentação e detectar regressões grosseiras
do runner.

Como reproduzir:

```powershell
cmake --build --preset debug
.\build\debug\modb_bench.exe run --profile smoke --seed 1 --output-dir benchmark-results
.\build\debug\modb_bench.exe compare <antes.jsonl> <depois.jsonl>
```

Detalhes do formato JSONL: [BASELINE_DESEMPENHO.md](BASELINE_DESEMPENHO.md).

## Documentos entregues nesta tag

- [README.md](../README.md) — OO first
- [FORMATO_DE_ARQUIVO.md](FORMATO_DE_ARQUIVO.md) — DBRT…WAL
- [OPERACAO.md](OPERACAO.md) — backup/restore/supervisor/`db check`
- `db check` reconhece páginas IXDR/BTLF/BTIN
