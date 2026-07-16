# Plano de desenvolvimento do ODB++ (moDb)

> Este plano substitui o `PLANO_DE_DESENVOLVIMENTO.md` relacional. O produto
> deixa de ser um banco relacional e passa a ser um banco **nativamente
> Orientado a Objetos**, conforme os três documentos de visão:
>
> - [arquitetura.md](../arquitetura.md) — modelo de objetos, catálogo, codec,
>   binding, projeção e evolução de schema;
> - [codigo-local.md](../codigo-local.md) — execução de código de domínio
>   dentro do servidor (Operations);
> - [streaming.md](../streaming.md) — streaming assíncrono como modelo nativo
>   de execução de consultas.

## 1. Objetivo

Construir um banco de dados orientado a objetos em C++, com identidade
permanente de objetos, catálogo versionado imutável, codec binário genérico,
evolução de schema sem migração imediata, transações com WAL, consultas em
streaming via coroutines e, no estágio final, um processo servidor que executa
operações de domínio registradas pela aplicação.

O plano preserva a filosofia do projeto: **entregas verticais** — cada fase
produz algo testável e observável de ponta a ponta, evitando construir
subsistemas isolados antes do primeiro resultado funcional.

## 2. O que se reaproveita do código atual

A fundação física construída até aqui permanece e vira a base do ObjectStore:

| Componente atual | Papel no ODB++ |
|---|---|
| `NativeFile` (I/O posicional + fsync) | inalterado — durabilidade real |
| `PageFile` (superbloco, alocação) | inalterado — o campo `catalog_root` já existe |
| `SlottedPage` (registros variáveis, generations) | inalterado — layout físico dos objetos |
| `TableHeap` (cadeia de páginas) | vira o heap físico de cada segmento de objetos |
| `endian.hpp`, `BinaryWriter/Reader` | base do codec genérico |
| `database_check` / `repair` | estendidos para o modelo OO |
| `Value`/`DataType` | evoluem para os valores de atributo do codec |

São aposentados (substituídos, não adaptados): `Catalog`, `Table`, `Schema`,
`Row` relacionais e os comandos relacionais da CLI. Os documentos relacionais
(`README.md`, `ESCOPO_MVP.md`, `FORMATO_DE_ARQUIVO.md`, plano antigo) serão
marcados como supersedidos e reescritos por fase.

## 3. Definição geral de pronto

Mantida do plano anterior. Uma tarefa só está concluída quando:

- o código compila sem warnings nos compiladores suportados;
- os testes relevantes foram escritos e estão passando;
- erros esperados retornam `Result`, sem encerrar o processo;
- não há comportamento indefinido conhecido;
- interfaces públicas e decisões não óbvias estão documentadas;
- o critério de aceite da fase foi demonstrado.

## 4. Visão das fases

| Fase | Tema | Entrega vertical | Depende de |
|---|---|---|---|
| 0 | Decisões e fundações | escopo do MVP OO, formatos, ADRs | — |
| 1 | Modelo de objetos e catálogo em memória | tipos e validação sem persistência | 0 |
| 2 | Codec genérico + ObjectStore persistente | objeto sobrevive a reabertura | 1 |
| 3 | Binding, Handle e ProjectionPlan | classe C++ materializada; evolução de schema | 2 |
| 4 | Relacionamentos, coleções e BlobStore | grafos de objetos e coleções grandes | 3 |
| 5 | Transações, WAL e recuperação | atomicidade e durabilidade comprovadas | 2 |
| 6 | Snapshots e MVCC | leituras consistentes durante escritas | 5 |
| 7 | Índices e consultas em streaming (embedded) | cursor O(1), coroutines, TTFR | 4, 6 |
| 8 | Servidor, protocolo binário e backpressure | streaming pela rede | 7 |
| 9 | Runtime de módulos de domínio | `client.call<TransferFunds>(...)` | 5, 8 |
| 10 | Desempenho e estabilização | benchmarks, buffer pool, fuzzing | todas |

O **MVP OO** compreende as fases 0 a 3. Critério, análogo ao MVP relacional:
criar um tipo `Employee`, persistir um objeto, fechar completamente a
instância, reabrir o arquivo e materializar exatamente o mesmo objeto — e
depois evoluir o schema (campo novo) e continuar lendo o objeto antigo.

## 5. Fases e tarefas

### Fase 0 — Decisões e fundações

Objetivo: remover ambiguidades que afetam o formato de arquivo e as interfaces
centrais, e registrar o pivô formalmente.

Tarefas:

- [x] Definir o escopo oficial do MVP OO (fases 0–3) e o que fica pós-MVP.
      ([ADR-007](decisions/ADR-007-limites-mvp-oo.md))
- [x] Definir a representação do `ObjectId` (largura, geração/sequência,
      relação com a localização física; a identidade nunca muda, o endereço
      físico sim). ([ADR-001](decisions/ADR-001-identidade.md))
- [x] Definir identificadores fortes: `ObjectId`, `TypeDefinitionId`,
      `FieldId`, `BlobId`, `BaselineId`, `DatabaseId`.
      ([ADR-001](decisions/ADR-001-identidade.md))
- [x] Definir os tipos de atributo primitivos do MVP (`bool`, `int64`,
      `double`, `string`, `bytes`, `ObjectId`, `BlobId`, `null`) e suas regras
      de conversão no ProjectionPlan (`Convert`).
      ([ADR-003](decisions/ADR-003-tipos-e-encoding.md))
- [x] Definir o layout do `ObjectHeader` (ObjectId, TypeDefinitionId) e do
      payload no formato binário.
      ([ADR-003](decisions/ADR-003-tipos-e-encoding.md) e Fase 2 do protocolo)
- [x] Definir a estratégia do mapa de identidade (ObjectId → localização
      física) e seu formato persistente.
      ([ADR-005](decisions/ADR-005-mapa-de-identidade.md))
- [x] Definir o problema de bootstrap do catálogo-como-objetos (meta-tipos
      primordiais fixos que descrevem TypeDefinition/AttributeDefinition).
      ([ADR-002](decisions/ADR-002-bootstrap-do-catalogo.md))
- [x] Decidir a política para o código relacional: aposentar `Catalog`/`Table`/
      `Schema`/`Row` quando a fase 2 entregar o caminho vertical OO.
      ([ADR-006](decisions/ADR-006-destino-do-codigo-relacional.md))
- [x] Marcar como supersedidos os documentos relacionais e atualizar o
      `GLOSSARIO.md` com os termos OO.
- [x] Registrar as decisões em `docs/decisions/`.

Entregáveis: documento de escopo do MVP OO; ADRs de identidade, formato de
objeto e bootstrap do catálogo; glossário atualizado.

Critério de aceite: duas pessoas leem as decisões e chegam à mesma
interpretação de identidade, formato e garantias do MVP OO.

### Fase 1 — Modelo de objetos e catálogo em memória

Objetivo: representar valores de atributo, definições de tipo e baselines sem
persistência.

Tarefas:

- [x] Implementar os identificadores fortes definidos na fase 0.
      ([ids.hpp](../include/modb/object/ids.hpp))
- [x] Evoluir `Value`/`DataType` para `AttributeValue` cobrindo os primitivos
      do MVP, incluindo `ObjectId` e `BlobId` como valores.
      ([attribute_value.hpp](../include/modb/object/attribute_value.hpp))
- [x] Implementar `AttributeDefinition` (FieldId, nome, tipo, nullable,
      default, collection, embedded).
      ([type_definition.hpp](../include/modb/object/type_definition.hpp))
- [x] Implementar `TypeDefinition` imutável com identidade própria.
      Relacionamentos (`Ref`/`OwnedRef`/`Embedded`) e coleções chegam como
      flags (`is_owned`/`is_embedded`/`is_collection`) nesta fase; a semântica
      completa é da Fase 4. Herança e constraints não fazem parte do MVP OO e
      ficam para uma fase futura, fora do escopo definido em
      [PROTOCOLO_FASES.md](PROTOCOLO_FASES.md).
- [x] Implementar `Baseline` imutável (snapshot estrutural completo).
      ([baseline.hpp](../include/modb/object/baseline.hpp))
- [x] Implementar `TypeRegistry` em memória (criar, localizar, versionar).
      ([type_registry.hpp](../include/modb/object/type_registry.hpp))
- [x] Validar payloads lógicos contra a `TypeDefinition` (contagem, tipos,
      nullability) antes de qualquer escrita. (`validate_object`)
- [x] Testes unitários de tipos, nulos, defaults, unicidade de FieldId e
      definições inválidas. ([object_model_test.cpp](../tests/object_model_test.cpp))

Entregáveis: API em memória para definir tipos e validar objetos lógicos;
suíte de testes do modelo.

Critério de aceite: ✅ uma `TypeDefinition` rejeita payloads incompatíveis e
`Baseline`/`TypeDefinition` são imutáveis após criação (nenhum mutador
público; evolução é feita por estampagem de um novo objeto, nunca por
mutação in-place).

### Fase 2 — Codec genérico e ObjectStore persistente

Objetivo: persistir objetos e o catálogo, e recuperá-los após reabertura — o
primeiro caminho vertical OO.

Tarefas:

- [x] Implementar o codec binário genérico: payload ↔ valores de atributo
      (o codec não conhece classes C++). ([object_codec](../src/object/object_codec.cpp))
- [x] Implementar `ObjectHeader` + payload sobre `SlottedPage`/`TableHeap`.
- [x] Implementar o mapa de identidade persistente (ObjectId → RecordId),
      com atualização quando um registro muda de página. (`IdentityMap`)
- [x] Implementar `ObjectStore`: create/get/update/remove por `ObjectId`.
- [x] Alocação de `ObjectId` monotônica e persistida (sem reuso no MVP).
- [x] Persistir o catálogo como objetos, usando o próprio codec e os
      meta-tipos primordiais da fase 0 (bootstrap). (`CatalogStore`)
- [x] Ligar a raiz do catálogo ao campo `catalog_root` do superbloco. (`DatabaseRoot`)
- [x] Reconstruir `TypeRegistry`/`Baseline` ao abrir o banco.
- [x] Estender `database_check` para reconhecer as páginas DBRT/IDMD/IDMP.
- [x] Aposentar o modelo relacional e adicionar comandos OO mínimos
      (`type define/list`, `object create/get/remove`). Removido o Anel 1
      (`Catalog`/`Table`/comando `catalog`/`catalog_test`); `Row`/`Value`/
      `Schema` e o codec relacional ficam como tooling de storage cru
      ([ADR-006, Atualização](decisions/ADR-006-destino-do-codigo-relacional.md)).
- [x] Teste de integração: centenas de objetos em múltiplas páginas,
      fechamento e reabertura. (500 objetos em `object_store_test`)

Entregáveis: objetos e catálogo persistentes; arquivo reabrível; CLI OO
mínima.

Critério de aceite: ✅ o teste cria um tipo e objetos, destrói a instância,
reabre o arquivo e recupera exatamente os mesmos objetos e definições
(`modb.object_store`, 500 objetos; e verificação end-to-end pela CLI entre
processos com `type`/`object`).

### Fase 3 — Binding, Handle e ProjectionPlan

Objetivo: ligar classes C++ reais ao formato persistente e provar a evolução
de schema sem migração imediata.

Tarefas:

- [ ] Implementar o `Binding` fluente
      (`db.bind<Employee>().field<1>(&Employee::name)...`), único por tipo na
      versão atual da aplicação.
- [ ] Implementar a materialização payload → objeto C++ (leitura) e
      objeto C++ → payload (escrita) via Binding + codec genérico.
- [ ] Implementar o `ProjectionPlan` com as operações `Copy`, `Convert`,
      `Default`, `Ignore` e `ResolveReference`.
- [ ] Implementar o cache de ProjectionPlans por
      (`TypeDefinitionId`, Binding atual).
- [ ] Implementar evolução de schema: registrar um Binding divergente cria uma
      nova `TypeDefinition`/`Baseline`; as antigas permanecem.
- [ ] Implementar migração preguiçosa: objeto antigo regravado passa a usar a
      definição atual.
- [ ] Implementar `registerMigration(...)` para mudanças semânticas.
- [ ] Implementar `Database`/`DatabaseRegistry` (`DatabaseId` → instância).
- [ ] Implementar `Handle<T>` (DatabaseId + ObjectId) com
      `get<&T::campo>()`/`set<&T::campo>(...)`.
- [ ] Testes de evolução: campo adicionado (default), removido (ignore),
      convertido (int↔double) e migração semântica registrada.

Entregáveis: API pública OO utilizável (`db.get<Employee>(id)`); evolução de
schema demonstrada.

Critério de aceite: **cenário do MVP OO completo** — aplicação v1 grava
`Employee{name,salary}`; aplicação v2 com campo `country` lê o objeto antigo
recebendo o default, sem qualquer migração manual.

### Fase 4 — Relacionamentos, coleções e BlobStore

Objetivo: dar semântica OO real a grafos de objetos e dados grandes.

Tarefas:

- [ ] Implementar `Ref<T>` (associação): persiste `ObjectId`, resolve para
      `Handle<T>` via `ResolveReference`.
- [ ] Implementar `Embedded<T>`: sem identidade, serializado no payload do
      pai.
- [ ] Implementar `OwnedRef<T>` (composição): remoção do pai remove o filho
      em cascata.
- [ ] Implementar o `BlobStore`: páginas de blob encadeadas, `BlobId`,
      escrita/leitura em streaming de binários maiores que uma página.
- [ ] Implementar `PersistentVector<T>` com armazenamento próprio via
      `BlobId` (o objeto pai guarda apenas a referência).
- [ ] Implementar `PersistentSet<T>` e `PersistentMap<K,V>`.
- [ ] Regras de integridade: remoção de objeto referenciado por `Ref` (política
      definida em ADR — proibir ou permitir referência pendente detectável).
- [ ] Estender `database_check` para blobs, coleções e cascatas.
- [ ] Testes: grafo com associações e composições, cascade delete, coleção
      com milhares de elementos em múltiplas páginas, blob multi-página.

Entregáveis: modelo de relacionamentos completo; coleções e blobs persistentes.

Critério de aceite: um grafo `Employee → Department`, `Employee ◆ Address` e
`Employee.projects: PersistentVector<Ref<Project>>` sobrevive à reabertura com
semântica preservada, incluindo remoção em cascata.

### Fase 5 — Transações, WAL e recuperação

Objetivo: atomicidade e durabilidade mesmo diante de interrupções. Toda
escrita passa a exigir transação.

Tarefas:

- [ ] Definir estados e API de `Transaction` (begin/commit/rollback).
- [ ] Exigir `Transaction&` em toda operação de escrita da API pública
      (`tx.create<T>()`, `handle.set(tx, ...)`, `tx.remove(...)`).
- [ ] Definir o formato do write-ahead log (WAL).
- [ ] Garantir WAL sincronizado (via `NativeFile::sync`) antes das páginas de
      dados.
- [ ] Implementar cache mínimo de páginas com rastreio de páginas sujas
      (embrião do BufferPool; a versão completa fica na fase 10).
- [ ] Implementar recuperação ao abrir o banco (redo/undo conforme o formato
      escolhido).
- [ ] Implementar rollback das operações suportadas.
- [ ] Definir estratégia de checkpoint.
- [ ] Exceção dentro de uma operação → rollback automático; término normal →
      commit (contrato que a fase 9 reutiliza).
- [ ] Testes de falha simulada em cada ponto crítico de escrita (kill entre
      WAL e página, entre páginas, durante checkpoint).
- [ ] Testar atomicidade, durabilidade e idempotência da recuperação.

Entregáveis: gerenciador de transações; WAL, recuperação e checkpoints;
relatório das garantias oferecidas.

Critério de aceite: após interrupção simulada, toda transação aparece completa
ou ausente — nunca parcialmente aplicada.

### Fase 6 — Snapshots e MVCC

Objetivo: leituras consistentes durante escritas — pré-requisito do streaming.

Tarefas:

- [ ] Definir o modelo de versão por objeto (ADR: escopo inicial
      single-writer / multi-reader com snapshots, antes de MVCC completo).
- [ ] Implementar `Snapshot` associado a transação/consulta.
- [ ] Leituras de um snapshot nunca enxergam escritas posteriores.
- [ ] Implementar retenção e descarte (GC) de versões antigas.
- [ ] Definir política de lock inicial para escritores.
- [ ] Testes: consulta longa lendo o mesmo estado lógico enquanto objetos são
      modificados e removidos em paralelo.

Entregáveis: snapshots consistentes; base para cursores de longa duração.

Critério de aceite: uma varredura iniciada antes de uma escrita produz
exatamente os objetos do snapshot, mesmo com commits concorrentes.

### Fase 7 — Índices e consultas em streaming (embedded)

Objetivo: consultas como fluxo de objetos, com memória O(1) e TTFR mínimo,
ainda dentro do processo (sem rede).

Tarefas:

- [ ] Implementar `IndexDefinition` no catálogo e índice B+ tree persistente
      sobre atributos.
- [ ] Implementar o cursor de varredura com estado mínimo (página atual, slot
      atual, estado do plano) — nunca materializar todos os resultados.
- [ ] Adotar C++20 coroutines (`Task<T>`/generator) como modelo de execução;
      validar suporte nos toolchains (MinGW GCC, MSVC, Clang).
- [ ] Implementar operadores naturalmente streaming: Scan, Index Scan,
      Predicate, Projection, Computed Functions, Limit.
- [ ] Implementar operadores bloqueantes: Sort sem índice, Aggregate global,
      Distinct global; e parcialmente bloqueantes: Top-K, Merge.
- [ ] Implementar o Planner ciente da natureza de cada operador (streaming /
      parcialmente bloqueante / bloqueante) e capaz de estimar TTFR e memória.
- [ ] Toda consulta abre um `Snapshot` (fase 6) e o mantém até o fim do fluxo.
- [ ] Implementar cancelamento cooperativo do cursor.
- [ ] API embedded: `query.stream()` consumível com `co_await stream.next()`.
- [ ] Benchmarks de TTFR e memória constante em coleções grandes.

Entregáveis: motor de consultas streaming embedded; índices persistentes;
métrica de TTFR estabelecida.

Critério de aceite: uma consulta sobre milhões de objetos entrega o primeiro
resultado sem materializar o conjunto, com memória O(1) comprovada por teste,
e buscas por chave usam o índice.

### Fase 8 — Servidor, protocolo binário e backpressure

Objetivo: levar o fluxo de objetos à rede, com o servidor como processo
dedicado a uma única aplicação.

Tarefas:

- [ ] ADR: biblioteca/abordagem de rede (ex.: asio standalone) e modelo de
      concorrência do servidor — o motor deixa de ser single-thread; revisar
      as premissas de escopo afetadas.
- [ ] Implementar o processo servidor hospedando `DatabaseRegistry`.
- [ ] Definir o protocolo binário de mensagens independentes:
      `Stream Begin` / `Object` / `Stream End` / `Stream Error`.
- [ ] Framing na camada de transporte (nunca lotes lógicos no banco).
- [ ] Serialização de objetos para a rede reutilizando o codec genérico.
- [ ] Backpressure fim-a-fim: socket lento suspende serializer → executor →
      storage (propagação natural via coroutines).
- [ ] Cliente C++ assíncrono: `co_await stream.next()`.
- [ ] Cancelamento pelo cliente e políticas de timeout/limite de recursos.
- [ ] Testes com cliente lento (backpressure), desconexão no meio do fluxo e
      erro após N objetos enviados (`Stream Error`).

Entregáveis: servidor + cliente streaming; protocolo documentado.

Critério de aceite: um cliente em outro processo consome um fluxo grande com
backpressure comprovado (servidor desacelera sem acumular memória) e recebe
`Stream Error` correto em falha no meio do fluxo.

### Fase 9 — Runtime de módulos de domínio

Objetivo: executar operações de domínio registradas dentro do servidor, em
transação, sem expor o armazenamento físico.

Tarefas:

- [ ] Implementar a interface `Operation` (`execute(ExecutionContext&)`) e
      `OperationResult`.
- [ ] Implementar `ExecutionContext` expondo somente `transaction()`,
      `objects()` e `logger()` — nunca páginas, buffer pool, WAL ou índices.
- [ ] Implementar o `OperationRegistry`
      (`registry.register<TransferFunds>("account.transfer")`).
- [ ] Despacho pelo protocolo: OperationId + argumentos serializados (nenhum
      código C++ trafega pela rede).
- [ ] Contrato transacional: término normal → commit; exceção → rollback
      (reutiliza a fase 5).
- [ ] Implementar `ModuleManifest` (version, baseline, api) e validação de
      compatibilidade na carga do módulo.
- [ ] Implementar `client.call<TransferFunds>(source, destination, amount)`.
- [ ] Migrações como Operations, reutilizando a mesma infraestrutura
      (`MigrationOperation` → ExecutionContext → Transaction → Projection).
- [ ] Documentar o modelo de falhas: crash do módulo encerra a instância;
      recuperação por supervisor externo (systemd/Kubernetes/Windows Service)
      + WAL recovery. Sem sandbox, por decisão registrada.
- [ ] Exemplo completo `TransferFunds` de ponta a ponta com teste de
      atomicidade (saldo insuficiente → rollback).

Entregáveis: runtime de operações; exemplo TransferFunds; guia de operação
sob supervisor.

Critério de aceite: `TransferFunds` executa atomicamente via
`client.call<...>`; uma exceção no módulo reverte tudo; a instância derrubada
por crash simulado volta consistente após restart + recovery.

### Fase 10 — Desempenho e estabilização

Objetivo: medir, otimizar e preparar interfaces estáveis.

Tarefas:

- [ ] Benchmarks reproduzíveis: TTFR, throughput de streaming, materialização
      (Binding + ProjectionPlan cacheado), codec e inserção.
- [ ] Completar o BufferPool (política de substituição, pin/unpin, métricas),
      evoluindo o cache mínimo da fase 5.
- [ ] Profiling antes de cada otimização relevante; garantir que o caminho
      crítico usa Binding + ProjectionPlan cacheado (sem interpretação
      dinâmica completa).
- [ ] Fuzzing dos decoders (codec genérico, ObjectHeader, catálogo, protocolo).
- [ ] Testar bancos maiores que a memória do cache.
- [ ] Política de compatibilidade do formato de arquivo e do protocolo.
- [ ] Estabilizar a API pública C++ e documentá-la.
- [ ] Reescrever `README.md` e documentação de formato para o modelo OO.
- [ ] Guia de backup, restauração, diagnóstico e operação.

Entregáveis: relatório de benchmarks; API e formato versionados; documentação
completa para usuários e contribuidores.

Critério de aceite: resultados reproduzíveis, regressões detectadas
automaticamente, interfaces públicas documentadas.

## 6. Itens deliberadamente fora deste plano

- múltiplas aplicações/tenants por instância (a instância é dedicada — decisão
  de arquitetura, não pendência);
- sandbox para código de domínio (o código C++ é confiável por decisão);
- replicação, alta disponibilidade e execução distribuída;
- otimizador baseado em custos com estatísticas sofisticadas;
- criptografia transparente do arquivo;
- compatibilidade com SQL ou com outro banco.

## 7. Estratégia de testes

Mantém a estratégia vigente e acrescenta os riscos novos:

- unitários para tipos, codec, ProjectionPlan, coleções e B+ tree;
- propriedades para round-trip do codec genérico e invariantes da árvore;
- fuzzing para decoders de arquivo e do protocolo de rede;
- integração para o caminho objeto → disco → reabertura → materialização;
- evolução de schema: matriz de casos v1→v2 (add/remove/convert/migração);
- falha simulada para WAL, commit, recovery e crash de módulo;
- streaming: TTFR, memória O(1), backpressure, cancelamento, Stream Error;
- sanitizers nos toolchains que os suportam (preset já configurado);
- benchmarks separados dos testes funcionais.

## 8. Regras arquiteturais

- O formato em disco não depende de padding, endianness ou ABI de C++;
  ponteiros nunca são persistidos.
- O formato persistente pertence ao banco, nunca ao layout das classes C++.
- Identidade (`ObjectId`) nunca muda; endereço físico pode mudar.
- `TypeDefinition` e `Baseline` são imutáveis; evolução cria novas definições.
- Existe um único codec binário genérico, dirigido pelo catálogo.
- Toda alteração persistente ocorre através da API transacional; código de
  domínio nunca acessa páginas, WAL, buffer pool ou índices.
- Dados vindos do arquivo ou da rede são sempre não confiáveis (validação
  defensiva); o código de domínio registrado é confiável.
- Streaming é o modelo nativo: nenhum componente materializa o conjunto de
  resultados; backpressure propaga até o storage.
- Operações de I/O retornam erros ricos (`Result`); não encerram o processo.

## 9. Ordem recomendada e racional

1. Fases 0–3 primeiro (MVP OO embedded): provam identidade, persistência,
   codec e evolução de schema — o coração diferencial do produto.
2. Fase 4 antes das transações, para que WAL/rollback já cubram blobs,
   coleções e cascatas.
3. Fases 5–6 antes do streaming, porque snapshot consistente é pré-requisito
   de cursor de longa duração.
4. Fases 8–9 por último: rede, concorrência e módulos multiplicam a
   complexidade; só entram sobre um núcleo com garantias comprovadas.

Não iniciar índices, streaming ou servidor antes de existir um teste confiável
de persistência, reabertura e recuperação. Cada fase preserva os testes e
garantias das anteriores.
