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
| 11 | Catálogo de facades e handles | `FacadeHandle` tipado sobre operações | 9, 10 |
| 12 | Handles de arestas e algoritmos de grafos | BFS/DFS e caminhos sobre relacionamentos tipados | 4, 6, 7, 10 |
| 13 | Container serverless | imagem OCI com escala a zero e dados duráveis | 8, 9, 10, 11, 12 |

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

- [x] Implementar o `Binding` fluente
      (`db.bind<Employee>().field<1>(&Employee::name)...`), único por tipo na
      versão atual da aplicação.
- [x] Implementar a materialização payload → objeto C++ (leitura) e
      objeto C++ → payload (escrita) via Binding + codec genérico.
- [x] Implementar o `ProjectionPlan` com as operações `Copy`, `Convert`,
      `Default`, `Ignore` e `ResolveReference`.
- [x] Implementar o cache de ProjectionPlans por
      (`TypeDefinitionId`, Binding atual).
- [x] Implementar evolução de schema: registrar um Binding divergente cria uma
      nova `TypeDefinition`/`Baseline`; as antigas permanecem.
- [x] Implementar migração preguiçosa: objeto antigo regravado passa a usar a
      definição atual.
- [x] Implementar `registerMigration(...)` para mudanças semânticas.
- [x] Implementar `Database`/`DatabaseRegistry` (`DatabaseId` → instância).
- [x] Implementar `Handle<T>` (DatabaseId + ObjectId) com
      `get<&T::campo>()`/`set<&T::campo>(...)`.
- [x] Testes de evolução: campo adicionado (default), removido (ignore),
      convertido (int↔double) e migração semântica registrada.

Entregáveis: API pública OO utilizável (`db.get<Employee>(id)`); evolução de
schema demonstrada.

Critério de aceite: **cenário do MVP OO completo** — aplicação v1 grava
`Employee{name,salary}`; aplicação v2 com campo `country` lê o objeto antigo
recebendo o default, sem qualquer migração manual.

Implementação validada com warnings como erros e com a configuração
`sanitizers`/hardening do MinGW: 36/36 testes passam, incluindo o cenário MVP.

### Fase 4 — Relacionamentos, coleções e BlobStore

Objetivo: dar semântica OO real a grafos de objetos e dados grandes.

Tarefas:

- [x] Implementar `Ref<T>` (associação): persiste `ObjectId`, resolve para
      `Handle<T>` via `ResolveReference`.
- [x] Implementar `Embedded<T>`: sem identidade, serializado no payload do
      pai.
- [x] Implementar `OwnedRef<T>` (composição): remoção do pai remove o filho
      em cascata.
- [x] Implementar o `BlobStore`: páginas de blob encadeadas, `BlobId`,
      escrita/leitura em streaming de binários maiores que uma página.
- [x] Implementar `PersistentVector<T>` com armazenamento próprio via
      `BlobId` (o objeto pai guarda apenas a referência).
- [x] Implementar `PersistentSet<T>` e `PersistentMap<K,V>`.
- [x] Regras de integridade: remoção de objeto referenciado por `Ref`
      ([ADR-008](decisions/ADR-008-integridade-de-referencias.md): referência
      pendente permitida e detectável na resolução).
- [x] Estender `database_check` para blobs (reconhecimento + validação
      estrutural de BLBP). Checagem semântica (cadeia de blob, refs órfãs,
      invariantes de catálogo) deferida a um verificador da camada de objetos.
- [x] Testes: grafo com associações e composições, cascade delete, coleção
      com milhares de elementos em múltiplas páginas, blob multi-página.

Entregáveis: modelo de relacionamentos completo; coleções e blobs persistentes.

Critério de aceite: um grafo `Employee → Department`, `Employee ◆ Address` e
`Employee.projects: PersistentVector<Ref<Project>>` sobrevive à reabertura com
semântica preservada, incluindo remoção em cascata.

### Fase 5 — Transações, WAL e recuperação

Objetivo: atomicidade e durabilidade mesmo diante de interrupções. Toda
escrita passa a exigir transação.

Tarefas:

- [x] Definir estados e API de `Transaction` (begin/commit/rollback).
- [x] Exigir `Transaction&` em toda operação de escrita da API pública
      (`create(tx, ...)`, `handle.set(tx, ...)`, `remove(tx, ...)`, coleções);
      `bind()` grava o catálogo sob transação interna.
- [x] Definir o formato do write-ahead log (WAL).
- [x] Garantir WAL sincronizado (via `NativeFile::sync`) antes das páginas de
      dados.
- [x] Implementar cache mínimo de páginas com rastreio de páginas sujas
      (buffer de transação no `PageFile`; BufferPool completo fica na fase 10).
- [x] Implementar recuperação ao abrir o banco (redo das transações commitadas,
      idempotente).
- [x] Implementar rollback das operações suportadas.
- [x] Definir estratégia de checkpoint (remoção do WAL após apply durável).
- [x] Exceção dentro de uma operação → rollback automático; término normal →
      commit (`Database::transact`, contrato que a fase 9 reutiliza).
- [x] Testes de falha simulada em cada ponto crítico de escrita (`FailpointFile`
      com I/O real + apply-failpoint; matriz em `modb.failpoint`).
- [x] Testar atomicidade, durabilidade e idempotência da recuperação
      (`modb.recovery`, `modb.wal`).

Entregáveis: gerenciador de transações; WAL, recuperação e checkpoints;
relatório das garantias oferecidas.

Critério de aceite: após interrupção simulada, toda transação aparece completa
ou ausente — nunca parcialmente aplicada.

### Fase 6 — Snapshots e MVCC

Objetivo: leituras consistentes durante escritas — pré-requisito do streaming.

Para reduzir o risco de uma mudança simultânea no formato persistente, nas
leituras e na concorrência, a fase é entregue em quatro incrementos completos:

#### Fase 6A — Épocas e formato versionado

- [x] Registrar o ADR-009: modelo single-writer / multi-reader por época,
      limite inicial de uma versão anterior, conflitos e comportamento após
      reabertura.
- [x] Persistir a época global no DBRT e implementar o IdentityMap v2 com
      localizações `current`/`previous` e migração do formato v1.

Entregável 6A: banco reabre e migra com segurança para o formato versionado,
sem alterar a semântica pública corrente.

Critério de aceite 6A: bancos v1 e v2 abrem com os mesmos objetos e a época
permanece monotônica após commit e reabertura.

#### Fase 6B — Snapshot e leituras consistentes

- [x] Implementar `Snapshot` RAII associado a transação/consulta e o registro
      em memória das épocas ativas.
- [x] Fazer `get` e `scan` selecionarem a versão visível na época do snapshot,
      cobrindo criação, atualização e remoção posteriores.
- [x] Implementar `snapshot_conflict` para impedir que uma segunda alteração
      sobrescreva a única versão anterior ainda visível.

Entregável 6B: API de snapshot com leituras pontuais e varreduras consistentes.

Critério de aceite 6B: um snapshot sempre devolve o estado lógico da época em
que foi criado, enquanto leituras sem snapshot devolvem o estado corrente.

#### Fase 6C — Retenção, GC e concorrência

- [x] Reter versões necessárias aos snapshots ativos e descartá-las quando a
      última época dependente for liberada.
- [x] Definir e implementar a política inicial de lock single-writer, incluindo
      a sincronização entre commit, abertura/fechamento de snapshot e GC.

Entregável 6C: ciclo de vida completo das versões, com uso de espaço limitado
e comportamento concorrente determinístico.

Critério de aceite 6C: nenhuma versão visível é descartada; versões obsoletas
são recuperadas e alterações incompatíveis falham sem corromper o estado.

#### Fase 6D — Integração e recuperação

- [x] Testar consulta longa lendo o mesmo estado lógico enquanto objetos são
      criados, modificados e removidos em paralelo.
- [x] Testar migração, commit, reabertura, recovery e limpeza de versões órfãs
      nos limites de falha relevantes.

Entregável 6D: matriz automatizada de integração e recuperação da Fase 6.

Critério de aceite 6D: a matriz passa sem leitura mista, versão perdida,
vazamento persistente ou corrupção após reabertura.

Entregáveis: snapshots consistentes; base para cursores de longa duração.

Critério de aceite da Fase 6: uma varredura iniciada antes de uma escrita produz
exatamente os objetos do snapshot, mesmo com commits concorrentes.

### Fase 7 — Índices e consultas em streaming (embedded)

Objetivo: consultas como fluxo de objetos, com memória O(1) e TTFR mínimo,
ainda dentro do processo (sem rede).

Para que cada incremento seja utilizável por quem consome a API, a fase é
dividida em cinco entregas verticais. `Generator`, cursores e B+ tree são
infraestrutura interna das entregas, não marcos isolados.

#### Fase 7A — Consulta streaming básica

- [x] Implementar `Generator<Result<T>>` com coroutines C++20 e cursor de
      varredura com estado mínimo; validar MinGW GCC, MSVC e Clang.
- [x] Implementar Scan, Predicate e Limit com avaliação preguiçosa e
      curto-circuito do upstream.
- [x] Fazer toda consulta manter um `Snapshot` até o fim e implementar
      cancelamento cooperativo.
- [x] Expor `query.stream()` para consumo incremental, sem materializar o
      conjunto de resultados.

Entregável 7A: consulta embedded por varredura, filtrável e cancelável.

Critério de aceite 7A: consulta com `limit 1` sobre 100 mil objetos entrega o
primeiro resultado lendo no máximo duas páginas de dados, mantém memória O(1)
e conserva o estado lógico do snapshot. A entrega depende da conclusão da
Fase 6D.

#### Fase 7B — Consultas indexadas

- [x] Implementar `IndexDefinition` no catálogo e B+ tree persistente com
      `insert`, `remove`, `find` e `range`.
- [x] Manter o índice atomicamente com as escritas de objetos e comprovar
      recuperação e reabertura.
- [x] Integrar Index Scan e buscas por igualdade/faixa à API streaming.

Entregável 7B: consultas por chave e intervalo sem varredura completa.

Critério de aceite 7B: buscas por chave e faixa usam comprovadamente o índice,
preservam duplicatas e permanecem corretas após commit, recovery e reabertura.

#### Fase 7C — Projeção e transformação

- [x] Implementar Projection com resultados tipados contendo somente os campos
      solicitados.
- [x] Implementar Computed Functions registradas e avaliadas durante o fluxo.

Entregável 7C: fluxo reduzido e transformado sem materializar objetos ou campos
desnecessários.

Critério de aceite 7C: projeções e funções computadas compõem com
Scan/Index Scan, Predicate e Limit mantendo avaliação preguiçosa e memória O(1).

#### Fase 7D — Ordenação e agregação

- [x] Implementar Sort global e Top-K, limitando Top-K a memória O(k).
- [x] Implementar Aggregate, Distinct e Merge, classificando e documentando
      operadores bloqueantes e parcialmente bloqueantes.

Entregável 7D: rankings, ordenação e consultas analíticas no pipeline embedded.

Critério de aceite 7D: resultados são corretos, Top-K comprova pico O(k) e todo
operador que materializa entrada é identificado como bloqueante.

#### Fase 7E — Planejamento automático e comprovação

- [x] Implementar Planner determinístico que escolha índice ou varredura e
      classifique a natureza do plano.
- [x] Implementar pushdown de Limit, seleção de Top-K e estimativas de TTFR e
      memória (`first_result_cost`).
- [x] Executar benchmarks reproduzíveis de TTFR, memória constante e ganho de
      índice em coleções grandes.

Entregável 7E: API de consulta que monta automaticamente o pipeline adequado,
com comportamento e desempenho observáveis.

Critério de aceite 7E: predicados elegíveis usam índice, Limit chega ao ponto
mais profundo seguro e os benchmarks comprovam TTFR e limites de memória.

Entregáveis: motor de consultas streaming embedded; índices persistentes;
métrica de TTFR estabelecida.

Critério de aceite da Fase 7: uma consulta sobre milhões de objetos entrega o primeiro
resultado sem materializar o conjunto, com memória O(1) comprovada por teste,
e buscas por chave usam o índice.

### Fase 8 — Servidor, protocolo binário e backpressure

Objetivo: levar o fluxo de objetos à rede, com o servidor como processo
dedicado a uma única aplicação.

Para que cada incremento seja utilizável e demonstrável, a fase é dividida
em seis entregas verticais. Sockets, codecs e filas são infraestrutura
interna das entregas, não marcos isolados. A fase depende da conclusão da
Fase 7.

#### Fase 8A — Contratos e codec do protocolo

- [x] Registrar a ADR-011 com o modelo de concorrência do servidor
      (leitor por conexão, workers de consulta, escritor dedicado) e
      revisar as premissas single-thread afetadas (`ScratchPagePool`,
      `DatabaseRegistry`, etc.).
- [x] Definir `QueryDescription` serializável e o codec das mensagens
      versionadas (`Hello`, `Query`, `StreamBegin`, `ObjectFrame`,
      `StreamEnd`, `StreamError`, `Cancel`), com `ObjectEnvelope` lógico
      (`ObjectId` + `TypeDefinitionId` + payload) e framing com diretório
      de slots; `compression=none` obrigatório nesta entrega.

Entregável 8A: protocolo binário documentado e testável sem rede.

Critério de aceite 8A: round-trip encode→decode de cada mensagem;
frames truncados, length mentiroso, >16 MiB, diretório inválido e lixo
são rejeitados com erro de protocolo, sem alocação gigante.

#### Fase 8B — Transporte e processo servidor

- [x] Implementar `NativeSocket` (Win32/POSIX, mesmo padrão do
      `NativeFile`) e o processo servidor hospedando `DatabaseRegistry`.
- [x] Abrir/registrar o banco, fazer bind em porta efêmera e negociar
      `Hello`/`HelloOk` (versão, baseline, codecs e limites).

Entregável 8B: servidor dedicado que aceita conexão e completa o
handshake.

Critério de aceite 8B: teste em loopback conecta, negocia e encerra
limpa; a CLI demonstra `modb serve` (ou equivalente) e um ping/info
remoto.

#### Fase 8C — Primeiro streaming remoto

- [x] Executar uma `QueryDescription` declarativa restrita no servidor e
      enviar `StreamBegin` → `ObjectFrame`(s) → `StreamEnd` (ou
      `StreamError` em falha do generator).
- [x] Implementar o cliente C++ e `ObjectStream` para consumo incremental
      do fluxo remoto, reutilizando o codec genérico; nunca expor
      `PageId`, `SlotId` ou `RecordId`.

Entregável 8C: consulta remota em streaming ponta a ponta.

Critério de aceite 8C: fluxo de 10 mil objetos com conteúdo íntegro e
ordem preservada; realocação física não altera os bytes lógicos do
objeto; falha injetada após N objetos entrega exatamente N +
`StreamError`.

#### Fase 8D — Backpressure e ciclo de recursos

- [x] Limitar frame, fila de saída (no máximo um frame / constante
      pequena de objetos em trânsito) e propagar bloqueio do socket até
      o generator e o storage.
- [x] Instrumentar produzidos − enviados, snapshots e cursores vivos;
      liberar recursos em desconexão abrupta.

Entregável 8D: backpressure fim-a-fim comprovado.

Critério de aceite 8D: com cliente lento (ex.: 1 obj/50 ms e janela TCP
pequena), produzidos − enviados permanece ≤ constante pequena e a
memória não cresce com o tamanho do fluxo; desconexão libera
cursor/snapshot.

#### Fase 8E — Cancelamento, multiplexação e API assíncrona

- [x] Receber `Cancel` enquanto o escritor envia; manter a conexão
      utilizável após o cancelamento.
- [x] Suportar múltiplos `query_id` na mesma conexão com escrita
      serializada e entregar `co_await stream.next()` com semântica e
      executor definidos na ADR-011.

Entregável 8E: cliente assíncrono com cancelamento e multiplexação.

Critério de aceite 8E: cancelamento interrompe a produção e permite nova
consulta na mesma conexão; duas consultas concorrentes intercalam
`query_id`s com ambos os fluxos íntegros.

#### Fase 8F — Limites, timeout, compressão e fechamento

- [x] Aplicar timeout, limite de streams concorrentes, frame máximo e
      razão de expansão; negociar compressão (codec escolhido por
      benchmark; `none` permanece obrigatório e fallback).
- [x] Fechar a suíte `modb.protocol` + `modb.server_streaming` e
      demonstrar consumo entre processos com a CLI.

Entregáveis: servidor + cliente streaming; protocolo documentado;
políticas de recurso ativas.

Critério de aceite 8F / da Fase 8: um cliente em outro processo consome
um fluxo grande com backpressure comprovado (servidor desacelera sem
acumular memória) e recebe `Stream Error` correto em falha no meio do
fluxo; compressão inválida ou não negociada é rejeitada sem alocação
excessiva.

### Fase 9 — Runtime de módulos de domínio

Objetivo: executar operações de domínio registradas dentro do servidor, em
transação, sem expor o armazenamento físico.

Tarefas:

- [x] Implementar a interface `Operation` (`execute(ExecutionContext&)`) e
      `OperationResult`.
- [x] Implementar `ExecutionContext` expondo somente `transaction()`,
      `objects()` e `logger()` — nunca páginas, buffer pool, WAL ou índices.
- [x] Implementar o `OperationRegistry`
      (`registry.register<TransferFunds>("account.transfer")`).
- [x] Despacho pelo protocolo: OperationId + argumentos serializados (nenhum
      código C++ trafega pela rede).
- [x] Contrato transacional: término normal → commit; exceção → rollback
      (reutiliza a fase 5).
- [x] Implementar `ModuleManifest` (version, baseline, api) e validação de
      compatibilidade na carga do módulo; incluir id, hash, métodos exportados
      e modo `read_only`/`read_write`; implementar `ModuleLoader` dentro do
      processo, restrito a uma origem confiável configurada pelo operador — o
      cliente nunca envia binários nem escolhe caminhos de carga
      ([ADR-012](decisions/ADR-012-runtime-de-modulos-no-processo.md)).
- [x] Implementar `client.call<TransferFunds>(source, destination, amount)`.
- [x] Migrações como Operations, reutilizando a mesma infraestrutura
      (`MigrationOperation` → ExecutionContext → Transaction → Projection).
- [x] Documentar o modelo de falhas: crash do módulo encerra a instância;
      recuperação por supervisor externo (systemd/Kubernetes/Windows Service)
      + WAL recovery. Sem sandbox no primeiro runtime, por decisão registrada.
- [x] Exemplo completo `TransferFunds` de ponta a ponta com teste de
      atomicidade (saldo insuficiente → rollback).

Entregáveis: runtime de operações e carregamento confiável no processo; exemplo
TransferFunds; guia de operação sob supervisor.

Critério de aceite: `TransferFunds` executa atomicamente via
`client.call<...>`; uma exceção no módulo reverte tudo; a instância derrubada
por crash simulado volta consistente após restart + recovery.

### Fase 10 — Desempenho e estabilização

Objetivo: medir, otimizar e preparar interfaces estáveis.

A fase é dividida em seis entregas verticais, cada uma com teste, demonstração
ou artefato reproduzível e tag própria.

#### Fase 10A — Runner e baseline de benchmarks

- [x] Implementar o [plano completo de benchmarks](PLANO_BENCHMARKS.md):
      runner, datasets determinísticos, perfis, coleta uniforme de todas as
      camadas e um JSONL autocontido por campanha, nomeado com data/hora UTC.
- [x] Registrar a baseline antes das otimizações, incluindo TTFR, throughput,
      p50/p95/p99, CPU, memória, I/O, espaço e metadados do ambiente.

Critério de aceite: duas campanhas com o mesmo seed são comparáveis e produzem
JSONL válido/autocontido; falha de correção invalida a campanha. Tag:
`0.0.10a`.

#### Fase 10B — BufferPool e bancos maiores que o cache

- [x] Completar o BufferPool (capacidade configurável, LRU, pin/unpin,
      write-back integrado ao WAL e métricas de hits/misses/evictions),
      evoluindo o cache mínimo da Fase 5.
- [x] Testar banco pelo menos 10× maior que o cache, incluindo pressão de
      leitura/escrita, eviction e recovery.

Critério de aceite: corretude e durabilidade preservadas sob eviction; pins não
são evictados; métricas fecham; `modb.buffer_pool` verde. Tag: `0.0.10b`.

#### Fase 10C — Profiling e otimizações medidas

- [x] Fazer profiling antes de cada otimização relevante e garantir que o
      caminho crítico usa Binding + ProjectionPlan cacheado, sem interpretação
      dinâmica completa.
- [x] Otimizar somente gargalos demonstrados; registrar comparação
      antes/depois e impedir regressão relevante nos demais perfis.

Critério de aceite: cada alteração de performance referencia evidência do
perfil e benchmark reproduzível; nenhum ganho é aceito só por hipótese. Tag:
`0.0.10c`.

#### Fase 10D — Robustez, fuzzing e entradas hostis

- [x] Implementar fuzzing dos decoders (codec genérico, ObjectHeader,
      TypeDefinition, catálogo, blobs, protocolo e WAL), com corpus mínimo e
      preset dedicado.
- [x] Rodar sanitizers e campanha mínima documentada, mantendo limites de
      alocação para entradas hostis.

Critério de aceite: alvos executam por 1 h sem crash/OOM/UB; corpus de regressão
fica versionado; suítes debug/sanitizers continuam verdes. Tag: `0.0.10d`.
Ver [FUZZING.md](FUZZING.md).

#### Fase 10E — Compatibilidade e API pública

- [x] Definir política de compatibilidade do formato de arquivo e do protocolo
      (major incompatível; minor somente aditivo).
- [x] Estabilizar e documentar a API pública C++, separando headers públicos de
      detalhes internos e cobrindo recusas de versão com erros claros.

Critério de aceite: matriz de compatibilidade automatizada cobre
arquivo/protocolo/API; exemplos públicos compilam como consumidores externos.
Tag: `0.0.10e`. Ver [COMPATIBILIDADE.md](COMPATIBILIDADE.md) e
[API_PUBLICA.md](API_PUBLICA.md).

#### Fase 10F — Documentação, operação e fechamento

- [x] Reescrever `README.md` e a documentação do formato para o modelo OO.
- [x] Publicar guia de backup, restauração, diagnóstico e operação sob
      supervisor.
- [x] Executar a matriz final de build/teste/benchmark e consolidar os
      resultados da Fase 10.

Critério de aceite: usuário novo percorre o exemplo OO, faz backup/restauração e
diagnóstico somente com a documentação; suíte inteira verde nos presets
suportados e baseline final registrada. Tag: `0.0.10f`.
Ver [OPERACAO.md](OPERACAO.md) e [FECHAMENTO_10F.md](FECHAMENTO_10F.md).

Entregáveis: runner e arquivos históricos de benchmark; API e formato
versionados; documentação completa para usuários e contribuidores.

Critério de aceite: resultados reproduzíveis, regressões detectadas
automaticamente, interfaces públicas documentadas.

### Fase 11 — Catálogo de facades e handles

Objetivo: agrupar as operações da Fase 9 em facades descobríveis e oferecer ao
consumidor um handle tipado que invoca métodos daquela facade, sem expor o
armazenamento físico nem substituir o `OperationRegistry`.

A Fase 9 permanece a unidade de execução (`Operation` + despacho + contrato
transacional). A Fase 11 acrescenta a superfície de API: catálogo heterogêneo
(`vector<FacadeDescriptor>`), descoberta/negociação de versão e
`FacadeHandle<TFacade>` com `invoke<Method>(args...)`.
([ADR-014](decisions/ADR-014-catalogo-de-facades-e-handles.md)).

Tarefas:

- [ ] Registrar em ADR o modelo de facades, handles, identidade estável
      (`FacadeId`) e a separação com o registry da Fase 9
      ([ADR-014](decisions/ADR-014-catalogo-de-facades-e-handles.md)).
- [ ] Definir `FacadeDescriptor` / `MethodDescriptor` e o catálogo em memória
      como `vector<FacadeDescriptor>` (posição no vetor nunca é identidade).
- [ ] Implementar `FacadeCatalog` (registro, listagem, lookup por `FacadeId`
      e versão).
- [ ] Implementar `FacadeHandle<TFacade>` no cliente: sessão + `FacadeId` +
      versão negociada; `invoke<Method>(args...)` tipado.
- [ ] Descoberta e negociação de versão/capacidades no protocolo (listar
      facades/métodos; obter handle somente se compatível).
- [ ] Validar que o método invocado pertence à facade do handle; erros
      `facade_not_found`, `facade_method_not_found`,
      `incompatible_facade_version`.
- [ ] Delegar a invocação ao `OperationRegistry` / `OpCall` da Fase 9,
      preservando commit/rollback, cancelamento e deadline.
- [ ] Expor facades a partir dos módulos carregados (manifesto → métodos
      agrupados), sem o cliente escolher caminhos ou enviar binários.
- [ ] Exemplo ponta a ponta: facade `Accounts` com `transfer` via handle;
      saldo insuficiente → rollback; método inexistente rejeitado.
- [ ] Documentar o contrato consumidor → handle → facade → registry e o
      modelo de evolução de versão de facade.

Entregáveis: catálogo e handles tipados; mensagens de descoberta/invocação;
exemplo `Accounts`/`transfer`; testes embedded e pela rede; ADR-014.

Critério de aceite: o consumidor obtém um `FacadeHandle` para uma facade
registrada, invoca um método tipado pela rede, a operação executa com o
mesmo contrato transacional da Fase 9; descoberta lista facades/métodos;
versão incompatível e método alheio à facade são rejeitados com ErrorCode
adequado.

### Fase 12 — Handles de arestas e algoritmos de grafos

Objetivo: tratar relacionamentos entre classes como arestas tipadas e oferecer
algoritmos básicos de grafos sob snapshot, reutilizando `Ref<T>`,
`OwnedRef<T>`, índices e streaming existentes.

`EdgeHandle<From, To, Kind>` é uma visão runtime da aresta: guarda
`DatabaseId`, origem, alvo e `FieldId`, mas não é persistido. `Ref<T>` e
`OwnedRef<T>` continuam sendo a representação no arquivo; `Embedded<T>` não é
vértice. ([ADR-015](decisions/ADR-015-handles-de-arestas-e-algoritmos-de-grafos.md)).

Tarefas:

- [ ] Registrar em ADR identidade, persistência, direção, ownership e política
      de referências órfãs para arestas
      ([ADR-015](decisions/ADR-015-handles-de-arestas-e-algoritmos-de-grafos.md)).
- [ ] Implementar `EdgeHandle<From, To, EdgeKind>` runtime-only, com origem,
      alvo, `FieldId` e resolução sob `Snapshot`.
- [ ] Implementar factories tipadas para campos escalares `Ref<T>` e
      `OwnedRef<T>`, rejeitando `Embedded<T>` e campos não relacionais.
- [ ] Expor adjacência para coleções `PersistentVector<Ref<T>>`, preservando
      tipo e semântica das arestas.
- [ ] Implementar BFS e DFS canceláveis/lazy, com limite de profundidade,
      máximo de vértices e política explícita para refs órfãs.
- [ ] Implementar caminho mínimo sem peso com reconstrução do caminho.
- [ ] Implementar detecção de ciclo e ordenação topológica (erro explícito
      para grafo cíclico).
- [ ] Implementar componentes conexos para uma view explicitamente não
      direcionada.
- [ ] Suportar arestas de entrada por índice de campo `Ref`; sem scan reverso
      ilimitado implícito.
- [ ] Cobrir snapshot único, reabertura, cancelamento, limites, órfãs,
      ownership e grafos heterogêneos nos testes.
- [ ] Estender a CLI com `graph bfs`, `dfs`, `shortest-path` e `toposort`,
      mantendo `graph demo` da Fase 4 como demonstração estrutural.
- [ ] Adicionar cenários de benchmark por largura, profundidade, densidade,
      cache cold/warm e pico do conjunto de visitados.

Entregáveis: `EdgeHandle` tipado; `GraphView`/provedor de adjacência; BFS,
DFS, caminho mínimo, ciclo, ordenação topológica e componentes; CLI, testes,
benchmarks e ADR-015.

Critério de aceite: após reabrir o banco, o consumidor cria handles de aresta
a partir de relacionamentos entre classes e executa BFS/DFS/caminho mínimo sob
um único snapshot; cancelamento, limites, refs órfãs, ciclos e direção são
determinísticos e cobertos; a CLI demonstra os algoritmos em grafo persistido.

### Fase 13 — Container serverless

Objetivo: empacotar e operar o servidor moDb em uma plataforma de containers
serverless, preservando as garantias de durabilidade e recuperação do banco.

O moDb continua stateful: o disco efêmero do container não é fonte de verdade
e a fase não introduz escala horizontal de escrita. A implantação inicial usa
um volume persistente compatível com escrita posicional e `fsync`, uma única
instância ativa por banco e escala a zero quando o serviço está ocioso.

Tarefas:

- [ ] Registrar em ADR o modelo de implantação, incluindo volume persistente,
      instância escritora única, cold start, escala a zero e plataformas
      serverless suportadas.
- [ ] Criar imagem OCI reproduzível em build multi-stage, mínima, executada por
      usuário não privilegiado e com filesystem raiz somente leitura.
- [ ] Adaptar o protocolo da fase 8 ao ingresso suportado pela plataforma
      escolhida, sem expor o formato físico nem enfraquecer backpressure,
      cancelamento ou limites de recursos.
- [ ] Configurar banco, porta, credenciais e limites somente por variáveis de
      ambiente e secrets; nunca incluir segredos ou dados na imagem.
- [ ] Montar os arquivos do banco e WAL em armazenamento persistente com as
      garantias de locking, flush e atomicidade exigidas pelo motor.
- [ ] Implementar I/O assíncrono real sob uma abstração única: `io_uring` no
      Linux e IOCP no Windows, com leitura, escrita, flush, cancelamento,
      limites de operações em voo e fallback síncrono detectado em runtime
      para kernels ou volumes incompatíveis. A ordem WAL → flush → páginas
      deve depender de completions/barreiras explícitas, nunca apenas da ordem
      de submissão.
- [ ] Implementar readiness, liveness, startup probe e desligamento gracioso,
      bloqueando novas operações e concluindo ou revertendo transações antes do
      prazo de término da plataforma.
- [ ] Comprovar recovery no cold start e após término forçado do container,
      incluindo banco com WAL pendente.
- [ ] Adicionar logs estruturados e métricas de cold start, recovery, conexões,
      transações, memória, I/O e duração das requisições.
- [ ] Automatizar build, SBOM, scan de vulnerabilidades e publicação versionada
      da imagem OCI.
- [ ] Documentar execução local e implantação de referência, incluindo
      restrições de plataforma, backup, restauração e custo operacional.

Entregáveis: imagem OCI publicada; manifesto de implantação serverless de
referência; camada de I/O assíncrono real; guia operacional; testes de
container, cold start e recovery.

Critério de aceite: a imagem sobe a partir de zero, monta armazenamento
durável, recupera o banco quando necessário e atende um cliente das fases
8/9/11/12; após término completo e nova inicialização, os objetos commitados
permanecem e transações incompletas não aparecem. A validação deve comprovar
uso de uma única instância ativa, memória limitada, backpressure e execução
sem privilégios. Nos ambientes compatíveis, métricas e testes devem comprovar
uso efetivo de `io_uring`/IOCP sem bloquear o executor; o fallback deve ser
explícito e preservar a mesma correção e durabilidade.

## 6. Itens deliberadamente fora deste plano

- herança e polimorfismo persistentes (tipos-base, discriminadores, consultas
  polimórficas, materialização dinâmica e `dynamic_cast`);
- constraints declarativas no metamodelo;
- persistência de ponteiros C++ crus ou smart pointers (`unique_ptr` e
  `shared_ptr`); referências e composição usam `Ref<T>` e `OwnedRef<T>`, sem
  posse compartilhada ou composição cíclica;
- mapeamento automático de construções C++ não cobertas pelo conjunto de
  atributos do codec, como `std::optional`, `std::variant`, enums,
  `std::chrono`, tuples e arrays;
- persistência direta de contêineres STL como membros; coleções persistentes
  usam `PersistentVector`, `PersistentSet` e `PersistentMap`;
- materialização por construtores ou fábricas para classes sem construtor
  padrão, e binding por getters/setters ou membros privados;
- persistência transparente, detecção automática de alterações e lazy loading
  por proxies que se comportem como objetos ou ponteiros C++ comuns;
- relacionamentos bidirecionais declarativos, sincronização automática dos
  dois lados e cardinalidades inversas;
- múltiplos bindings simultâneos para o mesmo tipo C++;
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
- facades/handles: descoberta, negociação de versão, invoke tipado e
  rejeição de método alheio à facade;
- grafos: handles de aresta, BFS/DFS, caminho mínimo, ciclo, direção,
  cancelamento, limites e snapshots;
- container serverless: build da imagem, cold start, término gracioso,
  persistência em volume e recovery após término forçado;
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
4. Fases 8–9 introduzem rede, concorrência e módulos somente sobre um núcleo
   com garantias comprovadas.
5. A Fase 10 estabiliza o produto; a Fase 11 organiza a superfície de
   domínio em facades/handles; a Fase 12 acrescenta algoritmos de grafos
   sobre relacionamentos; a Fase 13 adiciona os riscos operacionais de
   container, cold start e armazenamento remoto persistente.

Não iniciar índices, streaming ou servidor antes de existir um teste confiável
de persistência, reabertura e recuperação. Cada fase preserva os testes e
garantias das anteriores.
