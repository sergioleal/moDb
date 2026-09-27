# Plano — leitores concorrentes com um escritor

Criado em 2026-09-27. Origem: a aplicação `biblioteca` (servidor HTTP com várias
threads) precisa serializar todo acesso ao moDb com um mutex, porque o motor não
é seguro entre threads. O [ADR-011](../docs/decisions/ADR-011-concorrencia-do-servidor.md)
já previa "uma escrita de cada vez; leituras sob snapshot podem ser concorrentes",
mas isso nunca foi implementado: o servidor de rede também serializa tudo
(`Server::engine_mutex_`).

**Objetivo:** várias threads lendo ao mesmo tempo, cada uma sob um snapshot
(explícito ou implícito), enquanto **uma** transação de escrita roda, sem que o
leitor veja dado não confirmado e sem que a aplicação precise de lock próprio.

**Fora do escopo** (decisão a registrar no ADR da C9): várias transações de
escrita simultâneas (multi-writer). Exige gerenciador de locks, detecção de
conflito e um WAL com vários escritores. O modelo de um escritor continua, e o
ganho de escrita concorrente vem depois, se vier, por group commit (C14).

## Diagnóstico (levantamento de 2026-09-27)

O caminho de **leitura** escreve em estado compartilhado sem lock:

| Onde | O que a leitura faz |
|---|---|
| `BufferPool::get` | reordena a lista LRU (`touch`) e soma métricas em todo acerto; num miss, `put` despeja frames e reescreve frames residentes no lugar |
| `PageFile::view` | devolve ponteiro cru para um frame do cache, um nó de `tx_pages_` ou o buffer único `view_scratch_`; vale "até a próxima chamada a este PageFile". Nenhum frame é pinado (`pin` existe, mas nada o chama) |
| `ObjectStore::peek_type` | grava o cache de uma entrada `peeked_` |
| `Database::materialize_decoded` | cria sob demanda planos de projeção em `BoundType::plans` (`mutable unordered_map`) |
| `TableHeap::read_page_records` | `++data_pages_read_` (contador comum) |
| `ScratchPagePool` | vetor sem sincronização (hoje só no caminho de validação) |

E o **escritor** expõe o que ainda não confirmou:

- **Páginas da transação:** `PageFile::write` guarda a página em `tx_pages_` e também a espelha como suja no `BufferPool`. `read`/`view` servem essas páginas a **qualquer** chamador, e um `get` sem snapshot de outra thread vê dados não confirmados. Um leitor com snapshot filtra pelas épocas, mas lê os bytes das mesmas páginas que o escritor está alterando no lugar.
- **Época adiantada:** `commit_transaction` avança a época **antes** do WAL e da aplicação das páginas. Um snapshot aberto nesse intervalo recebe `época+1` e enxerga a transação ainda não durável; se o commit falhar, a época volta e é reusada.
- **Rollback reconstrói o store:** `resync_store_after_rollback` faz `store_ = ObjectStore::open(...)`. Isso destrói identidade, heap, catálogo, registro de tipos e índices por baixo de leitores e de geradores suspensos (`scan_stream` segue a cadeia de páginas viva entre um `yield` e outro).
- **Estado em memória mudado no lugar:** contadores e índices do `TableHeap`, `root_` das B+ trees, `registry_`, `indexes_`, `bound_` e as épocas.
- **Índices não versionados:** um update tira a chave antiga do índice. Uma consulta por índice sob snapshot perde o objeto que o snapshot ainda deveria ver com a chave antiga. É um defeito de correção, **mesmo com uma thread só**.
- **Alocação imediata:** `allocate_page` (e os splits da B+ tree) gravam direto no disco, fora do conjunto da transação.

**Consequência:** já hoje o servidor de rede fica exposto aos defeitos de época e
de índice, porque solta o `engine_mutex_` a cada frame e deixa escritas
intercalarem com streams suspensos.

## Desenho escolhido (a detalhar no ADR da C9)

**Isolamento lógico pelo MVCC que já existe e atomicidade física na aplicação do
commit** — não MVCC de páginas:

1. **Escrita privada.** O que a transação escreve fica só no conjunto dela (`tx_pages_`), visível apenas à própria transação. Nada é espelhado no cache compartilhado antes do commit.
2. **Aplicação atômica.** No commit, depois do `fsync` do WAL, as páginas entram no cache e no arquivo sob um latch **exclusivo** curto. Leitores seguram o latch **compartilhado** só durante cada leitura de página, nunca entre dois `yield` de um stream.
3. **Época publicada por último.** A época nova só fica visível depois da aplicação. Snapshot novo = última época publicada. Um `get` sem snapshot vira um snapshot implícito da época publicada.
4. **Visão de leitura imutável.** O estado em memória que o leitor usa (raízes, contadores, catálogo, planos) passa a ser uma visão imutável trocada atomicamente no commit (`shared_ptr` publicado). O escritor muda a própria cópia.
5. **Rollback sem reconstrução.** Com a escrita privada, abortar é descartar o conjunto da transação e a cópia do escritor. Nada que um leitor usa muda.
6. **Cache seguro entre threads.** `BufferPool` com lock (depois particionado, se medir contenção), e `view` devolvendo uma referência pinada (RAII) em vez de ponteiro cru.

Por que não MVCC de páginas (copy-on-write): o MVCC por objeto (`current`/`previous`
com épocas, ADR-009) já dá o isolamento lógico. Faltava só a atomicidade física e
a publicação na ordem certa, o que custa muito menos. MVCC de páginas fica como
alternativa se a C12 medir contenção no latch de aplicação.

## Regras

As mesmas do plano de desempenho: predição escrita antes de medir, teste que falha
antes da correção (e mutação para provar o teste), um processo por repetição,
**medições só em máquina dedicada** (hoje sem nenhuma: a conta da DO está sem
planos dedicados). Para concorrência, também:

- **ThreadSanitizer** (`-fsanitize=thread`) no Linux (WSL ou máquina dedicada): o MinGW não tem TSan. Toda tarefa da C4 em diante passa limpa no TSan;
- **teste de estresse** (C5) rodando N leitores e 1 escritor com invariantes checadas, antes e depois de cada tarefa.

## Tarefas

### P0 — defeitos de correção que já existem (valem com uma thread só)

#### C1 — Snapshot aberto durante um commit vê a transação não durável *(pequeno)* ✅

- [x] 1.1 *(`tests/epoch_publication_test.cpp`: commit parado em `stop_after_images` e em `stop_after_commit_record`; falhava em 8 verificações — o snapshot via o objeto criado e a versão nova do alterado)* Teste que falha hoje: abrir um snapshot entre `advance_epoch` e o fim do commit (failpoint de `CommitPhase` ou gancho de teste) e confirmar que ele vê o objeto da transação; o mesmo com o commit falhando depois
- [x] 1.2 *(`Database::published_epoch_`, publicado logo depois de `apply_transaction`; `snapshot()` e `epoch()` leem a publicada; o rollback que relê o store republica a do disco)* Correção: calcular a época nova no começo do commit, mas só publicá-la (`DatabaseRoot::epoch_`) depois da aplicação; `snapshot()` lê a época publicada
- [x] 1.3 *(os carimbos já eram a época da transação; agora têm nome: `ObjectStore::transaction_epoch()`)* Revisar os carimbos `root_.epoch()+1` (`object_store.cpp:305, 467, 505`) para usar a época da transação, não "a publicada mais um"

#### C2 — Consulta por índice sob snapshot perde objetos cuja chave mudou *(médio)* ✅ (medição pendente)

- [x] 2.1 *(`tests/index_snapshot_test.cpp`: chave mudada, faixa, objeto removido e ordem da faixa; falhava em 4)* Teste que falha hoje: índice em `User.name`, abrir snapshot, mudar o nome de um objeto, consultar pelo nome antigo através do snapshot → hoje não acha
- [x] 2.2 *(escolhida a varredura revalidada: `ObjectStore::index_range_at` usa a B+ tree quando nenhuma chave do índice foi retirada depois da época do snapshot, e senão varre a época e ordena como o índice (valor, id). Só snapshots mais velhos que a última retirada pagam a varredura; consultas novas seguem pelo índice. A decisão entra no ADR da C9)* Decidir no ADR (C9): manter a chave antiga no índice até o GC (entrada com época de remoção) ou, sob snapshot mais antigo que a última escrita no índice, cair para varredura revalidada
- [x] 2.3 Implementar *(feito)* e medir *(pendente: máquina dedicada)* o custo em `range_scan_sweep` (máquina dedicada)

#### C3 — Rollback reconstrói o `store_` por baixo de quem lê *(pequeno, contenção)* ✅ (sem defeito com uma thread)

- [x] 3.1 *(`tests/rollback_readers_test.cpp`: snapshot e stream antigos atravessando um rollback. **Passa sem correção**: a releitura do store refaz as versões `previous` a partir do disco, e o stream só guarda ids de página e cópias entre um `yield` e outro. Fica como teste de regressão)* Teste que falha hoje: stream suspenso (`scan_stream`) + transação que faz rollback + retomar o stream
- [x] 3.2 *(desnecessário: com uma thread não há defeito, e no servidor o rollback e cada passo de um stream correm sob o `engine_mutex_`. O risco que sobra é só concorrente — a releitura mexe no store enquanto outra thread lê — e é o que a C8 resolve)* Paliativo até a C8: `rollback` recusa rodar (ou espera) com geradores vivos, e o servidor passa a segurar o `engine_mutex_` também durante o `send` quando houver escrita pendente
- [ ] 3.3 Remover o paliativo quando a C8 tornar o rollback local ao escritor

### P1 — fundações: o caminho de leitura seguro entre threads

#### C4 — Infra de teste: TSan no WSL e um teste de fumaça multithread *(pequeno)*

- [ ] 4.1 Preset `tsan` (Linux/WSL: `-fsanitize=thread`, RelWithDebInfo) e roteiro em `docs/` para rodá-lo
- [ ] 4.2 Teste `concurrency_smoke`: 8 threads fazendo `get`/`query` num banco só de leitura. Deve **falhar no TSan hoje** (LRU, `peeked_`, planos): é a linha de base

#### C5 — Teste de estresse com invariantes *(médio)*

- [ ] 5.1 N leitores (snapshot e sem snapshot) + 1 escritor fazendo create/update/delete/rollback, com duração configurável e semente
- [ ] 5.2 Invariantes: cada snapshot vê um estado que existiu num commit (contagem e checksum iguais aos gravados pelo escritor na época), nenhum leitor vê transação abortada, nenhum crash ou erro de página
- [ ] 5.3 Entra no `ctest` numa versão curta; a versão longa entra como caso do `modb_load` (`concurrent_readers`), para a máquina dedicada

#### C6 — Caches e contadores do caminho de leitura *(médio)*

- [ ] 6.1 `BufferPool`: um mutex para `get`/`put`/`touch`/`evict` (primeira versão, simples) e métricas atômicas
- [ ] 6.2 `PageFile::view` devolve `PageRef` (RAII) que pina o frame até sair de escopo; `view_scratch_` deixa de existir (um miss aloca ou usa um frame pinado); revisar os chamadores (`table_heap.cpp`, `identity_map.cpp`)
- [ ] 6.3 `peeked_`: tirar do objeto compartilhado (por chamada, ou por thread) — medir se o ganho da T15 se mantém
- [ ] 6.4 `BoundType::plans`: construir o plano no `bind` ou proteger a criação sob demanda com mutex; `data_pages_read_` atômico; `ScratchPagePool` com mutex
- [ ] 6.5 Critério: `concurrency_smoke` (C4.2) limpo no TSan com leitores só; custo em thread única no ruído (máquina dedicada)

### P2 — isolamento: leitores e escritor ao mesmo tempo

#### C7 — Escrita privada e aplicação atômica no commit *(grande)*

- [ ] 7.1 `tx_pages_` visível só à transação dona (token da `Transaction`/thread); `write` deixa de espelhar no cache compartilhado
- [ ] 7.2 Latch de aplicação (`std::shared_mutex`): leitura de página em modo compartilhado, `apply_transaction` em exclusivo; os streams não o seguram entre `yield`s
- [ ] 7.3 `allocate_page` e os splits da B+ tree passam a ser da transação (hoje gravam direto no disco); páginas alocadas e abortadas voltam para a lista livre
- [ ] 7.4 Época publicada depois da aplicação (fecha a C1 de vez); `get` sem snapshot = snapshot implícito da época publicada
- [ ] 7.5 Predição e antes/depois do custo do commit e das leituras (máquina dedicada)

#### C8 — Visão de leitura imutável e rollback local *(grande)*

- [ ] 8.1 Separar o estado do `ObjectStore` em "visão publicada" (raízes, contadores, catálogo, registro de tipos, índices: imutável, trocada por `shared_ptr` atômico no commit) e "estado do escritor" (cópia que a transação muda)
- [ ] 8.2 `scan_stream` e demais geradores seguram a visão com que começaram (e o snapshot), nunca a cadeia viva
- [ ] 8.3 Rollback = descartar o conjunto da transação e a cópia do escritor; `resync_store_after_rollback` deixa de existir (remove o paliativo da C3)
- [ ] 8.4 DDL (`bind`, `create_index`, `register_migration`, `register_computed`) em modo exclusivo: espera os leitores terminarem ou recusa com erro claro

#### C9 — ADR de concorrência *(pequeno, junto com a C7)*

- [ ] 9.1 ADR: modelo (N leitores + 1 escritor), escolhas das C2/C7/C8, por que não MVCC de páginas, multi-writer fora do escopo, contrato de thread-safety da API
- [ ] 9.2 Atualizar ADR-011 (a tabela de componentes), `GARANTIAS_TRANSACIONAIS.md` §9 e o README

### P3 — API e adoção

#### C10 — `Database` seguro entre threads, com um contrato claro *(médio)*

- [ ] 10.1 Documentar (e checar em debug) quais chamadas podem ser concorrentes: leituras e snapshots de qualquer thread; uma transação por vez (a segunda espera ou falha — decidir); DDL exclusiva
- [ ] 10.2 `begin()` com espera opcional (`begin(timeout)`) em vez de só `transaction_active`, para aplicações com várias threads escritoras enfileirarem
- [ ] 10.3 `Handle`/`Snapshot` seguros para usar da thread que os criou; documentar que não são compartilháveis entre threads (ou torná-los)

#### C11 — Servidor de rede e réplicas sem lock global *(médio)*

- [ ] 11.1 `Server`: tirar o `engine_mutex_` das consultas; manter a serialização só para `OpCall` de escrita (ou usar a fila da C10.2)
- [ ] 11.2 Réplica (ADR-016 §6): aplicar a transação replicada com o mesmo latch de aplicação da C7

#### C12 — Medir *(máquina dedicada)*

- [ ] 12.1 Caso `concurrent_readers` no `modb_load`: 1, 2, 4, 8 leitores com e sem um escritor; ops/s de leitura agregada, latência de cauda do escritor
- [ ] 12.2 Predição: leitura escala até o número de núcleos enquanto o conjunto cabe no cache; o escritor perde no máximo o tempo das aplicações
- [ ] 12.3 Se o mutex do `BufferPool` for o gargalo: particionar (por `page_id`) e medir de novo

#### C13 — Biblioteca sem mutex próprio *(pequeno, depois da C10)*

- [ ] 13.1 Tirar o `std::mutex` do serviço da `biblioteca` para as leituras; manter a ordem nas escritas pela fila da C10.2
- [ ] 13.2 Teste de carga HTTP simples (N clientes lendo + 1 emprestando/devolvendo) com as invariantes do domínio (um exemplar nunca em dois empréstimos abertos)

### P4 — depois

#### C14 — Reavaliar group commit (T8) *(médio, condicional)*

- [ ] 14.1 Com a aplicação separada do `fsync`, medir se vários escritores enfileirados (C10.2) ganham com um `fsync` para vários commits; decidir com números (condição de reabertura da T8)

## Ordem sugerida

C1 → C3 (paliativo) → C4 → C5 → C6 → C2 → C9 → C7 → C8 → C10 → C11 → C13 → C12 → C14.

A C2 fica depois da C6 porque a correção do índice precisa do teste de estresse
para ser validada sob concorrência. A C12 depende de máquina dedicada; enquanto
não houver uma, os critérios de correção (TSan e estresse) seguem valendo, e os
números de desempenho ficam pendentes, marcados como tal.

## Registro de execução

| Data | Tarefa | Commit | Resultado |
|---|---|---|---|
| 2026-09-27 | Plano | — | Diagnóstico por leitura de código; nenhum teste ainda |
| 2026-09-27 | C1 | (este commit) | Época publicada separada da do DBRT; teste falhava em 8 verificações |
| 2026-09-27 | C2 | (este commit) | Varredura revalidada quando o índice perdeu chaves que o snapshot vê; teste falhava em 4. Custo a medir |
| 2026-09-27 | C3 | (este commit) | Hipótese não confirmada com uma thread (o teste passa antes da correção); vira teste de regressão. Risco concorrente fica para a C8 |
| 2026-09-27 | C4 | — | **Bloqueado:** o WSL (Ubuntu 24.04) tem GCC 13 (não testado com o moDb; o build cai para C++23 sem C++26) mas não tem cmake nem ninja, e instalar pacotes pede a senha de sudo. Sem TSan, as tarefas C4 em diante não têm como cumprir o critério "limpo no TSan" |
