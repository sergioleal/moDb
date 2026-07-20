# Operação — Réplica de leitura (Fase 14)

Guia operacional do follower read-only alimentado pelo WAL do primary.
Decisão: [ADR-016](decisions/ADR-016-replica-de-leitura-por-streaming-do-wal.md).

## Papéis

| Papel | Arquivos | Escrita |
|---|---|---|
| Primary | `<db>.modb` + `<db>.wal` (durável) | única |
| Follower | cópia própria de `<db>.modb` | só o applier |

Nunca compartilhe o mesmo volume de dados entre primary e follower.

## Bootstrap

```text
modb replicate bootstrap primary.modb follower.modb
```

O primary toma uma barreira do escritor, copia o arquivo e grava o follower.
O `cut_lsn` impresso é o ponto a partir do qual o follower deve assinar o WAL
(`cut_lsn + 1`).

## Apply incremental

```text
modb replicate apply-wal follower.modb primary.modb.wal <from_lsn>
```

Aplica registros com LSN ≥ `from_lsn`. Gap abaixo da retenção /
descontinuidade → erro `replication_gap` (novo bootstrap).

## Status

```text
modb replicate status primary.modb
```

Mostra `uuid`, `timeline`, `next_lsn`, `checkpoint_lsn`, `follower_ack_lsn` e
`oldest_available_lsn`.

## Reconexão

1. Ler `checkpoint_lsn` / `applied_lsn` do follower.
2. Pedir frames a partir de `applied_lsn + 1`.
3. Se o primary responder gap, executar novo bootstrap.

## Primary `wal_only` (Fase 15)

O modo em que o primary não mantém arquivos de dados está na Fase 15
([ADR-017](decisions/ADR-017-primary-wal-only-sem-arquivos-de-dados.md)).
