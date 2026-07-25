# ADR-020 — Catch-up de réplica por WAL baixável

## Contexto

A Fase 14 mantém uma réplica de leitura atualizada por streaming do WAL, e a
Fase 15 permite primary `wal_only`, com dados materializados nas réplicas. Ainda
faltava um caminho operacional para uma réplica nova, vazia ou incompleta baixar
um lote fechado de WAL, aplicar localmente e alcançar o estado `up_to_date` sem
depender de uma sessão de streaming contínua desde o começo.

## Decisão

O Ring0 passa a oferecer catch-up por WAL baixável:

- uma fonte autorizada publica um manifesto lógico do WAL disponível;
- cada manifesto declara UUID do banco, timeline, `oldest_available_lsn`,
  intervalo de LSN e hash/tamanho do segmento;
- a réplica mantém estado persistente em `<replica>.catchup`;
- o download usa spool local, arquivo temporário, validação de hash/tamanho,
  `sync` e rename atômico;
- o apply batch reutiliza o applier idempotente da Fase 14;
- a réplica só declara `up_to_date` depois de aplicar até `target_lsn` e
  persistir a posição.

No formato atual o WAL físico ainda é um arquivo único (`<db>.wal`), então o
manifesto contém um segmento lógico. A API já usa vetor de segmentos para
preservar o contrato quando a segmentação física evoluir.

## Consequências

- Réplicas vazias podem ser criadas e preenchidas a partir de WAL retido.
- Réplicas parciais descobrem a posição local por metadata/ACK/checkpoint e
  baixam apenas o intervalo ainda necessário.
- Gap abaixo da retenção vira `bootstrap_required`; o applier nunca pula LSN.
- Hash divergente ou tamanho incorreto aborta antes do apply.
- O sidecar de catch-up é legível e removível em operação, sem alterar o DBRT.
- A semântica do streaming não muda: depois de `up_to_date`, a réplica pode
  assinar `last_lsn + 1`.
