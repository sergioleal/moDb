# ADR-002 — Bootstrap do catálogo (ObjectIds reservados)

- Estado: aceito para o MVP OO
- Data: 2026-07-16

## Contexto

O catálogo do ODB++ também é composto por objetos: `TypeDefinition`,
`AttributeDefinition`, `Baseline` são gravados no mesmo ObjectStore, usando o
mesmo codec genérico dos objetos de usuário (ver
[arquitetura.md](../arquitetura.md) §15 e §21). Isso cria um problema de
bootstrap: para decodificar uma `TypeDefinition` persistida seria preciso já
conhecer a `TypeDefinition` que descreve `TypeDefinition` — uma recursão sem
fundo.

## Decisão

- Os `ObjectId` de `1` a `15` são **reservados** para meta-objetos.
- Os três primeiros são **meta-tipos compilados no motor** (nunca lidos do
  disco):

  | ObjectId | Meta-tipo |
  |---|---|
  | 1 | `TypeDefinition` (o tipo que descreve tipos) |
  | 2 | `AttributeDefinition` |
  | 3 | `Baseline` |

- Objetos de catálogo gravados em disco usam esses ids como
  `TypeDefinitionId`. O motor decodifica uma `TypeDefinition` persistida
  usando a descrição compilada do meta-tipo 1 — sem ler nada do disco para
  isso.
- Os ids `4..15` ficam reservados para meta-objetos futuros
  (`IndexDefinition`, `RelationshipDefinition`, `ConstraintDefinition`).
- O primeiro `ObjectId` de usuário é `16`
  (`first_user_object_id`).

## Consequências

O catálogo é uniforme (tudo é objeto, sem estruturas especiais) sem cair em
recursão infinita: existe um piso fixo, conhecido em tempo de compilação, que
ancora a decodificação. O custo é reservar 15 ids e manter as três descrições
de meta-tipo no código, sincronizadas com o formato do catálogo persistido
(ver [ADR-003](ADR-003-tipos-e-encoding.md) e a Fase 2 em
[PROTOCOLO_FASES.md](../PROTOCOLO_FASES.md)).
