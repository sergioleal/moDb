# Rastreador de andamento — ODB++ (moDb)

> Este é o **único lugar** onde o estado atual de cada tarefa é mantido.
> Para o texto completo e o "porquê" de cada tarefa, veja
> [PLANO_ODB.md](PLANO_ODB.md) (a definição do trabalho); para o "como"
> implementar, veja [PROTOCOLO_FASES.md](PROTOCOLO_FASES.md). Este documento
> não redefine escopo — só registra progresso.

## Como usar este documento

1. Antes de começar uma tarefa, mude o status dela para `🔄 Em andamento` e
   escreva seu nome/identificador na coluna **Notas**.
2. Ao concluir (código + testes + suíte verde, ver
   [Definição de pronto](PROTOCOLO_FASES.md#defini%C3%A7%C3%A3o-de-pronto-por-fase)),
   mude para `✅ Concluída`, preencha **Notas** com o hash do commit e a data
   (`AAAA-MM-DD`), e atualize o checkbox correspondente em
   [PLANO_ODB.md](PLANO_ODB.md).
3. Se uma tarefa travar em uma decisão pendente ou dependência não resolvida,
   marque `🚫 Bloqueada` e explique o bloqueio em **Notas**.
4. Depois de mudar tarefas de uma fase, atualize a contagem
   `(concluídas/total)` no [Painel geral](#painel-geral) e, se a fase inteira
   fechar, marque a linha da fase como `✅ Concluída` lá.
5. Nunca marque uma tarefa como concluída sem o teste automatizado
   correspondente passando — a suíte (`ctest`) é a fonte de verdade sobre o
   que realmente funciona; este documento é a fonte de verdade sobre o que
   **já foi tentado e verificado**.

### Legenda de status

| Símbolo | Significado |
|---|---|
| ⬜ | Não iniciada |
| 🔄 | Em andamento |
| ✅ | Concluída (testada e na suíte verde) |
| 🚫 | Bloqueada (ver Notas) |

---

## Painel geral

| Fase | Tema | Status | Progresso | Bloqueada por |
|---|---|---|---|---|
| [0](#fase-0--decisões-e-fundações) | Decisões e fundações | ✅ Concluída | 10/10 | — |
| [1](#fase-1--modelo-de-objetos-e-catálogo-em-memória) | Modelo de objetos em memória | ⬜ Não iniciada | 0/8 | — |
| [2](#fase-2--codec-genérico-e-objectstore-persistente) | Codec genérico + ObjectStore | ⬜ Não iniciada | 0/11 | Fase 1 |
| [3](#fase-3--binding-handle-e-projectionplan) | Binding, Handle, ProjectionPlan | ⬜ Não iniciada | 0/10 | Fase 2 |
| [4](#fase-4--relacionamentos-coleções-e-blobstore) | Relacionamentos, coleções, BlobStore | ⬜ Não iniciada | 0/9 | Fase 3 |
| [5](#fase-5--transações-wal-e-recuperação) | Transações, WAL, recuperação | ⬜ Não iniciada | 0/11 | Fase 2 |
| [6](#fase-6--snapshots-e-mvcc) | Snapshots e MVCC | ⬜ Não iniciada | 0/6 | Fase 5 |
| [7](#fase-7--índices-e-consultas-em-streaming-embedded) | Índices e streaming (embedded) | ⬜ Não iniciada | 0/10 | Fases 4, 6 |
| [8](#fase-8--servidor-protocolo-binário-e-backpressure) | Servidor, protocolo, backpressure | ⬜ Não iniciada | 0/9 | Fase 7 |
| [9](#fase-9--runtime-de-módulos-de-domínio) | Runtime de módulos de domínio | ⬜ Não iniciada | 0/10 | Fases 5, 8 |
| [10](#fase-10--desempenho-e-estabilização) | Desempenho e estabilização | ⬜ Não iniciada | 0/9 | Todas |
| **Total** | | | **10/103 (~10%)** | |

**MVP OO (critério de aceite maior) = Fases 0–3.** Progresso do MVP: 10/39
tarefas (~26%).

---

## Fase 0 — Decisões e fundações

Status: ✅ **Concluída** (10/10) — commit `4928468`, 2026-07-16.
Definição completa: [PLANO_ODB.md §Fase 0](PLANO_ODB.md#fase-0--decisões-e-fundações).

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 0.1 | Escopo oficial do MVP OO e o que fica pós-MVP | ✅ | [ADR-007](decisions/ADR-007-limites-mvp-oo.md) — commit `4928468` |
| 0.2 | Representação do `ObjectId` | ✅ | [ADR-001](decisions/ADR-001-identidade.md) |
| 0.3 | Identificadores fortes (ObjectId, TypeDefinitionId, FieldId, BlobId, BaselineId, DatabaseId) | ✅ | [ADR-001](decisions/ADR-001-identidade.md) |
| 0.4 | Tipos de atributo primitivos e regras de conversão | ✅ | [ADR-003](decisions/ADR-003-tipos-e-encoding.md) |
| 0.5 | Layout do `ObjectHeader` e do payload | ✅ | [ADR-003](decisions/ADR-003-tipos-e-encoding.md) |
| 0.6 | Estratégia e formato do mapa de identidade | ✅ | [ADR-005](decisions/ADR-005-mapa-de-identidade.md) |
| 0.7 | Bootstrap do catálogo-como-objetos | ✅ | [ADR-002](decisions/ADR-002-bootstrap-do-catalogo.md) |
| 0.8 | Política para o código relacional existente | ✅ | [ADR-006](decisions/ADR-006-destino-do-codigo-relacional.md) |
| 0.9 | Marcar docs relacionais como supersedidos + glossário OO | ✅ | `README.md`, `PLANO_DE_DESENVOLVIMENTO.md`, `ESCOPO_MVP.md`, [GLOSSARIO.md](GLOSSARIO.md) |
| 0.10 | Registrar as decisões em `docs/decisions/` | ✅ | 7 ADRs (ADR-001..007) |

Critério de aceite: ✅ demonstrado — decisões revisadas e consistentes entre si.

---

## Fase 1 — Modelo de objetos e catálogo em memória

Status: ⬜ Não iniciada (0/8) — Definição completa:
[PLANO_ODB.md §Fase 1](PLANO_ODB.md#fase-1--modelo-de-objetos-e-catálogo-em-memória) ·
[PROTOCOLO_FASES.md §Fase 1](PROTOCOLO_FASES.md#fase-1--modelo-de-objetos-e-catálogo-em-memória)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 1.1 | Identificadores fortes (`ids.hpp`) | ⬜ | |
| 1.2 | `AttributeValue`/`AttributeType` (evolução de `Value`/`DataType`) | ⬜ | |
| 1.3 | `AttributeDefinition` | ⬜ | |
| 1.4 | `TypeDefinition` imutável | ⬜ | |
| 1.5 | `Baseline` imutável | ⬜ | |
| 1.6 | `TypeRegistry` em memória | ⬜ | |
| 1.7 | `validate_object` (payload lógico × TypeDefinition) | ⬜ | |
| 1.8 | Testes unitários do modelo | ⬜ | |

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.object_model` | `tests/object_model_test.cpp` | ⬜ |

Critério de aceite: ⬜ `TypeDefinition` rejeita payloads incompatíveis;
`Baseline`/`TypeDefinition` imutáveis após criação.

---

## Fase 2 — Codec genérico e ObjectStore persistente

Status: ⬜ Não iniciada (0/11) — Definição completa:
[PLANO_ODB.md §Fase 2](PLANO_ODB.md#fase-2--codec-genérico-e-objectstore-persistente) ·
[PROTOCOLO_FASES.md §Fase 2](PROTOCOLO_FASES.md#fase-2--codec-genérico-e-objectstore-persistente)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 2.1 | Codec genérico (`encode_object`/`decode_object`) | ⬜ | |
| 2.2 | `ObjectHeader` + payload sobre `SlottedPage`/`TableHeap` | ⬜ | |
| 2.3 | Mapa de identidade persistente (`IdentityMap`) | ⬜ | |
| 2.4 | `ObjectStore` (create/get/update/remove) | ⬜ | |
| 2.5 | Alocação monotônica de `ObjectId` | ⬜ | |
| 2.6 | Persistir catálogo como objetos (`CatalogStore`) | ⬜ | |
| 2.7 | Ligar raiz do catálogo ao `catalog_root` do superbloco | ⬜ | |
| 2.8 | Reconstruir `TypeRegistry`/`Baseline` na abertura | ⬜ | |
| 2.9 | Estender `database_check` (headers de objeto, IDMD/IDMP, DBRT) | ⬜ | |
| 2.10 | Aposentar modelo relacional + CLI OO mínima | ⬜ | |
| 2.11 | Teste de integração (centenas de objetos, reabertura) | ⬜ | |

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.object_codec` | `tests/object_codec_test.cpp` | ⬜ |
| `modb.identity_map` | `tests/identity_map_test.cpp` | ⬜ |
| `modb.object_store` | `tests/object_store_test.cpp` | ⬜ |
| `modb.catalog_persistence` | `tests/catalog_persistence_test.cpp` | ⬜ |

Critério de aceite (**1º caminho vertical OO**): ⬜ criar tipo + objetos,
destruir instância, reabrir arquivo, recuperar exatamente os mesmos dados.

---

## Fase 3 — Binding, Handle e ProjectionPlan

Status: ⬜ Não iniciada (0/10) — Definição completa:
[PLANO_ODB.md §Fase 3](PLANO_ODB.md#fase-3--binding-handle-e-projectionplan) ·
[PROTOCOLO_FASES.md §Fase 3](PROTOCOLO_FASES.md#fase-3--binding-handle-e-projectionplan)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 3.1 | `Binding` fluente (`BindingBuilder<T>`) | ⬜ | |
| 3.2 | Materialização payload ↔ objeto C++ | ⬜ | |
| 3.3 | `ProjectionPlan` (Copy/Convert/Default/Ignore/ResolveReference) | ⬜ | |
| 3.4 | Cache de ProjectionPlans | ⬜ | |
| 3.5 | Evolução de schema (nova TypeDefinition/Baseline) | ⬜ | |
| 3.6 | Migração preguiçosa | ⬜ | |
| 3.7 | `register_migration(...)` | ⬜ | |
| 3.8 | `Database`/`DatabaseRegistry` | ⬜ | |
| 3.9 | `Handle<T>` (get/set) | ⬜ | |
| 3.10 | Testes de evolução (add/remove/convert/migração) | ⬜ | |

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.binding` | `tests/binding_test.cpp` | ⬜ |
| `modb.projection` | `tests/projection_test.cpp` | ⬜ |
| `modb.schema_evolution` | `tests/schema_evolution_test.cpp` | ⬜ |

Critério de aceite (**MVP OO completo**): ⬜ app v1 grava
`Employee{name,salary}`; app v2 (+`country`) lê o objeto antigo com o default,
sem migração manual. **Este é o critério de aceite do MVP OO inteiro
(Fases 0–3).**

---

## Fase 4 — Relacionamentos, coleções e BlobStore

Status: ⬜ Não iniciada (0/9) — Definição completa:
[PLANO_ODB.md §Fase 4](PLANO_ODB.md#fase-4--relacionamentos-coleções-e-blobstore) ·
[PROTOCOLO_FASES.md §Fase 4](PROTOCOLO_FASES.md#fase-4--relacionamentos-coleções-e-blobstore)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 4.1 | `Ref<T>` (associação) | ⬜ | |
| 4.2 | `Embedded<T>` | ⬜ | |
| 4.3 | `OwnedRef<T>` (composição, cascata) | ⬜ | |
| 4.4 | `BlobStore` (páginas encadeadas) | ⬜ | |
| 4.5 | `PersistentVector<T>` | ⬜ | |
| 4.6 | `PersistentSet<T>` / `PersistentMap<K,V>` | ⬜ | |
| 4.7 | Política de integridade para `Ref` órfã | ⬜ | |
| 4.8 | Estender `database_check` (blobs, coleções, cascatas) | ⬜ | |
| 4.9 | Testes de grafo/cascata/coleção/blob | ⬜ | |

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.blob_store` | `tests/blob_store_test.cpp` | ⬜ |
| `modb.relationship` | `tests/relationship_test.cpp` | ⬜ |
| `modb.collection` | `tests/collection_test.cpp` | ⬜ |

Critério de aceite: ⬜ grafo `Employee→Department`/`Employee◆Address`/
`Employee.projects` sobrevive à reabertura com cascata correta.

---

## Fase 5 — Transações, WAL e recuperação

Status: ⬜ Não iniciada (0/11) — Definição completa:
[PLANO_ODB.md §Fase 5](PLANO_ODB.md#fase-5--transações-wal-e-recuperação) ·
[PROTOCOLO_FASES.md §Fase 5](PROTOCOLO_FASES.md#fase-5--transações-wal-e-recuperação)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 5.1 | Estados e API de `Transaction` | ⬜ | |
| 5.2 | Exigir `Transaction&` em toda escrita pública | ⬜ | |
| 5.3 | Formato do WAL | ⬜ | |
| 5.4 | WAL sincronizado antes das páginas de dados | ⬜ | |
| 5.5 | `PageCache` com páginas sujas (embrião do BufferPool) | ⬜ | |
| 5.6 | Recuperação na abertura | ⬜ | |
| 5.7 | Rollback | ⬜ | |
| 5.8 | Estratégia de checkpoint | ⬜ | |
| 5.9 | Contrato commit/rollback automático por exceção | ⬜ | |
| 5.10 | Testes de falha simulada (failpoints) | ⬜ | |
| 5.11 | Testes de atomicidade/durabilidade/idempotência | ⬜ | |

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.wal` | `tests/wal_test.cpp` | ⬜ |
| `modb.recovery` | `tests/recovery_test.cpp` | ⬜ |
| `modb.failpoint` | `tests/failpoint_test.cpp` | ⬜ |

Critério de aceite: ⬜ matriz de failpoints 100% verde — nenhuma transação
aparece parcialmente aplicada.

---

## Fase 6 — Snapshots e MVCC

Status: ⬜ Não iniciada (0/6) — Definição completa:
[PLANO_ODB.md §Fase 6](PLANO_ODB.md#fase-6--snapshots-e-mvcc) ·
[PROTOCOLO_FASES.md §Fase 6](PROTOCOLO_FASES.md#fase-6--snapshots-e-mvcc)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 6.1 | Modelo de versão por objeto (ADR-009: épocas single-writer) | ⬜ | |
| 6.2 | `Snapshot` associado a transação/consulta | ⬜ | |
| 6.3 | Isolamento: snapshot não vê escritas posteriores | ⬜ | |
| 6.4 | Retenção e GC de versões antigas | ⬜ | |
| 6.5 | Política de lock inicial para escritores | ⬜ | |
| 6.6 | Testes de consulta longa com commits concorrentes | ⬜ | |

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.snapshot` | `tests/snapshot_test.cpp` | ⬜ |

Critério de aceite: ⬜ scan sob snapshot produz estado idêntico ao da época,
com commits concorrentes intercalados.

---

## Fase 7 — Índices e consultas em streaming (embedded)

Status: ⬜ Não iniciada (0/10) — Definição completa:
[PLANO_ODB.md §Fase 7](PLANO_ODB.md#fase-7--índices-e-consultas-em-streaming-embedded) ·
[PROTOCOLO_FASES.md §Fase 7](PROTOCOLO_FASES.md#fase-7--índices-e-consultas-em-streaming-embedded)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 7.1 | `IndexDefinition` + B+ tree persistente | ⬜ | |
| 7.2 | Cursor de varredura com estado mínimo | ⬜ | |
| 7.3 | Coroutines C++20 (`Generator<T>`) | ⬜ | |
| 7.4 | Operadores streaming (Scan/Index Scan/Predicate/Projection/Limit) | ⬜ | |
| 7.5 | Operadores bloqueantes/parciais (Sort/Aggregate/Distinct/Top-K/Merge) | ⬜ | |
| 7.6 | Planner (natureza do operador, estimativa de TTFR) | ⬜ | |
| 7.7 | Consulta abre e mantém `Snapshot` até o fim do fluxo | ⬜ | |
| 7.8 | Cancelamento cooperativo do cursor | ⬜ | |
| 7.9 | API embedded (`query.stream()`) | ⬜ | |
| 7.10 | Benchmarks de TTFR e memória constante | ⬜ | |

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.btree` | `tests/btree_test.cpp` | ⬜ |
| `modb.generator` | `tests/generator_test.cpp` | ⬜ |
| `modb.streaming_query` | `tests/streaming_query_test.cpp` | ⬜ |
| `modb.planner` | `tests/planner_test.cpp` | ⬜ |

Critério de aceite: ⬜ 1º resultado em ≤ 2 páginas lidas sobre 100k objetos
(TTFR); buscas por chave via índice.

---

## Fase 8 — Servidor, protocolo binário e backpressure

Status: ⬜ Não iniciada (0/9) — Definição completa:
[PLANO_ODB.md §Fase 8](PLANO_ODB.md#fase-8--servidor-protocolo-binário-e-backpressure) ·
[PROTOCOLO_FASES.md §Fase 8](PROTOCOLO_FASES.md#fase-8--servidor-protocolo-binário-e-backpressure)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 8.1 | ADR de rede e modelo de concorrência do servidor | ⬜ | |
| 8.2 | Processo servidor hospedando `DatabaseRegistry` | ⬜ | |
| 8.3 | Protocolo binário (Begin/Object/End/Error) | ⬜ | |
| 8.4 | Framing na camada de transporte | ⬜ | |
| 8.5 | Serialização de objetos para a rede (reusa o codec) | ⬜ | |
| 8.6 | Backpressure fim-a-fim | ⬜ | |
| 8.7 | Cliente C++ assíncrono | ⬜ | |
| 8.8 | Cancelamento e políticas de timeout | ⬜ | |
| 8.9 | Testes de cliente lento/desconexão/erro no meio do fluxo | ⬜ | |

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.protocol` | `tests/protocol_test.cpp` | ⬜ |
| `modb.server_streaming` | `tests/server_streaming_test.cpp` | ⬜ |

Critério de aceite: ⬜ backpressure comprovado (produção casada ao consumo,
sem crescer memória) com cliente lento.

---

## Fase 9 — Runtime de módulos de domínio

Status: ⬜ Não iniciada (0/10) — Definição completa:
[PLANO_ODB.md §Fase 9](PLANO_ODB.md#fase-9--runtime-de-módulos-de-domínio) ·
[PROTOCOLO_FASES.md §Fase 9](PROTOCOLO_FASES.md#fase-9--runtime-de-módulos-de-domínio)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 9.1 | Interface `Operation` + `OperationResult` | ⬜ | |
| 9.2 | `ExecutionContext` (transaction/objects/logger apenas) | ⬜ | |
| 9.3 | `OperationRegistry` | ⬜ | |
| 9.4 | Despacho pelo protocolo (OperationId + args) | ⬜ | |
| 9.5 | Contrato transacional (commit/rollback automático) | ⬜ | |
| 9.6 | `ModuleManifest` + validação de compatibilidade | ⬜ | |
| 9.7 | `client.call<Op>(...)` | ⬜ | |
| 9.8 | Migrações como Operations | ⬜ | |
| 9.9 | Documentar modelo de falhas (crash/supervisor/WAL recovery) | ⬜ | |
| 9.10 | Exemplo `TransferFunds` completo + teste de atomicidade | ⬜ | |

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.operation` | `tests/operation_test.cpp` | ⬜ |
| `modb.operation_server` | `tests/operation_server_test.cpp` | ⬜ |

Critério de aceite: ⬜ `TransferFunds` atômico via `client.call`, rollback em
exceção, consistente após crash simulado + recovery.

---

## Fase 10 — Desempenho e estabilização

Status: ⬜ Não iniciada (0/9) — Definição completa:
[PLANO_ODB.md §Fase 10](PLANO_ODB.md#fase-10--desempenho-e-estabilização) ·
[PROTOCOLO_FASES.md §Fase 10](PROTOCOLO_FASES.md#fase-10--desempenho-e-estabilização)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 10.1 | Benchmarks reproduzíveis (TTFR, throughput, materialização) | ⬜ | |
| 10.2 | Completar o BufferPool (LRU, pin/unpin, métricas) | ⬜ | |
| 10.3 | Profiling antes de cada otimização | ⬜ | |
| 10.4 | Fuzzing dos decoders | ⬜ | |
| 10.5 | Testar bancos maiores que o cache | ⬜ | |
| 10.6 | Política de compatibilidade (formato + protocolo) | ⬜ | |
| 10.7 | Estabilizar e documentar a API pública | ⬜ | |
| 10.8 | Reescrever `README.md`/formato de arquivo | ⬜ | |
| 10.9 | Guia de backup/restauração/diagnóstico | ⬜ | |

### Testes/artefatos desta fase (não são CTest — ver protocolo)

| Item | Local | Status |
|---|---|---|
| `modb.buffer_pool` (teste) | `tests/buffer_pool_test.cpp` | ⬜ |
| Benchmarks (alvo separado, fora do ctest) | `benchmarks/` | ⬜ |
| Alvos de fuzzing (preset `fuzz`) | `tests/fuzz/` | ⬜ |

Critério de aceite: ⬜ benchmarks reproduzíveis, regressões detectadas
automaticamente, interfaces públicas documentadas.

---

## Histórico de fechamento de fases

| Fase | Data de conclusão | Commit |
|---|---|---|
| 0 | 2026-07-16 | `4928468` |
