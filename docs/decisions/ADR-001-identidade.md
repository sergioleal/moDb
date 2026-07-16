# ADR-001 — Identidade de objetos

- Estado: aceito para o MVP OO
- Data: 2026-07-16
- Contexto do pivô: o produto deixou de ser relacional e passou a ser um banco
  Orientado a Objetos (ver [PLANO_ODB.md](../PLANO_ODB.md)). Esta é a primeira
  decisão da série OO; substitui, junto com as demais ADR-00X, o modelo das
  ADRs legadas [0001](0001-formato-de-armazenamento.md) e
  [0002](0002-tipos-e-erros.md).

## Contexto

Todo objeto precisa de identidade permanente, independente da sua localização
física — o endereço pode mudar (compactação, movimentação de página) sem que a
identidade mude. Relacionamentos referenciam objetos por essa identidade, nunca
por ponteiro ou endereço. As larguras dos identificadores precisam ser fixadas
antes do formato de arquivo, pois aparecem em headers, no mapa de identidade e
no protocolo.

## Decisão

Identificadores fortes (struct com um único campo `value` e `operator==`
default, como `PageId`):

| Id | Persistência | Largura | Regra |
|---|---|---|---|
| `ObjectId` | persistente | u64 | monotônico, **nunca reutilizado**; `0` = inválido/nulo |
| `TypeDefinitionId` | persistente | u64 | **é o `ObjectId`** do objeto TypeDefinition (o catálogo é objeto) |
| `BaselineId` | persistente | u64 | é o `ObjectId` do objeto Baseline |
| `FieldId` | persistente | u16 | único dentro do tipo; nunca reutilizado entre versões do tipo |
| `BlobId` | persistente | u64 | `PageId` da primeira página do blob; `0` = ausente |
| `DatabaseId` | runtime | u32 | atribuído pelo `DatabaseRegistry`; **nunca persistido** |

- A identidade (`ObjectId`) nunca muda durante a vida do objeto.
- O endereço físico (página/slot) pode mudar; a tradução vive no mapa de
  identidade (ver [ADR-005](ADR-005-mapa-de-identidade.md)).
- ObjectIds são alocados por um contador monotônico persistido; não há reuso no
  MVP (simplifica o mapa de identidade indexado diretamente por id).

## Consequências

Referências (`Ref`, `OwnedRef`) armazenam apenas `ObjectId`, sobrevivem a
qualquer reorganização física e nunca dependem de ponteiros. O não-reuso de
ObjectId permite um mapa de identidade endereçado diretamente pelo id (O(1)),
ao custo de um espaço de ids que só cresce — aceitável com u64. `DatabaseId`
ser runtime mantém os handles pequenos e independentes do arquivo.
