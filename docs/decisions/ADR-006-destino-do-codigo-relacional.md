# ADR-006 — Destino do código relacional

- Estado: aceito para o MVP OO; execução em duas etapas (ver Atualização)
- Data: 2026-07-16

## Atualização (Fase 2)

A remoção foi dividida em dois anéis para não derrubar a cobertura de testes da
camada de armazenamento, da qual o ObjectStore depende:

- **Anel 1 — removido na Fase 2:** o modelo de dados relacional propriamente
  dito — `Catalog`, `Table`, o comando `catalog` da CLI e o teste
  `catalog_test`. É o que competia diretamente com o catálogo/ObjectStore OO.
- **Anel 2 — mantido por ora, como ferramenta de armazenamento cru:**
  `Row`, `Value`/`DataType`, o codec relacional (`encode_row`/`decode_row`,
  `encode_schema`/`decode_schema`) e os comandos `record`/`heap`/`codec`.
  Esses exercitam `SlottedPage`/`TableHeap` (a fundação física do ObjectStore)
  e são usados por vários testes de storage. Reenquadrados como tooling de
  registro cru; a remoção plena fica como continuação, quando/se houver
  substituto equivalente para exercitar o storage.

O restante da decisão abaixo permanece a intenção de longo prazo.

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
