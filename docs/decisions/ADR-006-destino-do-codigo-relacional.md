# ADR-006 — Destino do código relacional

- Estado: aceito para o MVP OO
- Data: 2026-07-16

## Contexto

O código atual contém um modelo relacional (`Catalog`, `Table`, `Schema`,
`Row`, `Value`/`DataType`) e comandos relacionais na CLI, construídos sob o
plano legado. Com o pivô para OO, é preciso decidir explicitamente o que se
reaproveita e o que se aposenta, para não arrastar duas visões incompatíveis no
mesmo código.

## Decisão

**Permanece intacto** (vira a fundação física do ObjectStore):

- `NativeFile` (I/O posicional + fsync), `PageFile` (superbloco, alocação;
  o campo `catalog_root` já existe), `SlottedPage`, `TableHeap`,
  `endian.hpp`, `BinaryReader`/`BinaryWriter`, `database_check`/`repair`,
  `ScratchPagePool`.

**É absorvido**:

- `Value`/`DataType` → `AttributeValue`/`AttributeType`
  ([ADR-003](ADR-003-tipos-e-encoding.md)).

**É removido na Fase 2** (quando o caminho vertical OO os substitui, não antes):

- `catalog.hpp/cpp`, `table.hpp/cpp`, `schema.*`, `row.hpp`;
- comandos relacionais da CLI (`record insert` relacional, etc.);
- testes `catalog_test.cpp` e `model_test.cpp` (este substituído por
  `object_model_test.cpp`).

**É marcado como supersedido** (não apagado, para preservar o histórico da
decisão): `PLANO_DE_DESENVOLVIMENTO.md`, `ESCOPO_MVP.md`, `README.md` e as ADRs
[0001](0001-formato-de-armazenamento.md)/[0002](0002-tipos-e-erros.md) —
todos com aviso apontando para [PLANO_ODB.md](../PLANO_ODB.md).

## Consequências

Nada do trabalho de storage é descartado — ele é exatamente a base sobre a qual
o modelo OO é construído. O modelo relacional some de uma vez na Fase 2,
evitando um período longo com dois modelos coexistindo. O histórico das
decisões relacionais permanece legível, marcado como superado.
