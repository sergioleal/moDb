# ADR-003 — Tipos de atributo e encoding de valores

- Estado: aceito para o MVP OO
- Data: 2026-07-16
- Sucede: as decisões de tipo da ADR legada
  [0002](0002-tipos-e-erros.md) (tipos SQL `INTEGER`/`REAL`/...), agora
  reexpressas como tipos de atributo de objeto.

## Contexto

O codec é único e genérico: interpreta qualquer objeto a partir do catálogo,
sem conhecer classes C++ (ver [arquitetura.md](../arquitetura.md) §21). Para
isso, o payload precisa ser autodescritivo por tags de tipo, e o conjunto de
tipos e seu encoding binário precisam ser fixados antes de qualquer escrita.
Dados vindos do arquivo são não confiáveis e cada decodificação valida limites
antes de alocar ou ler.

## Decisão

Tag de tipo (`AttributeType`, u8) e encoding do valor no payload:

| Tag | Tipo | Encoding do valor |
|---|---|---|
| 0 | `null` | (sem bytes) |
| 1 | `boolean` | u8 (0/1; outro valor = `invalid_encoding`) |
| 2 | `int64` | u64 LE (complemento de dois via `std::bit_cast`) |
| 3 | `float64` | u64 LE (bits IEEE-754 via `std::bit_cast`) |
| 4 | `string` | u32 comprimento + bytes UTF-8 |
| 5 | `bytes` | u32 comprimento + bytes crus |
| 6 | `ref` | u64 `ObjectId` |
| 7 | `blob` | u64 `BlobId` |
| 8 | `embedded` | u32 comprimento + payload aninhado (formato de objeto da Fase 2) |

- Inteiros e reais são reinterpretados por `std::bit_cast` para u64 e gravados
  em little-endian por `store_le`/`load_le`
  ([endian.hpp](../include/modb/storage/endian.hpp)).
- `null` é um estado separado, nunca um sentinela de valor.
- Conversões permitidas no `ProjectionPlan::Convert` (Fase 3):
  `int64 ↔ float64` (float→int trunca; overflow → erro) e `boolean → int64`.
  Qualquer outra conversão exige migração registrada
  (`register_migration`).
- Conversões implícitas com perda silenciosa são proibidas.

## Consequências

O formato independe de ABI, padding e endianness nativa. O payload
autodescritivo permite decodificar sem a `TypeDefinition` em mãos (a validação
semântica contra o tipo é feita por quem chama), o que sustenta o bootstrap do
catálogo ([ADR-002](ADR-002-bootstrap-do-catalogo.md)) e a evolução de schema.
Adicionar um tipo novo é acrescentar uma tag, nunca reinterpretar tags
existentes.
