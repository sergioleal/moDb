# Formato de arquivo do Ring0

O magic de arquivo (`MODB`), o namespace, a CLI e alguns nomes de pacote ainda
usam o identificador técnico `modb` / `moDb`.

Estado: consolidado na **Fase 10F**. Compatibilidade major/minor:
[COMPATIBILIDADE.md](COMPATIBILIDADE.md) · [ADR-015](decisions/ADR-015-compatibilidade.md).

## Regras gerais

- tamanho de página fixo na compilação (`MODB_PAGE_SIZE`): 8192 no build
  CMake padrão, 16384 no SDK binário, 4096 nos bancos antigos; vai gravado no
  superbloco, e um arquivo de outro tamanho é recusado na abertura
  (`page size mismatch`);
- um registro do heap (o objeto codificado) pode ter qualquer tamanho
  ([ADR-030](decisions/ADR-030-registros-de-qualquer-tamanho.md)): até
  `page_size − 37` bytes ele fica inline na slotted page (16347 no SDK); acima
  disso, vai para uma cadeia de páginas `OVFP`, e a slotted page guarda só um
  stub de 17 bytes. Heaps de antes da ADR-030 mantêm o limite de
  `page_size − 36` bytes e respondem `record_too_large`;
- inteiros little-endian; estruturas C++ nunca são memcpy’d para o disco;
- `PageId` = índice da página; offset = `PageId * page_size`;
- o banco OO vive num único arquivo `<db>`; o WAL é `<db>.wal` (arquivo separado).

## Superbloco — magic `MODB` (página 0)

| Offset | Campo | Notas |
|---:|---|---|
| 0 | magic `MODB` | 4 bytes |
| 4 | versão `u16` | legado: major quando minor=0; ver ADR-015 |
| 8 | page size `u32` | deve bater com o build |
| 12 | page count `u64` | inclui o superbloco |
| 20 | catalog/root hint `u64` | histórico; OO usa DBRT via ObjectStore |

Abertura rejeita major incompatível ou minor futura não suportada.

## Páginas do modelo OO

| Magic | Nome | Papel |
|---|---|---|
| `DBRT` | Database root | ponteiros para heaps, identidade, catálogo, época (v2) |
| `IDMD` | Identity directory | cadeia de diretórios do mapa de identidade |
| `IDMP` | Identity entries | entradas ObjectId → localização (+ previous em v2) |
| `BLBP` | Blob page | cadeia de blobs (header 24 B + payload) |
| `IXDR` | Index directory | catálogo de índices (tipo/campo → raiz B-tree) |
| `BTLF` | B-tree leaf | folhas do índice |
| `BTIN` | B-tree internal | nós internos |
| `SLPG` | Slotted page | registros de tamanho variável (heaps) |
| `THRP` | TableHeap root | raiz de um heap de registros; desde 2026-09 guarda também `free_head`, a lista de páginas livres do heap (campo no fim, zero = vazia; [ADR-023](decisions/ADR-023-lista-de-paginas-livres-do-heap.md)). O bit 0 das flags (offset 6) marca os registros com byte de tipo e overflow ([ADR-030](decisions/ADR-030-registros-de-qualquer-tamanho.md)) |
| `OVFP` | Overflow page | pedaço de um registro maior que uma página: `"OVFP" \| versão u16 (1) \| reservado u16 \| next u64 \| tamanho u32 \| reservado u32 \| dados` (cabeçalho de 24 B); a cadeia de cada registro é conferida pelo `db check` ([ADR-030](decisions/ADR-030-registros-de-qualquer-tamanho.md)) |

`modb db check` classifica essas assinaturas e valida cabeçalhos básicos
(versão/comprimento). Cadeias semânticas profundas (ciclos de blob, consistência
de índices) podem exigir níveis futuros.

### DBRT (resumo)

Versão corrente **2**. Campos incluem raízes de heaps de objetos/catálogo,
mapa de identidade, próximo ObjectId e época MVCC. v1 é lida e regravada como v2
na abertura (migração explícita, ADR-009).

Campos acrescentados no fim da v2, sem mudar a versão — ausentes (zero) em
arquivos anteriores, e zero significa "desconhecido":

| campo | desde | significado |
|---|---|---|
| uuid, timeline, `next_lsn`, `checkpoint_lsn`, `follower_ack_lsn` | Fase 14 | identidade e posições do WAL |
| `checkpoint_wal_offset` `u64` | 2026-09 (T26) | offset do WAL logo depois do último commit coberto pelo checkpoint; a recuperação lê dali em diante quando o valor é confiável, e o WAL inteiro quando não é |

### IDMD / IDMP

- IDMD v1: diretório de páginas de entradas.
- IDMP v2: localização atual + época + previous (MVCC); v1 migra para v2.

### BLBP

```text
| "BLBP" 4 | version u16 | reserved u16 | next u64 | length u32 | reserved u32 | payload |
```

`length ≤ page_size - 24`. Cadeia encadeada por `next`; ciclo é erro em leitura.

### Índices (IXDR / BTLF / BTIN)

Versão **1**. IXDR lista `(type_name, field_id, root_page)`. Nós B-tree usam
header de 32 bytes (magic, version, level, key_count, cell_top, link, …).

## Payload de objeto (heap)

Registro no TableHeap:

```text
| object_id u64 | type_definition_id u64 | payload |
```

Num heap com a flag de registros marcados (ADR-030), o que vai na slotted page
leva um byte de tipo na frente:

```text
0x00 | registro                                  inline
0x01 | tamanho_total u64 | primeira_página u64    stub; os bytes estão na cadeia OVFP
```

Quem lê pelo `TableHeap` recebe sempre o registro inteiro, sem o byte de tipo.

Payload (versão 1):

```text
| version u8 | field_count u16 | (field_id u16, tag u8, value)* |
```

Tags alinhadas ao codec OO (`null`, bool, int64, float64, string, bytes, ref,
blob, embedded, …). Limite de campos: `max_columns_per_table` (ADR-007).

## WAL — arquivo `<db>.wal`

Magic `MOWL`, versão 1. Registros tipados (begin / page image / commit) com
CRC. Recovery na abertura reaplica commits duráveis; trabalho sem commit some.
Ver [OPERACAO.md](OPERACAO.md) para backup conjunto db+wal.

## Legado relacional (ainda no binário)

O codec `Value`/`Row`/`Schema` e páginas SLPG/THRP também servem caminhos
relacionais antigos e testes. Novos consumidores devem usar a API OO
([API_PUBLICA.md](API_PUBLICA.md)). Detalhes históricos do encoding relacional
permanecem nos testes e no código sob `src/storage/codec.cpp` /
`src/model/`.

## Evolução

- **Major** do superbloco/protocolo: incompatível — erro específico.
- **Minor** aditivo: leitor com minor ≥ artefato lê; ver ADR-015.
- Mudanças destrutivas de página (ex.: IDMP v1→v2) são migrações explícitas
  no código do artefato, não “minor silencioso”.
