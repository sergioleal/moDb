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
| [1](#fase-1--modelo-de-objetos-e-catálogo-em-memória) | Modelo de objetos em memória | ✅ Concluída | 8/8 | — |
| [2](#fase-2--codec-genérico-e-objectstore-persistente) | Codec genérico + ObjectStore | ✅ Concluída | 11/11 | — |
| [3](#fase-3--binding-handle-e-projectionplan) | Binding, Handle, ProjectionPlan | ✅ Concluída | 10/10 | Fase 2 |
| [4](#fase-4--relacionamentos-coleções-e-blobstore) | Relacionamentos, coleções, BlobStore | ✅ Concluída | 9/9 | Fase 3 |
| [5](#fase-5--transações-wal-e-recuperação) | Transações, WAL, recuperação | ✅ Concluída | 11/11 | Fase 2 |
| [6](#fase-6--snapshots-e-mvcc) | Snapshots e MVCC | ✅ Concluída | 9/9 | Fase 5 |
| [7](#fase-7--índices-e-consultas-em-streaming-embedded) | Índices e streaming (embedded) | ✅ Concluída | 14/14 | Fases 4, 6 |
| [8](#fase-8--servidor-protocolo-binário-e-backpressure) | Servidor, protocolo, backpressure | 🔄 Em andamento | 2/12 | Fase 7 |
| [9](#fase-9--runtime-de-módulos-de-domínio) | Runtime de módulos de domínio | ⬜ Não iniciada | 0/10 | Fases 5, 8 |
| [10](#fase-10--desempenho-e-estabilização) | Desempenho e estabilização | ⬜ Não iniciada | 0/9 | Todas |
| [11](#fase-11--container-serverless) | Container serverless | ⬜ Não iniciada | 0/11 | Fases 8, 9, 10 |
| **Total** | | | **84/124 (~68%)** | |

**MVP OO (critério de aceite maior) = Fases 0–3.** Progresso do MVP: 29/39
tarefas (~74%).

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

Status: ✅ **Concluída** (8/8) — commit `bfcc5ef`, 2026-07-16.
Definição completa:
[PLANO_ODB.md §Fase 1](PLANO_ODB.md#fase-1--modelo-de-objetos-e-catálogo-em-memória) ·
[PROTOCOLO_FASES.md §Fase 1](PROTOCOLO_FASES.md#fase-1--modelo-de-objetos-e-catálogo-em-memória)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 1.1 | Identificadores fortes (`ids.hpp`) | ✅ | `include/modb/object/ids.hpp` — commit `bfcc5ef` |
| 1.2 | `AttributeValue`/`AttributeType` (evolução de `Value`/`DataType`) | ✅ | `include/modb/object/attribute_value.hpp` |
| 1.3 | `AttributeDefinition` | ✅ | `include/modb/object/type_definition.hpp` |
| 1.4 | `TypeDefinition` imutável | ✅ | Herança/constraints ficam fora do escopo definido (não fazem parte do MVP OO) |
| 1.5 | `Baseline` imutável | ✅ | `include/modb/object/baseline.hpp` |
| 1.6 | `TypeRegistry` em memória | ✅ | `include/modb/object/type_registry.hpp` |
| 1.7 | `validate_object` (payload lógico × TypeDefinition) | ✅ | `src/object/type_definition.cpp` |
| 1.8 | Testes unitários do modelo | ✅ | `tests/object_model_test.cpp` |

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.object_model` | `tests/object_model_test.cpp` | ✅ |

Critério de aceite: ✅ `TypeDefinition::create`/`Baseline::create` rejeitam
entradas incompatíveis (`duplicate_field`, `too_many_columns`,
`invalid_identifier`, `type_mismatch`, `null_constraint_violation`,
`duplicate_type`, `invalid_object_id`); ambas as classes não possuem mutador
público — evolução é sempre um novo objeto estampado, nunca mutação in-place.

**Extra (fora da lista de tarefas):** comando `modb types` adicionado à CLI —
demo em memória do modelo desta fase (`TypeDefinition`/`TypeRegistry`/
`validate_object`), no mesmo espírito de `modb catalog`. Não persiste nada;
não é uma tarefa de nenhuma fase, só uma vitrine útil enquanto a Fase 2 não
chega. Ver [USO_DA_CLI.md](USO_DA_CLI.md#modb-types--modelo-de-objetos-em-memória-odb).

---

## Fase 2 — Codec genérico e ObjectStore persistente

Status: ✅ **Concluída** (11/11) — critério de aceite verde. Commits
`8d23923`, `2266503`, `cc6ee9b`, `85a5712` e a remoção do Anel 1 relacional.
Definição completa:
[PLANO_ODB.md §Fase 2](PLANO_ODB.md#fase-2--codec-genérico-e-objectstore-persistente) ·
[PROTOCOLO_FASES.md §Fase 2](PROTOCOLO_FASES.md#fase-2--codec-genérico-e-objectstore-persistente)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 2.1 | Codec genérico (`encode_object`/`decode_object`) | ✅ | `object_codec` — commit `8d23923` |
| 2.2 | `ObjectHeader` + payload sobre `SlottedPage`/`TableHeap` | ✅ | formato de registro no codec |
| 2.3 | Mapa de identidade persistente (`IdentityMap`) | ✅ | IDMD/IDMP — commit `2266503` |
| 2.4 | `ObjectStore` (create/get/update/remove) | ✅ | commit `cc6ee9b` |
| 2.5 | Alocação monotônica de `ObjectId` | ✅ | contador no DBRT, gravado antes do registro |
| 2.6 | Persistir catálogo como objetos (`CatalogStore`) | ✅ | commit `cc6ee9b` |
| 2.7 | Ligar raiz do catálogo ao `catalog_root` do superbloco | ✅ | via DBRT (ADR-004) |
| 2.8 | Reconstruir `TypeRegistry`/`Baseline` na abertura | ✅ | `CatalogStore::load_all` |
| 2.9 | Estender `database_check` (DBRT/IDMD/IDMP) | ✅ | commit `85a5712` |
| 2.10 | Aposentar modelo relacional + CLI OO mínima | ✅ | CLI OO (`type`/`object`) ✅; Anel 1 relacional removido (Catalog/Table/comando `catalog`/catalog_test). Row/Value/codec relacional mantidos como tooling de storage cru (ADR-006, Atualização) |
| 2.11 | Teste de integração (centenas de objetos, reabertura) | ✅ | critério de aceite (500 objetos) |

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.object_codec` | `tests/object_codec_test.cpp` | ✅ |
| `modb.identity_map` | `tests/identity_map_test.cpp` | ✅ |
| `modb.object_store` | `tests/object_store_test.cpp` | ✅ |
| `modb.catalog_persistence` | `tests/catalog_persistence_test.cpp` | ✅ |

Critério de aceite (**1º caminho vertical OO**): ✅ criar tipo + objetos,
destruir instância, reabrir arquivo, recuperar exatamente os mesmos dados
(teste de 500 objetos + verificação end-to-end pela CLI entre processos).

---

## Fase 3 — Binding, Handle e ProjectionPlan

Status: ✅ Concluída (10/10) — commit `264213f`, 2026-07-17. Definição completa:
[PLANO_ODB.md §Fase 3](PLANO_ODB.md#fase-3--binding-handle-e-projectionplan) ·
[PROTOCOLO_FASES.md §Fase 3](PROTOCOLO_FASES.md#fase-3--binding-handle-e-projectionplan)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 3.1 | `Binding` fluente (`BindingBuilder<T>`) | ✅ | defaults e tipos escalares/ref |
| 3.2 | Materialização payload ↔ objeto C++ | ✅ | Binding + codec genérico |
| 3.3 | `ProjectionPlan` (Copy/Convert/Default/Ignore/ResolveReference) | ✅ | conversões com overflow/perda validados |
| 3.4 | Cache de ProjectionPlans | ✅ | por TypeDefinitionId no binding corrente |
| 3.5 | Evolução de schema (nova TypeDefinition/Baseline) | ✅ | versões históricas preservadas |
| 3.6 | Migração preguiçosa | ✅ | `Handle::set` regrava na definição corrente |
| 3.7 | `register_migration(...)` | ✅ | migração registrada precede projeção automática |
| 3.8 | `Database`/`DatabaseRegistry` | ✅ | registro por DatabaseId |
| 3.9 | `Handle<T>` (get/set) | ✅ | identidade DatabaseId + ObjectId |
| 3.10 | Testes de evolução (add/remove/convert/migração) | ✅ | três alvos CTest escritos |

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.binding` | `tests/binding_test.cpp` | ✅ |
| `modb.projection` | `tests/projection_test.cpp` | ✅ |
| `modb.schema_evolution` | `tests/schema_evolution_test.cpp` | ✅ |

Critério de aceite (**MVP OO completo**): ✅ app v1 grava
`Employee{name,salary}`; app v2 (+`country`) lê o objeto antigo com o default,
sem migração manual. **Este é o critério de aceite do MVP OO inteiro
(Fases 0–3).**

O cenário está automatizado em `modb.schema_evolution` e passa nas configurações
Debug com warnings como erros e `sanitizers`/hardening do MinGW.

---

## Fase 4 — Relacionamentos, coleções e BlobStore

Status: ✅ Concluída (9/9) — critério de aceite verde (grafo do critério em
`modb.collection`). Commit `f902a0b`, 2026-07-17. Definição completa:
[PLANO_ODB.md §Fase 4](PLANO_ODB.md#fase-4--relacionamentos-coleções-e-blobstore) ·
[PROTOCOLO_FASES.md §Fase 4](PROTOCOLO_FASES.md#fase-4--relacionamentos-coleções-e-blobstore)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 4.1 | `Ref<T>` (associação) | ✅ | `ref.hpp`; tag `ref`, resolve via `Database::get<T>` |
| 4.2 | `Embedded<T>` | ✅ | `EmbeddedValue` (sub-payload opaco) + `BindingBuilder::embedded` com binding aninhado |
| 4.3 | `OwnedRef<T>` (composição, cascata) | ✅ | `is_owned`; cascata profundidade-primeiro em `Database::remove` |
| 4.4 | `BlobStore` (páginas encadeadas) | ✅ | `blob_store.hpp/.cpp` (BLBP); create/read/read_chunks/rewrite/remove |
| 4.5 | `PersistentVector<T>` | ✅ | `collection.hpp`; push_back = rewrite O(n) (limitação de MVP documentada) |
| 4.6 | `PersistentSet<T>` / `PersistentMap<K,V>` | ✅ | ordenados por codificação canônica, busca binária na leitura |
| 4.7 | Política de integridade para `Ref` órfã | ✅ | [ADR-008](decisions/ADR-008-integridade-de-referencias.md): Ref pendente detectável; cascata só em OwnedRef; ciclo → `invalid_argument` |
| 4.8 | Estender `database_check` (blobs, coleções, cascatas) | ✅ | Reconhecimento + validação estrutural de BLBP (versão/comprimento). Checagem semântica (cadeia de blob, refs órfãs, invariantes de catálogo) deferida — ver nota abaixo |
| 4.9 | Testes de grafo/cascata/coleção/blob | ✅ | três alvos CTest; grafo do critério verde |

**Nota de integridade (verificador semântico deferido).** A parte de storage da
4.8 está feita: o `storage::check_database` reconhece páginas BLBP e valida o
cabeçalho (versão e comprimento) — sem isso, qualquer banco com coleções/blobs
seria acusado de "formato desconhecido". O que fica deferido é um verificador
**semântico** na camada de objetos (não em `storage::check_database`, que é só de
storage por decisão de camada,
[database_check.cpp](../src/storage/database_check.cpp#L176)). Esse verificador
cobriria, como **aviso** e não erro:

1. cadeia de blob percorrível sem ciclo e com comprimentos coerentes (hoje
   validado só na leitura via `BlobStore::read`, não em varredura offline);
2. `Ref` órfã (alvo removido) — detectável na resolução (`record_not_found`),
   mas não varrida proativamente (ADR-008);
3. invariantes de catálogo herdados da Fase 3: todo objeto aponta para um
   `type_definition_id` existente; toda `Baseline` referencia `TypeDefinitionId`s
   existentes; id-map aponta para registro vivo. Hoje `CatalogStore::load_all`
   ([catalog_store.cpp](../src/object/catalog_store.cpp#L249)) revalida tipos e
   baselines individualmente, sem checagem cruzada.

Recovery é da Fase 5 — não há o que adicionar até lá.

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.blob_store` | `tests/blob_store_test.cpp` | ✅ |
| `modb.relationship` | `tests/relationship_test.cpp` | ✅ |
| `modb.collection` | `tests/collection_test.cpp` | ✅ |

Critério de aceite: ✅ grafo `Employee→Department` (Ref), `Employee◆Address`
(OwnedRef) e `Employee.projects` (`PersistentVector<Ref<Project>>`) sobrevive à
reabertura; remover `Employee` remove o owned em cascata e preserva os alvos de
associação (`modb.collection`, caso "grafo do critério"). Validado em Debug,
`-Werror` e `sanitizers`: 42/42.

**Extra (fora da lista de tarefas):** a CLI ganhou três grupos para exercitar a
Fase 4 (no espírito do `modb oo` da Fase 3) — `modb blob` (put/get/info sobre o
BlobStore), `modb graph demo` (associação + embedded + cascata + vector-de-refs
de ponta a ponta) e `modb coll demo` (vector/set/map). Cobertos por seis testes
`modb.cli.*` (help + demos); suíte total em 48/48. Ver
[USO_DA_CLI.md](USO_DA_CLI.md#modb-blob--binários-encadeados-odb-fase-4).

---

## Fase 5 — Transações, WAL e recuperação

Status: ✅ Concluída (11/11) — matriz de failpoints verde. Commit `bc51f6e`,
2026-07-17. Definição completa:
[PLANO_ODB.md §Fase 5](PLANO_ODB.md#fase-5--transações-wal-e-recuperação) ·
[PROTOCOLO_FASES.md §Fase 5](PROTOCOLO_FASES.md#fase-5--transações-wal-e-recuperação) ·
Garantias: [GARANTIAS_TRANSACIONAIS.md](GARANTIAS_TRANSACIONAIS.md)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 5.1 | Estados e API de `Transaction` | ✅ | RAII move-only; begin/commit/rollback; destrutor reverte se não committada |
| 5.2 | Exigir `Transaction&` em toda escrita pública | ✅ | create/update/remove + mutações de coleção validam tx ativa; `bind()` grava o catálogo em transação interna |
| 5.3 | Formato do WAL | ✅ | `MOWL` + registros com CRC32; fim lógico por CRC/truncamento (`wal.hpp`) |
| 5.4 | WAL sincronizado antes das páginas de dados | ✅ | imagens+commit sincronizados antes do apply (`commit_transaction`) |
| 5.5 | `PageCache` com páginas sujas (embrião do BufferPool) | ✅ | buffer de páginas sujas dentro do `PageFile` (mesmo papel; ver relatório §6) |
| 5.6 | Recuperação na abertura | ✅ | `tx::recover` reaplica tx commitadas, idempotente, remove o WAL |
| 5.7 | Rollback | ✅ | explícito e por destrutor; commit que falha mantém a tx ativa p/ reverter |
| 5.8 | Estratégia de checkpoint | ✅ | checkpoint = remoção do WAL após apply durável |
| 5.9 | Contrato commit/rollback automático por exceção | ✅ | `Database::transact(fn)`: Ok→commit, erro/exceção→rollback |
| 5.10 | Testes de falha simulada (failpoints) | ✅ | `FailpointWalSink` (I/O real) + apply-failpoint; sem página escrita à mão |
| 5.11 | Testes de atomicidade/durabilidade/idempotência | ✅ | `modb.recovery` (redo/uncommitted/idempotente/rollback/transact) |

### Testes automatizados desta fase

| Teste (CTest) | Arquivo | Status |
|---|---|---|
| `modb.wal` | `tests/wal_test.cpp` | ✅ |
| `modb.recovery` | `tests/recovery_test.cpp` | ✅ |
| `modb.failpoint` | `tests/failpoint_test.cpp` | ✅ |

Critério de aceite: ✅ matriz de failpoints 100% verde — nenhuma transação
aparece parcialmente aplicada. Suíte completa (59 testes) verde em Debug,
`-Werror` e `sanitizers`. Garantias documentadas em
[GARANTIAS_TRANSACIONAIS.md](GARANTIAS_TRANSACIONAIS.md).

**Nota de ambiente:** a toolchain do CLion migrou para GCC 15.2, cujo
`libwinpthread.a` estático quebra o link `-static`
(`__intrinsic_setjmpex`/`__ms_vsnprintf` indefinidos). O link estático virou a
opção `MODB_STATIC_MINGW_RUNTIME` (padrão OFF; link dinâmico resolve as DLLs
pelo PATH). Isso afetava **todos** os alvos, não só os de tx.

**Correção pós-fechamento (encontrada pela CLI, não pela suíte original):**
`Database::rollback_transaction()` só descartava o buffer de páginas do
`PageFile`; os contadores em memória de `TableHeap`/`IdentityMap` (avançados
otimisticamente durante a transação) não eram revertidos, corrompendo a raiz do
heap na próxima escrita real após um `rollback → create → commit → reopen`.
Corrigido com `Database::resync_store_after_rollback()` (reconstrói `store_`
via `ObjectStore::open`, puramente leitura) mais um watermark que impede o
contador de `ObjectId` de retroceder através do resync (preservando "nunca
reutilizado" do [ADR-001](decisions/ADR-001-identidade.md) mesmo após um
rollback). Coberto por um novo caso em `modb.recovery`
("rollback-then-write") e detalhado em
[GARANTIAS_TRANSACIONAIS.md §6](GARANTIAS_TRANSACIONAIS.md#6-limitações-e-desvios-documentados-mvp).

**Extra (fora da lista de tarefas):** a CLI ganhou o grupo `modb tx`
(`demo`/`crash`/`wal-info`/`get`), no espírito de `modb oo`/`graph`/`coll`.
`crash` é o mais incomum dos comandos desta sessão: ele chama `std::exit` logo
após atingir a fase de commit pedida, pulando todos os destrutores locais — uma
simulação de queda genuína através de um processo real, não um truque de teste.
Isso permitiu observar a garantia de atomicidade (`before-commit` → ausente;
`after-commit`/`mid-apply`/`before-cleanup` → presente após recuperação) via
`modb tx wal-info`/`modb tx get` em invocações separadas. Nove testes
`modb.cli.tx_*`; suíte total em 59/59. Ver
[USO_DA_CLI.md](USO_DA_CLI.md#modb-tx--transações-wal-e-recuperação-odb-fase-5).

---

## Fase 6 — Snapshots e MVCC

Status: ✅ Concluída (9/9) — quatro entregas incrementais; 6A, 6B, 6C e 6D
concluídas. Definição completa:
[PLANO_ODB.md §Fase 6](PLANO_ODB.md#fase-6--snapshots-e-mvcc) ·
[PROTOCOLO_FASES.md §Fase 6](PROTOCOLO_FASES.md#fase-6--snapshots-e-mvcc)

### Fase 6A — Épocas e formato versionado

Status: ✅ Concluída (2/2) — commit `1e08cf4`, 2026-07-17.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 6A.1 | ADR-009: épocas single-writer, limite de versões e reabertura | ✅ | `ADR-009`; IDMP v2 de 48 bytes |
| 6A.2 | Época no DBRT, IdentityMap v2 e migração v1→v2 | ✅ | DBRT v2; migração publicada pela raiz |

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.identity_map` | migração IDMP v1→v2, objetos preservados, v2 reabre sem re-migrar | ✅ |
| `modb.binding` | época 0→2→3 por commit, preservada na reabertura, DBRT v1 abre como v2 com época 0 | ✅ |
| `modb.cli.mvcc_help` | grupo `modb mvcc` (status/upgrade/tick) | ✅ |

Critério de aceite 6A: ✅ bancos v1/v2 reabrem com os mesmos objetos e época
monotônica através de commit e reabertura — demonstrado por
`modb.identity_map`/`modb.binding` e pela CLI (`modb mvcc status/upgrade/tick`,
ver [USO_DA_CLI.md §MVCC](USO_DA_CLI.md#mvcc--fase-6a)). Suíte completa 60/60
em Debug, `-Werror` e sanitizers. `database_check` estendido para diagnóstico
somente-leitura de WAL/DBRT/IDMP (nunca migra/reaplica/apaga durante o check) —
ver [RELATORIO_CHECK_RECOVERY_FASES_5_6.md](RELATORIO_CHECK_RECOVERY_FASES_5_6.md).

### Fase 6B — Snapshot e leituras consistentes

Status: ✅ Concluída (3/3) — commit `ecc80e9`, 2026-07-17.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 6B.1 | `Snapshot` RAII e registro de épocas ativas | ✅ | `Database::snapshot()`; `std::multiset<epoch>` em memória |
| 6B.2 | Visibilidade por época em `get` e `scan` | ✅ | `find_at`; `get_at`/`scan_at` filtram físicos por identidade |
| 6B.3 | `snapshot_conflict` para limite de uma versão anterior | ✅ | `check_snapshot_conflict`: só há uma posição `previous` |

Consequência física documentada (ver [ADR-009](decisions/ADR-009-epocas-e-idmp-v2.md)
e [GARANTIAS_TRANSACIONAIS.md](GARANTIAS_TRANSACIONAIS.md)): para preservar a
versão `previous`, `update` **sempre** insere um registro físico novo e `remove`
**não** apaga fisicamente; a recuperação de espaço fica para a Fase 6C. Por isso
`scan`/`scan_at` filtram cada registro físico contra a identidade resolvida,
ignorando cópias antigas/órfãs.

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.snapshot` | leitura estável, remoção/criação invisível, scan consistente e `snapshot_conflict` | ✅ |
| `modb.identity_map` | `find_at`/`current_epoch`/`has_previous` em bind→rebind→erase versionados | ✅ |
| `modb.object_store` | `update`/`remove` com época e `scan` filtrando físicos preservados | ✅ |
| `modb.cli.mvcc_snapshot_demo` | `modb mvcc snapshot-demo`: leitura estável + conflito + liberação | ✅ |

Critério de aceite 6B: ✅ um snapshot devolve o estado lógico da época em que foi
criado (imune a update/remove/criação posteriores) enquanto a leitura sem
snapshot devolve o último commit — demonstrado por `modb.snapshot` e pela CLI
(`modb mvcc snapshot-demo`). Suíte completa 62/62 em Debug, `-Werror` e
sanitizers.

### Fase 6C — Retenção, GC e concorrência

Status: ✅ Concluída (2/2) — commit `06bd103`, 2026-07-17.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 6C.1 | Retenção e GC de versões anteriores | ✅ | `Database::collect_garbage()`; reconcilia heap×identidade, respeita snapshot mais antigo |
| 6C.2 | Lock single-writer e sincronização de snapshots/GC | ✅ | `snapshot_registry_mutex_`; commits serializados pela guarda de `begin()` |

O GC roda numa transação própria (liberações passam pelo WAL). Reconcilia cada
registro físico com a identidade: a versão `current` viva nunca é tocada; uma
`previous` é liberada (e a entrada compactada por `clear_previous`) quando a
época do snapshot aberto mais antigo já é `>=` à época `current` da entrada;
cópias que não são nem `current` nem a `previous` referenciada (ex.: um
`previous` sobrescrito por uma segunda alteração) são órfãs e sempre
recuperadas. `data_record_count()` expõe o total físico para observar o efeito.

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.snapshot` | retenção (GC=0 com snapshot aberto), reciclagem (GC após fechar) e coleta de órfão (dois updates sem snapshot) | ✅ |
| `modb.cli.mvcc_gc` | `modb mvcc gc`: coleta idempotente sobre arquivo já compactado | ✅ |
| `modb.cli.mvcc_snapshot_demo` | demo estendida com retenção + reciclagem do GC | ✅ |

Critério de aceite 6C: ✅ nenhuma versão ainda visível é descartada (GC com
snapshot aberto coleta 0); ao fechar o último snapshot dependente as versões
obsoletas são recuperadas; uma segunda alteração incompatível continua
retornando `snapshot_conflict` sem escrita parcial (6B). Suíte completa 63/63 em
Debug, `-Werror` e sanitizers.

### Fase 6D — Integração e recuperação

Status: ✅ Concluída (2/2) — commit `3f65e3f`, 2026-07-17.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 6D.1 | Consulta longa com criação/update/remove intercalados | ✅ | snapshot atravessa 4 mutações; get/scan estáveis |
| 6D.2 | Migração, reabertura, recovery e limpeza de versões órfãs | ✅ | update/remove versionados via WAL; sem vazamento após reabrir |

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.mvcc_recovery` | leitura longa estável; durabilidade versionada + GC sem vazamento na reabertura; recovery de update/remove versionados; `database_check` após recovery | ✅ |
| `modb.snapshot` | matriz de snapshot/GC da 6B/6C | ✅ |
| `modb.identity_map` | migração IDMP v1→v2 (limite de formato da 6D) | ✅ |
| `modb.binding` | época monotônica após reabertura | ✅ |

Critério de aceite 6D: ✅ a matriz passa sem leitura mista (a consulta longa
enxerga sempre o estado da época), sem versão perdida (update/remove
versionados sobrevivem a recovery), sem vazamento persistente (o GC recupera as
versões órfãs após reabrir) e sem corrupção após recovery (`database_check` ok).

Critério de aceite da Fase 6: ✅ scan sob snapshot produz o estado idêntico ao da
época, mesmo com create/update/remove commitados e intercalados no mesmo
processo (`modb.snapshot`, `modb.mvcc_recovery`). Suíte completa 64/64 em Debug,
`-Werror` e sanitizers.

---

## Fase 7 — Índices e consultas em streaming (embedded)

Status: ✅ **Concluída** (14/14) — cinco entregas verticais 7A–7E.
Definição completa:
[PLANO_ODB.md §Fase 7](PLANO_ODB.md#fase-7--índices-e-consultas-em-streaming-embedded) ·
[PROTOCOLO_FASES.md §Fase 7](PROTOCOLO_FASES.md#fase-7--índices-e-consultas-em-streaming-embedded)

### Fase 7A — Consulta streaming básica

Status: ✅ Concluída (4/4) — commit `a585fab`, 2026-07-17.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 7A.1 | `Generator<Result<T>>` + cursor de scan com estado mínimo | ✅ | coroutines C++20; `TableHeap::read_page_records` + `ObjectStore::scan_stream` |
| 7A.2 | Operadores preguiçosos Scan, Predicate e Limit | ✅ | `query::{limit,filter,cancellable}`; Limit encerra o upstream |
| 7A.3 | Snapshot durante todo o fluxo + cancelamento cooperativo | ✅ | Snapshot movido para a moldura da coroutine; `CancellationToken` |
| 7A.4 | API embedded `query.stream()` | ✅ | `Database::query<T>()` fluente; filtro por nome (todas as versões) |

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.generator` | preguiça (contador + limit), composição filter/limit, propagação de erro, cancelamento, destruição precoce; **pico constante de payloads em 100 mil entradas** | ✅ |
| `modb.streaming_query` | **TTFR: `limit 1` lê ≤ 2 páginas sobre 100 mil objetos**; varredura completa; filtro; filter+limit curto-circuita; cancelamento; estabilidade do snapshot | ✅ |
| `modb.cli.query` | `modb query` (streaming, filtro, limite) sobre o tipo evoluído | ✅ |

Critério de aceite 7A: ✅ `limit 1` lê ≤ 2 páginas de dados sobre 100 mil
objetos, mantém memória O(1) (pico instrumentado de payloads permanece constante),
preserva o estado lógico do snapshot por toda a leitura e encerra o upstream ao
atingir o limite ou ao cancelar. Suíte completa 75/75 em Debug, `-Werror` e
sanitizers.

### Fase 7B — Consultas indexadas

Status: ✅ Concluída (3/3) — commit `e3d76a3`, 2026-07-18.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 7B.1 | `IndexDefinition` + B+ tree persistente (`find`/`range`) | ✅ | `index::BTree` (BTLF/BTIN, split até a raiz); `key_codec` ordenável; `IndexCatalog` no DBRT |
| 7B.2 | Manutenção transacional e recovery do índice | ✅ | `index_maintain` em create/update/remove; raiz no catálogo; recovery via WAL |
| 7B.3 | Index Scan integrado à API streaming | ✅ | `query<T>().equals/.between`; candidatos revalidados contra o snapshot |

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.btree` | inserção ordenada/embaralhada (2–3 mil), invariantes (ordem, profundidade uniforme, fill mínimo, split interno), duplicatas, faixa, remoção com borrow/merge e encolhimento de raiz, reabertura, ordem por tipo | ✅ |
| `modb.indexed_query` | criação com backfill, igualdade/faixa em ordem, duplicatas, índice lê menos páginas que scan, manutenção em update/remove, reabertura e **recovery** de objeto + índice | ✅ |
| `modb.index_catalog` | diretório IXDR multipágina (cadeia), `set_root`, reabertura completa | ✅ |
| `modb.cli.oo_index` / `modb.cli.query_indexed` | `oo employee index` + `modb query --salary` (Index Scan por igualdade) | ✅ |

 Critério de aceite 7B: ✅ buscas por igualdade e faixa usam comprovadamente a B+
 tree (uma consulta seletiva lê menos páginas que o scan completo), preservam
 duplicatas (chave composta valor+ObjectId) e permanecem corretas após commit,
 recovery (objeto e índice refeitos juntos pelo WAL) e reabertura (raiz
 persistida no catálogo). Remoção rebalanceia por borrow/merge; o diretório de
 índices cresce em cadeia IXDR. Suíte completa 75/75 em Debug, `-Werror` e
 sanitizers.

Limitação documentada (MVP): páginas liberadas por merge da B+ tree ou por
encolhimento da cadeia IXDR ficam órfãs (sem free list geral — Fase 10). O Index
Scan usa o índice corrente e revalida cada candidato contra a versão do snapshot
(sem falso-positivo; um valor alterado depois de o snapshot abrir pode causar um
miss — cenário raro no single-writer).

### Fase 7C — Projeção e transformação

Status: ✅ Concluída (4/4) — commit `b6284c4`; identidade uniforme `2f9adee`;
`ProjectedQuery::map` tipado no commit `38e6f4c`, 2026-07-18.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 7C.1 | Projection com resultados tipados | ✅ | `ProjectedRow` + `Query::select` / operadores `project` |
| 7C.2 | Computed Functions registradas no fluxo | ✅ | `Database::register_computed` + `Query::compute` / `map` |
| 7C.3 | `ObjectId` como campo projetado uniforme | ✅ | `FieldId{0}`, `ProjectedField{name="id"}` e `row.get("id")` |
| 7C.4 | Materialização tipada opcional da projeção | ✅ | `ProjectedQuery::map<Out>` → `MappedQuery<Out>` (streaming) |

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.projection_query` | project∘filter∘limit; select; compute; map tipado; ProjectedRow→classe; mapper falível; compute ausente | ✅ |
| `modb.cli.query_project` | `modb query --project id,name,salary --compute annual_salary` | ✅ |
| `modb.cli.query_materialize` | `ProjectedRow` → `map<EmployeeSummary>` → stream pela CLI | ✅ |

Critério de aceite 7C: ✅ projeções e funções computadas compõem com Scan/Index
Scan, Predicate e Limit mantendo avaliação preguiçosa e memória O(1). Suíte
completa 82/82 em Debug e sanitizers.

### Fase 7D — Ordenação e agregação

Status: ✅ Concluída (2/2) — commit `172134d`, 2026-07-18.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 7D.1 | Sort global e Top-K | ✅ | `query::sort` / `top_k`; `Query::order_by` / `top_k`; pico O(k) |
| 7D.2 | Aggregate, Distinct e Merge | ✅ | `aggregate` / `distinct` / `merge`; `OperatorNature` + `Query::nature()` |

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.aggregation_query` | sort/top_k/distinct/aggregate/merge isolados; nature; order_by/top_k/distinct_by/aggregate na API; pico O(k) no Top-K | ✅ |
| `modb.cli.query_topk` | `modb query --order-by salary --top 1` | ✅ |

Critério de aceite 7D: ✅ resultados corretos, Top-K com pico O(k) e operadores
que materializam entrada classificados como bloqueantes (não como streaming).
Suíte completa 79/79 em Debug, `-Werror` e sanitizers.

### Fase 7E — Planejamento automático e comprovação

Status: ✅ Concluída (3/3) — commit `17ad869`, 2026-07-18.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 7E.1 | Planner determinístico (Index Scan ou Scan + Predicate) | ✅ | `query::plan_query` + `Query::plan()`; fallback Scan+Predicate |
| 7E.2 | Pushdown de Limit, Top-K, `nature()` e `first_result_cost()` | ✅ | `order_by`+`limit` → Top-K; `Query::first_result_cost()` |
| 7E.3 | Benchmarks de TTFR, memória e ganho de índice | ✅ | em `modb.planner` (TTFR ≤2 págs, pico O(k), index < scan) |

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.planner` | regras do planner; index vs scan; pushdown; Top-K automático; TTFR; ganho de índice; pico O(k) | ✅ |
| `modb.cli.query_explain` | `modb query --salary … --explain` | ✅ |

Critério de aceite 7E: ✅ planner escolhe índice e aplica pushdown seguro; os
benchmarks confirmam TTFR e limites de memória. Suíte completa 81/81 em Debug,
`-Werror` e sanitizers.

Critério de aceite da Fase 7: ✅ 1º resultado em ≤ 2 páginas lidas sobre 100k
objetos (TTFR); buscas por chave via índice.

---

## Fase 8 — Servidor, protocolo binário e backpressure

Status: 🔄 Em andamento (2/12) — seis entregas verticais 8A–8F.
Definição completa:
[PLANO_ODB.md §Fase 8](PLANO_ODB.md#fase-8--servidor-protocolo-binário-e-backpressure) ·
[PROTOCOLO_FASES.md §Fase 8](PROTOCOLO_FASES.md#fase-8--servidor-protocolo-binário-e-backpressure)

### Fase 8A — Contratos e codec do protocolo

Status: ✅ Concluída (2/2) — commit `9f65f81`, tag `0.0.8a`.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 8A.1 | ADR-011: concorrência do servidor + revisão single-thread | ✅ | [ADR-011](decisions/ADR-011-concorrencia-do-servidor.md): leitor/workers/escritor; fila de 1 frame; `ScratchPagePool` isolado |
| 8A.2 | `QueryDescription` + codec de mensagens/`ObjectEnvelope`/`ObjectFrame` (`none`) | ✅ | `modb/net/protocol.hpp`; ErrorCode `protocol_error`/`frame_too_large`/`connection_closed` |

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.protocol` | round-trip; frames hostis (truncado, length, >16 MiB, diretório inválido, lixo) | ✅ |
| `modb.cli.protocol` | demo `modb protocol` em memória | ✅ |

Critério de aceite 8A: ✅ encode→decode idêntico; entradas hostis →
`protocol_error`/`frame_too_large` sem alocação gigante. Suíte completa
85/85 em Debug e sanitizers (MinGW: `_GLIBCXX_ASSERTIONS` + stack protector).

### Fase 8B — Transporte e processo servidor

Status: ⬜ Não iniciada (0/2) — tag prevista `0.0.8b`. Depende de 8A.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 8B.1 | `NativeSocket` Win32/POSIX | ⬜ | Mesmo padrão do `NativeFile` |
| 8B.2 | Processo servidor + `Hello`/`HelloOk` em porta efêmera | ⬜ | Hospeda `DatabaseRegistry`; CLI `modb serve` / ping |

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.server_streaming` (handshake) | loopback: conexão, negociação, encerramento limpo | ⬜ |

Critério de aceite 8B: ⬜ handshake e encerramento limpos; CLI demonstra
servidor e ping/info remoto.

### Fase 8C — Primeiro streaming remoto

Status: ⬜ Não iniciada (0/2) — tag prevista `0.0.8c`. Depende de 8B.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 8C.1 | Execução remota Begin → ObjectFrame(s) → End/Error | ⬜ | `QueryDescription` declarativa restrita |
| 8C.2 | Cliente C++ + `ObjectStream` incremental | ⬜ | Codec genérico; sem `PageId`/`SlotId`/`RecordId` |

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.server_streaming` (fluxo) | 10 mil objetos; ordem; independência física; erro após N | ⬜ |

Critério de aceite 8C: ⬜ fluxo íntegro de 10 mil objetos; falha parcial
entrega N + `StreamError`.

### Fase 8D — Backpressure e ciclo de recursos

Status: ⬜ Não iniciada (0/2) — tag prevista `0.0.8d`. Depende de 8C.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 8D.1 | Fila/frame limitados; socket lento suspende o generator | ⬜ | ≤ 1 frame / constante pequena em trânsito |
| 8D.2 | Instrumentação e liberação em desconexão | ⬜ | produzidos − enviados; cursor/snapshot |

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.server_streaming` (backpressure) | cliente lento 1 obj/50 ms; desconexão sem vazamento | ⬜ |

Critério de aceite 8D: ⬜ produzidos − enviados ≤ constante; memória não
cresce com o tamanho do fluxo.

### Fase 8E — Cancelamento, multiplexação e API assíncrona

Status: ⬜ Não iniciada (0/2) — tag prevista `0.0.8e`. Depende de 8D.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 8E.1 | `Cancel` durante envio; conexão reutilizável | ⬜ | Leitor desacoplado do escritor |
| 8E.2 | Multiplexação de `query_id` + `co_await stream.next()` | ⬜ | Semântica/executor na ADR-011 |

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.server_streaming` (concorrência) | cancelamento; duas consultas na mesma conexão | ⬜ |

Critério de aceite 8E: ⬜ cancelamento interrompe produção e permite nova
consulta; dois `query_id`s íntegros.

### Fase 8F — Limites, timeout, compressão e fechamento

Status: ⬜ Não iniciada (0/2) — tag prevista `0.0.8f`. Depende de 8E.

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 8F.1 | Timeout, limites de stream/frame/expansão; compressão negociada | ⬜ | Codec por benchmark; `none` obrigatório e fallback |
| 8F.2 | Suíte completa + demo CLI entre processos | ⬜ | Fecha critério da Fase 8 |

### Testes automatizados desta subfase

| Teste (CTest) | Cobre | Status |
|---|---|---|
| `modb.protocol` (compressão) | negociada; incompressível → `none`; inválida rejeitada | ⬜ |
| `modb.server_streaming` (fechamento) | limites/timeout; demo entre processos | ⬜ |

Critério de aceite 8F / da Fase 8: ⬜ cliente em outro processo com
backpressure comprovado e `StreamError` correto; compressão inválida
rejeitada sem alocação excessiva.

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
| 9.6 | `ModuleManifest` + `ModuleLoader` confiável no processo | ⬜ | [ADR-012](decisions/ADR-012-runtime-de-modulos-no-processo.md): origem administrativa + hash; sem binário enviado pelo cliente |
| 9.7 | `client.call<Op>(...)` | ⬜ | |
| 9.8 | Migrações como Operations | ⬜ | |
| 9.9 | Documentar modelo de falhas (crash/supervisor/WAL recovery) | ⬜ | [ADR-012](decisions/ADR-012-runtime-de-modulos-no-processo.md): sem sandbox inicialmente; isolamento futuro preservado pelo contrato serializado |
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
| 10.1 | Plano completo e runner de benchmarks reproduzíveis | ⬜ | [PLANO_BENCHMARKS.md](PLANO_BENCHMARKS.md): todas as camadas; um JSONL timestampado e autocontido por campanha |
| 10.2 | Completar o BufferPool (LRU, pin/unpin, métricas) | ⬜ | |
| 10.3 | Profiling antes de cada otimização | ⬜ | |
| 10.4 | Fuzzing dos decoders | ⬜ | |
| 10.5 | Testar bancos maiores que o cache | ⬜ | |
| 10.6 | Política de compatibilidade (formato + protocolo) | ⬜ | |
| 10.7 | Estabilizar e documentar a API pública | ⬜ | |
| 10.8 | Reescrever `README.md`/formato de arquivo | ⬜ | |
| 10.9 | Guia de backup/restauração/diagnóstico | ⬜ | |

**Dívidas de performance herdadas da Fase 3** (nenhuma bloqueou o critério de
aceite; candidatas a medir na 10.1/10.3 antes de otimizar):

- `FieldBinder::store`/`load` usam `std::function` por campo
  ([binding.hpp](../include/modb/object/binding.hpp#L123)) — despacho indireto
  no caminho quente de materialização; um NTTP (`field<Id, &T::member>()`)
  viraria ponteiro de função cru.
- `ProjectionPlan::materialize` faz busca linear por `FieldId` em
  `object.fields` a cada passo
  ([projection_plan.cpp](../src/object/projection_plan.cpp#L58)) — o plano
  poderia pré-computar o índice de origem, já que a ordem de decodificação é
  determinística.
- `to_field_values`/materialização copiam cada `std::string` em ambas as
  direções ([binding.cpp](../src/object/binding.cpp#L26)); zero-copy exigiria
  um visitor direto sobre o objeto no encoder.
- `Handle::get<Member>()` materializa o objeto inteiro para ler um campo só
  ([database.hpp](../include/modb/object/database.hpp#L294)) e paga o mutex
  global do `DatabaseRegistry` a cada chamada.
- `Database::migration_for` aloca um `std::string` a cada `materialize` só
  para consultar o mapa de migrações
  ([database.cpp](../src/object/database.cpp#L95)), mesmo quando nenhuma
  migração está registrada — falta um fast-path `migrations_.empty()`.
- `Database::get<T>()` seguido de `materialize()` decodifica o mesmo objeto
  duas vezes (uma para validar existência/tipo, outra na materialização).

**Dívidas de performance/robustez herdadas da Fase 4:**

- Escrita de coleção é O(n): `PersistentVector::push_back` e as mutações de
  `Set`/`Map` reescrevem o blob inteiro
  ([collection.hpp](../include/modb/object/collection.hpp)). Append/patch
  incremental é candidato à 10.1 (com medição antes).
- `BlobStore` não tem free list: `remove`/`rewrite` (ao encolher) apenas zeram
  as páginas, que ficam órfãs no arquivo
  ([blob_store.cpp](../src/object/blob_store.cpp)). A recuperação de espaço
  entra com o BufferPool/gestão de espaço da Fase 10.
- `size()`/`at()`/`for_each` das coleções leem o blob inteiro para operar; um
  cursor por página (sem materializar a cadeia) casa melhor com o streaming da
  Fase 7.

### Testes/artefatos desta fase (não são CTest — ver protocolo)

| Item | Local | Status |
|---|---|---|
| `modb.buffer_pool` (teste) | `tests/buffer_pool_test.cpp` | ⬜ |
| Benchmarks (alvo separado, fora do ctest) | `benchmarks/` + [plano](PLANO_BENCHMARKS.md) | ⬜ |
| Alvos de fuzzing (preset `fuzz`) | `tests/fuzz/` | ⬜ |

Critério de aceite: ⬜ benchmarks reproduzíveis, regressões detectadas
automaticamente, interfaces públicas documentadas.

---

## Fase 11 — Container serverless

Status: ⬜ Não iniciada (0/11) — Definição completa:
[PLANO_ODB.md §Fase 11](PLANO_ODB.md#fase-11--container-serverless) ·
[PROTOCOLO_FASES.md §Fase 11](PROTOCOLO_FASES.md#fase-11--container-serverless)

| # | Tarefa | Status | Notas |
|---|---|---|---|
| 11.1 | ADR do modelo de implantação serverless (volume, writer único, cold start) | ⬜ | [ADR-013](decisions/ADR-013-execucao-serverless-em-container.md) |
| 11.2 | Imagem OCI multi-stage, mínima, não privilegiada, rootfs read-only | ⬜ | `deploy/Dockerfile` |
| 11.3 | Ingresso/protocolo da Fase 8 na plataforma escolhida | ⬜ | Sem expor formato físico; backpressure preservado |
| 11.4 | Configuração só por env/secrets | ⬜ | Sem dados nem credenciais na imagem |
| 11.5 | Volume persistente para banco + WAL com `fsync` confiável | ⬜ | Disco efêmero proibido como fonte de verdade |
| 11.6 | I/O assíncrono real: `io_uring` (Linux), IOCP (Windows) e fallback | ⬜ | Completion/`co_await`, cancelamento, fila limitada, ordering WAL explícito |
| 11.7 | Readiness, liveness, startup probe e desligamento gracioso | ⬜ | `SIGTERM` drena I/O/streams e sincroniza antes do prazo |
| 11.8 | Recovery em cold start e após término forçado | ⬜ | Inclui WAL pendente |
| 11.9 | Logs estruturados e métricas operacionais | ⬜ | Inclui backend de I/O, queue depth, completions e fallbacks |
| 11.10 | CI: build, SBOM, scan e publish versionado da imagem | ⬜ | |
| 11.11 | Guia de operação local e implantação de referência | ⬜ | `docs/OPERACAO_SERVERLESS.md` |

### Testes/artefatos desta fase

| Item | Local | Status |
|---|---|---|
| Build da imagem OCI | `deploy/Dockerfile` | ⬜ |
| Smoke container + volume | `tests/container_smoke_test` ou script CI | ⬜ |
| Contrato de I/O assíncrono real | `tests/async_file_test.cpp` em Linux/Windows | ⬜ |
| Kill -9 + reabertura no mesmo volume | prova de recovery | ⬜ |
| Manifesto de referência | `deploy/` | ⬜ |

Critério de aceite: ⬜ imagem sobe do zero com volume durável, recupera WAL,
atende cliente 8/9, preserva commits após término forçado, uma instância
ativa, sem privilégios, com backpressure. Em ambientes compatíveis, testes e
métricas comprovam completions reais por `io_uring`/IOCP; fallback explícito
mantém a mesma durabilidade.

---

## Histórico de fechamento de fases

| Fase | Data de conclusão | Commit |
|---|---|---|
| 0 | 2026-07-16 | `4928468` |
| 1 | 2026-07-16 | `bfcc5ef` |
| 2 | 2026-07-16 | `8d23923`…`cc6ee9b`…`85a5712` + remoção do Anel 1 |
| 3 | 2026-07-17 | `264213f` |
| 4 | 2026-07-17 | `f902a0b` |
| 5 | 2026-07-17 | `bc51f6e` |
