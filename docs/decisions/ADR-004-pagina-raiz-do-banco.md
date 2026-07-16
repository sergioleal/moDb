# ADR-004 — Página raiz do banco (`DBRT`)

- Estado: aceito para o MVP OO
- Data: 2026-07-16

## Contexto

O superbloco (página zero, ADR legada [0001](0001-formato-de-armazenamento.md))
já reserva um campo `catalog_root`. O modelo OO precisa de várias raízes
persistentes — mapa de identidade, heap de catálogo, heap de dados, contador de
ObjectId, baseline corrente. Colocá-las direto no superbloco engessaria a
página zero; melhor concentrá-las numa página dedicada apontada pelo
`catalog_root`.

## Decisão

O campo `catalog_root` do superbloco passa a apontar para uma página `DBRT`
(criada na primeira escrita OO). Layout:

| Offset | Campo | Tipo |
|---|---|---|
| 0 | magic `"DBRT"` | 4 bytes |
| 4 | versão | u16 (=1) |
| 6 | flags | u16 (=0) |
| 8 | `identity_dir` (PageId da IDMD raiz) | u64 |
| 16 | `catalog_heap_root` (THRP do heap de catálogo) | u64 |
| 24 | `data_heap_root` (THRP do heap de dados) | u64 |
| 32 | `next_object_id` | u64 |
| 40 | `current_baseline` (ObjectId) | u64 |
| 48.. | reservado (zeros) | — |

- `0` em qualquer campo de página/id significa "ainda não existe".
- No MVP há **um** heap de dados para todos os objetos; segmentação de heaps
  por tipo é pós-MVP.
- `next_object_id` é gravado **antes** do registro do objeto correspondente; um
  crash entre as duas escritas desperdiça um id, nunca duplica
  (ver [ADR-001](ADR-001-identidade.md)).
- A época MVCC (Fase 6) reutilizará um dos campos reservados; a versão da página
  `DBRT` sobe quando isso ocorrer.

## Consequências

O superbloco permanece estável (magic, versão, tamanho de página, um ponteiro).
Toda a topologia OO fica numa página só, fácil de ler, validar e evoluir por
versão. A leitura de `DBRT` valida magic e versão antes de confiar nos
ponteiros, tratando arquivos corrompidos ou de formato relacional antigo com
erro claro em vez de interpretação silenciosa.
