# ADR-007 — Limites do MVP OO

- Estado: aceito para o MVP OO
- Data: 2026-07-16
- Sucede: os limites da ADR legada e do `ESCOPO_MVP.md` relacional.

## Contexto

Limites precisam ser centralizados e validados antes de qualquer escrita, para
que arquivos e entradas fora do previsto sejam rejeitados com erro claro em vez
de corromper o banco. O MVP OO (Fases 0–3) prova identidade, persistência,
codec e evolução de schema; limites maiores entram depois, com medição.

## Decisão

| Limite | Valor |
|---|---|
| atributos por tipo | 256 |
| identificadores (nomes de tipo e atributo) | ≤ 63 bytes UTF-8 |
| payload de um objeto | deve caber na área útil de uma página |
| string/bytes inline | ≤ capacidade do payload; acima disso, usar blob (Fase 4) |
| tamanho de página | 4096 bytes (herdado; configurável só em builds de profiling) |
| conversões implícitas com perda | proibidas |

- Os limites vivem centralizados em `limits.hpp` (estendendo o arquivo atual) e
  são checados antes de escrever.
- Objetos maiores que uma página não são fatiados no MVP; dados grandes vão
  para a `BlobStore` (Fase 4), e o objeto guarda apenas o `BlobId`.

## Consequências

O MVP mantém o objeto sempre em uma página, simplificando ObjectStore e mapa de
identidade. Coleções e binários grandes só passam a caber quando a BlobStore
existir (Fase 4), o que é coerente com a ordem das fases. Elevar qualquer limite
é uma decisão consciente, com nova validação, nunca um efeito colateral.
