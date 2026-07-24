# Operação — I/O assíncrono do WAL (Fase 13)

Guia operacional de `DatabaseOptions::wal_io`. Decisão: [ADR-019](decisions/ADR-019-io-assincrono.md).

## O que é

`storage::AsyncFile` é um backend de I/O posicional assíncrono opcional
(IOCP no Windows, POSIX AIO no Linux) com barreiras explícitas
(`submit_*` + `drain`/`barrier`). Desde a Fase 13.4, o WAL pode gravar sobre
esse backend em vez do `NativeFile` síncrono: `AsyncWalSink` (`src/tx/wal.cpp`)
só enfileira cada `write_at` (`submit_write_at`) e drena tudo — after-images
acumulados + o marcador de sync — num único `sync()`.

## Como habilitar

```cpp
DatabaseOptions opts;
opts.wal_io = WalIoMode::async; // default: WalIoMode::sync
auto db = Database::create(path, opts);
```

Não muda o formato do WAL nem a ordem WAL-antes-de-páginas: `PageFile`
continua em `NativeFile` e só aplica páginas depois que `wal->sync()` retorna
(`Database::commit_transaction`, `src/object/database.cpp`). `Database::wal_io()`
reporta o modo em uso. Não há flag na CLI para isso ainda — é uma opção de
`DatabaseOptions` para quem constrói a aplicação sobre a biblioteca.

## Resultado do benchmark (13.7) — sem ganho consistente ainda

Cenário `storage.async_io.{sync,async}` (`benchmarks/scenarios/storage_async_io.cpp`):
N transações, cada uma commitando M objetos (M page-images antes do sync).
Medido em 2026-07-24, build Debug, sem otimizações:

| Plataforma | Transações × objetos | sync (ops/s) | async (ops/s) | Diferença |
|---|---|---:|---:|---:|
| Windows (IOCP) | 100 × 4 | 1283.6 | 1076.2 | -16% |
| Windows (IOCP) | 100 × 32 | 7728.2 | 5355.4 | -31% |
| Windows (IOCP) | 1000 × 32 | 4664.6 | 4933.6 | +6% |
| Linux (POSIX AIO, WSL) | 100 × 32 | 1697.9 | 1641.7 | -3% |
| Linux (POSIX AIO, WSL) | 1000 × 32 | 1632.5 | 1724.9 | +6% |

**Conclusão honesta:** a diferença fica dentro do ruído (±3-30%) e trocou de
sinal entre execuções, nos dois backends. Não há evidência de ganho
consistente no tamanho de transação testado. Suspeita (não confirmada por
profiling): cada `submit_write_at`/`submit_read_at` copia o buffer inteiro
para a fila interna (`op.write_bytes.assign(...)` em
`src/storage/async_file_windows.cpp` e `src/storage/async_file_linux.cpp`),
e o backend Windows aguarda a conclusão de cada operação individualmente
dentro do grupo (`GetQueuedCompletionStatus` por op) — nenhum dos dois
paraleliza de fato o I/O de uma única transação neste tamanho de lote.

**Recomendação:** manter `WalIoMode::sync` (default) em produção. Tratar
`async` como caminho experimental, reavaliar com profiling real (não só
medição ponta a ponta) antes de promover a hot paths, e refazer o benchmark
se a implementação de `AsyncFile` for otimizada (ex.: eliminar a cópia por
operação, paralelizar entre transações concorrentes em vez de só dentro de
uma).

## Limites atuais

- `max_inflight` padrão 64 operações entre drains; exceder retorna
  `invalid_argument` (sem fila ilimitada).
- Sem fallback automático: Windows e Linux sempre usam backend nativo
  (`sync_fallback` reservado para uma plataforma futura sem suporte).
- Backend POSIX AIO validado em WSL Ubuntu 24.04 (g++ 13.3, toolchain
  CLion) — sem pipeline de CI própria; revalidar manualmente a cada mudança
  em `src/storage/async_file_linux.cpp`.
