# ADR-005 — Mapa de identidade (ObjectId → localização física)

- Estado: aceito para o MVP OO
- Data: 2026-07-16

## Contexto

A identidade é permanente mas o endereço físico muda
([ADR-001](ADR-001-identidade.md)). É preciso uma tradução `ObjectId →
(página, slot)` persistente, rápida (caminho quente de todo `get`) e barata de
atualizar quando um registro migra de página. Como os ObjectId são monotônicos
e nunca reutilizados, o mapa pode ser endereçado diretamente pelo id, sem
árvore de busca.

## Decisão

Duas camadas de páginas.

**IDMD (diretório)** — header 16 bytes:

| Offset | Campo | Tipo |
|---|---|---|
| 0 | magic `"IDMD"` | 4 bytes |
| 4 | versão | u16 |
| 6 | reservado | u16 |
| 8 | `next_dir` (PageId do próximo IDMD) | u64 |

Após o header, até `(page_size-16)/8` PageIds de páginas IDMP. Diretórios
encadeiam por `next_dir` quando lotam.

**IDMP (entradas)** — header 16 bytes (`"IDMP"` | versão u16 | reservado u16 |
reservado u64) seguido de entradas de 16 bytes:

| Offset | Campo | Tipo |
|---|---|---|
| 0 | `page` (PageId do registro) | u64 |
| 8 | `slot` | u16 |
| 10 | `generation` | u16 |
| 12 | flags (bit0 = alocado, bit1 = removido) | u32 |

**Endereçamento** (O(1), ≤ 2 leituras de página):

```text
entradas_por_pagina = (page_size - 16) / 16
indice_global       = object_id
pagina_idmp         = indice_global / entradas_por_pagina   (resolvida pelo diretório)
entrada             = indice_global % entradas_por_pagina
```

- `generation` acompanha a geração do slot no `TableHeap`, detectando
  referências obsoletas.
- `rebind` (objeto mudou de página) atualiza só a entrada, sem tocar em quem
  referencia o objeto.
- `erase` marca o tombstone (bit1); o id não é reaproveitado.
- Versão 2 da IDMP (Fase 6, MVCC) amplia a entrada para 48 bytes com uma versão
  anterior; a migração regrava o mapa na primeira abertura pós-upgrade.

## Consequências

Lookup de identidade em tempo constante com no máximo duas leituras (diretório
+ entradas), sem estrutura de busca. O preço é um mapa que cresce com o maior
ObjectId já alocado (denso porque não há reuso); páginas IDMP são alocadas sob
demanda conforme os ids avançam. Referências pendentes (objeto removido) são
detectáveis pelo tombstone e pela geração.
