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

## Catch-up por download do WAL (Fase 16)

```text
modb replicate catch-up follower.modb primary.modb.wal primary.modb [from_lsn]
```

O comando monta um manifesto do WAL disponível, baixa o segmento para um spool
local (`.modb-catchup-spool` ao lado da réplica), valida tamanho/hash e aplica
os registros em ordem até `up_to_date`.

Se `from_lsn` for omitido, a réplica descobre a posição por metadata de
catch-up, `follower_ack_lsn` e `checkpoint_lsn`. Use `from_lsn` apenas para
forçar uma retomada operacional específica; internamente ele significa aplicar
a partir desse LSN, com base local `from_lsn - 1`.

Estados persistem em `<follower.modb>.catchup`:

- `empty`: nenhuma tentativa confirmada;
- `catching_up`: download/apply em andamento ou retomável;
- `up_to_date`: aplicado até `target_lsn`;
- `requires_bootstrap`: o WAL retido não cobre o buraco local;
- `failed`: falha terminal com mensagem em `last_error`.

`replicate status` mostra `catchup_state`, `catchup_applied_lsn`,
`catchup_target_lsn` e, quando houver, `catchup_last_error`.

## Primary `wal_only` (Fase 15)

Com `primary_storage=wal_only` o primary **não** mantém arquivo de páginas.
Persiste apenas o controle `MCTL` no path lógico e o WAL (`<path>.wal`).
Decisão: [ADR-017](decisions/ADR-017-primary-wal-only-sem-arquivos-de-dados.md).

### Seed sem snapshot do primary

```text
modb replicate seed-wal follower.modb primary.modb.wal primary.modb
```

Cria a réplica com arquivo de dados vazio (identidade alinhada) e aplica o WAL
desde o LSN 1. O primary `wal_only` **não** pode doar bootstrap por cópia de
arquivo (`replicate bootstrap` falha com `data_files_disabled`); use seed-wal
ou doação entre réplicas de dados.

### Status

`modb replicate status` mostra `primary_storage`, uuid/timeline e LSNs (incluindo
`follower_ack_lsn` usado na retenção e na política de ACK do commit).
