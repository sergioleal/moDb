# Plano — servidor de banco com stored procedures compiladas

Criado em 2026-09-27. Origem: a aplicação `biblioteca` embute o moDb no próprio
processo. A decisão agora é outra:

- **o banco é um processo servidor** (`<app>-server`), dono dos arquivos;
- **as regras de negócio rodam dentro dele como stored procedures** em C++,
  **compiladas junto com o servidor** (um executável de servidor por aplicação);
- **a aplicação é só cliente**: fala com o servidor pela rede, nunca abre o
  arquivo do banco, e chama procedures pelo nome.

```
┌──────── biblioteca-server (processo do banco) ────────┐
│ engine moDb + procs da biblioteca (C++, compiladas)   │
│   biblioteca.emprestar, biblioteca.devolver, ...      │
└────────────────▲───────────────────────────────────────┘
                 │ protocolo do moDb (ADR-010): OpCall / OpResult
┌────────────────┴──────── biblioteca-web ──────────────┐
│ HTTP + tela; chama procs com modb::app_client          │
└────────────────────────────────────────────────────────┘
```

**Isolamento:** a aplicação fica isolada do banco (outro processo, outra máquina
se quiser). As procs rodam no processo do banco, **sem sandbox**: um erro
comum (exceção, erro de regra) desfaz só a transação da chamada; um crash de
verdade (acesso inválido de memória) derruba o servidor, que o supervisor
reinicia, e o WAL garante que nenhuma transação confirmada se perde. Isso foi
escolhido de propósito: sem IPC entre a proc e o motor.

## O que já existe (levantamento de 2026-09-27)

| Peça | Estado |
|---|---|
| `net::Server` | biblioteca: `listen(path, host, port)` + `serve_forever()`, uma thread por sessão, um banco por servidor; todo acesso ao motor sob `engine_mutex_` |
| Protocolo | `Hello`/`HelloOk`, `Query`/`ObjectFrame`/`StreamEnd`/`StreamError`/`Cancel`, **`OpCall`/`OpResult`**, `FacadeList`/`FacadeOpen` |
| Procs | `ops::Operation` (classe C++, `execute(ExecutionContext&)`), `OperationRegistry::dispatch` faz **begin → execute → commit**, rollback em erro ou exceção; modo `read_only`/`read_write` |
| Módulos | `ModuleLoader` + `ModuleManifest` + allowlist por hash (ADR-012), **estáticos** (linkados) — exatamente o modelo escolhido |
| Cliente | `modb::app_client` → `ServerConnection::call(op_id, bytes)`, `query`, `open_facade<T>()`; só C++ |
| Executável | só `modb serve <arquivo>`, que **não registra procs** (`OpCall` → `operation_not_found`) |

O que falta:

- um **executável de servidor com procs** e uma forma simples de montá-lo;
- **argumentos e resultados com formato**: hoje são bytes crus, e cada operação decodifica os seus;
- uma **API de dados completa para as procs**: `ObjectAccess` só tem `create`/`get`/`read`/`update`/`remove`, sem consulta, índice ou coleção;
- **erros de regra** com código (inválido / não encontrado / conflito);
- **operação**: arquivo de configuração, log, serviço do sistema.

## Desenho

1. **`modb::server_host`**: biblioteca nova que monta um servidor completo a partir de uma lista de módulos (argumentos/configuração, abertura do banco, bind dos tipos, registro das procs, sinais, log, `READY <porta>`). O servidor de uma aplicação é um `main` de poucas linhas, gerado por uma função CMake `modb_add_server(biblioteca-server MODULES biblioteca_procs)`.
2. **Módulo de aplicação**: tipos (bindings) + procs, registrados por uma função `registrar(Modulo&)`. Proc declarada como função, sem a cerimônia de classe + fábrica:
   ```cpp
   modulo.proc("biblioteca.emprestar", Modo::escrita,
       [](Contexto& c, const Args& a) -> Resultado {
           auto ex = c.ler<Exemplar>(a.id("exemplar"));
           ...
           return Valor::objeto({{"id", novo.id()}, ...});
       });
   ```
3. **Valores autodescritos** para argumentos e resultados: nulo, bool, inteiro, double, texto, id, lista, mapa. É uma codificação binária própria (tag + valor, como o codec de objetos do ADR-003), com conversão para JSON e de JSON nos dois lados. Os clientes não precisam conhecer `TypeDefinitionId` nem `FieldId`.
4. **Erros de regra**: `falha::invalido / nao_encontrado / conflito` → `OpResult{ok=false, code, message}`; o cliente recebe o código e decide (a web traduz em 400/404/409).
5. **Descoberta**: a lista de procs (nome, modo, descrição, argumentos) sai por `FacadeList` ou por uma proc de sistema (`sys.procs`), para ferramentas e gateways.

## Tarefas

### P0 — servidor com procs utilizável

#### S1 — `modb::server_host` e `modb_add_server` *(médio)* ✅

- [x] 1.1 Biblioteca `modb_server_host`: `run(argc, argv, modulos)` com `--db`, `--host`, `--porta`, `--config ARQ`, sinais (SIGINT/SIGTERM → `request_stop`), log em stderr, `READY <porta>`
- [x] 1.2 Função CMake `modb_add_server(<alvo> MODULES <libs>)` que gera o `main`
- [x] 1.3 `examples/server_procs/`: servidor mínimo com um módulo de exemplo e um cliente que chama uma proc; teste de ponta a ponta (servidor em processo filho, cliente real)
- [x] 1.4 ADR "Servidor de aplicação com procedures compiladas" (este desenho, isolamento sem sandbox, um banco por servidor)

#### S2 — Valores autodescritos para argumentos e resultados *(médio)* ✅

- [x] 2.1 *(`ops::Value`, `include/modb/ops/value.hpp`)* `ops::Valor` (nulo, bool, int64, double, texto, id, lista, mapa) + codificação binária versionada + testes de ida e volta
- [x] 2.2 *(`to_json`/`from_json`; id vira número e `Args::id` aceita inteiro ≥ 1)* Conversão `Valor` ↔ JSON (para gateways web e para o CLI)
- [x] 2.3 *(`ops::Args`: `id`, `text`, `integer`, `boolean`, `ids`, `*_or`)* `Args` com leitura tipada e erro claro (`a.id("exemplar")`, `a.texto_ou("nome", "")`)

#### S3 — Procs declaradas como função, com erros de regra *(médio)* ✅

- [x] 3.1 *(`modb::server::ModuleBuilder::proc`, `include/modb/server/module.hpp`; a fábrica do registro virou `std::function`)* `Modulo::proc(nome, modo, descricao, fn)` sobre o `OperationRegistry` existente (sem mudar o protocolo)
- [x] 3.2 *(`invalid`/`not_found`/`conflict`; `ErrorCode::conflict` e `internal_error` novos, no fim do enum — compatível no fio)* Códigos de erro de regra (`invalido`, `nao_encontrado`, `conflito`, `interno`) no `OpResult`; exceção vira `interno` com rollback
- [x] 3.3 `modb call <host> <porta> <proc> '<json>'` no CLI, para testar procs à mão

#### S4 — API de dados das procs *(médio)* ✅

- [x] 4.1 *(`modb::server::Context`: `read`, `where`/`all`, `find` pelo índice, `create`/`update`/`set<&T::campo>`/`remove`, `blobs()`+`transaction()` para coleções, `today()`, `log()`)* `Contexto` expõe, dentro da transação da chamada: `ler/criar/atualizar/set/remover`, `consulta<T>()` (com `where`/`equals`/`between`), `por_indice<T>(campo, valor)`, coleções (`PersistentVector`/`BlobStore`) e o relógio do servidor
- [x] 4.2 *(já era assim no `OperationRegistry::dispatch`; escrita numa proc de leitura dá `transaction_required`)* Procs `read_only` rodam sob snapshot, sem abrir transação de escrita
- [x] 4.3 *(`ModuleBuilder::type(binding)` e `::index<T>(campo)`, aplicados no `prepare`)* Tipos e índices declarados pelo módulo (`modulo.tipo(binding)`, `modulo.indice<T>(campo)`) e aplicados na abertura pelo `server_host`

### P1 — pronto para operar

#### S5 — Operação *(médio)* ✅

- [x] 5.1 *(`--config ARQUIVO`, `chave = valor`; flags valem mais; caminhos relativos à pasta do arquivo; `max_streams`, `idle_timeout_ms`, `proc_timeout_ms`, `log`)* Arquivo de configuração (banco, host, porta, limites de conexões e de tempo de proc)
- [x] 5.2 *(`OperationRegistry::set_call_observer`; uma linha por chamada: instante UTC, proc, modo, ms, `ok`/`error <código> <mensagem>`; stderr, arquivo ou `off`)* Log estruturado por chamada (proc, duração, resultado, código de erro)
- [x] 5.3 *(`docs/OPERACAO.md` § Servidor de aplicação; `examples/server_procs/deploy/notas-server.{service,conf}`; NSSM no Windows; o teste já mata o processo à força e reabre — agora pelo arquivo de configuração)* Rodar como serviço: unidade systemd e Windows Service (ou NSSM) documentados em `docs/OPERACAO.md`; teste de reinício depois de matar o processo (o WAL recupera)
- [x] 5.4 *(`OperationRegistry::set_time_limit`; prazo no `ExecutionContext`; `Context::in_time()` antes de cada acesso; passou do prazo → `ErrorCode::operation_timeout` e rollback, mesmo que a proc não tenha notado)* Tempo máximo por proc (cooperativo: o `Contexto` checa o prazo a cada acesso ao banco) e desfaz a transação ao estourar
- [x] 5.5 *(sessões registradas em `net::Server`; `request_stop` faz `NativeSocket::shutdown` nelas — no Windows também `CancelIoEx`, porque `shutdown` não acorda um `recv` bloqueado; parada com cliente ocioso: 30 s → imediata)* Desligamento ativo: `request_stop` fecha as sessões abertas; hoje `serve_forever` espera cada cliente ocioso até o idle timeout (30 s) — achado na S2

#### S6 — Descoberta de procs *(pequeno)* ✅

- [x] 6.1 *(módulo de sistema `sys` carregado pelo `server_host` em todo servidor; `[{name, mode, module, description}]`; argumentos por convenção na descrição — não há esquema formal)* `sys.procs` devolve nome, modo, descrição e argumentos esperados de cada proc
- [x] 6.2 *(agrupado por módulo)* `modb procs <host> <porta>` no CLI

### P2 — biblioteca no novo modelo

#### S7 — `biblioteca-server`: módulo de procs *(médio)* ✅

- [x] 7.1 *(repositório `biblioteca`, commit 18cf90c; `tests/` à parte; saíram `src/biblioteca.*`, `api.*`, `json.*`)* Reorganizar o repositório `biblioteca`: `modulo/` (modelo + procs), `servidor/` (o `main` via `modb_add_server`), `web/` (cliente)
- [x] 7.2 *(`modulo/biblioteca_procs.cpp`: 31 procs; as regras numa classe `Acervo` sobre o `Context`; cada escrita numa transação — antes a validação e a escrita eram passos separados; `exemplo.carregar` numa transação só; relógio do servidor, ou `BIBLIOTECA_HOJE` para testes)* Portar as regras de `src/biblioteca.cpp` para procs: `autores.listar/obter/criar/atualizar/remover` (e o mesmo para editoras, livros, exemplares e leitores), `emprestimos.listar/emprestar/devolver`, `resumo`, `exemplo.carregar`
- [x] 7.3 *(`tests/procs_test.cpp`: 60 verificações; o "passar 20 dias" mata o servidor e o sobe com outra data, o que também confere a recuperação pelo WAL e os índices depois de reabrir)* Testes das procs contra o servidor de verdade (processo filho, banco em arquivo temporário): as 55 verificações de hoje, agora pela rede

#### S8 — `biblioteca-web`: só cliente *(pequeno)* ✅

- [x] 8.1 *(`web/gateway.cpp`: tabela rota → proc, JSON ↔ `Value` pelo `ops::from_json/to_json`; o frontend não mudou. A web linka só `modb::app_client` — que hoje depende da biblioteca `modb` inteira; o processo não abre banco)* A web deixa de linkar o motor: só `modb::app_client` + cpp-httplib; cada rota HTTP vira uma chamada de proc (`Valor` ↔ JSON da S2.2), códigos de erro → 400/404/409
- [x] 8.2 *(`--servidor HOST:PORTA`; leitura repetida numa conexão nova se o servidor reiniciou; escrita nunca repetida, e só reaproveita conexão usada há menos de 1 s; conexão perto do idle timeout do servidor é trocada antes de usar)* Configuração do endereço do servidor; reconexão se o servidor reiniciar
- [x] 8.3 *(`tests/web_test.cpp`: 20 verificações, com idle timeout curto no servidor, queda (503) e volta)* Teste: web + servidor em processos separados, derrubar o servidor no meio e ver a web se recuperar

### P3 — depois

#### S9 — Concorrência *(ver PLANO_CONCORRENCIA.md)*

- [ ] 9.1 Hoje o servidor serializa tudo (`engine_mutex_`), igual ao mutex da biblioteca: correto, mas uma proc por vez. Procs `read_only` concorrentes dependem das C6–C8; as de escrita seguem uma por vez (C10.2: fila de escritores)

#### S10 — Clientes em outras linguagens, por um protocolo rápido *(médio)*

Revisto a pedido: em vez de um gateway HTTP/JSON, o protocolo mais performático
possível, na linha do RDMA (ADR-026).

- [x] 10.1 *(ADR-026: gRPC, Cap'n Proto, MessagePack-RPC, Arrow Flight, HTTP e RDMA avaliados)* Decisão: o protocolo nativo publicado para qualquer linguagem, mais um transporte por memória compartilhada na mesma máquina
- [x] 10.2 *(`docs/PROTOCOLO_CLIENTES.md`)* Especificação byte a byte: frame, `Hello`, `OpCall`/`OpResult`, `Value` v1, anel
- [x] 10.3 *(`ShmAttach`/`ShmAttachOk`, protocolo minor 1; `net::shm::Region`/`Ring`/`Backoff`; `Client::attach_shared_memory`)* Anel de memória compartilhada: dois anéis SPSC por cliente, frames do TCP no lugar, polling adaptativo, TCP como linha de vida
- [x] 10.4 *(`clients/python/modb_client.py`, `modb.python_client`: 35 verificações, TCP e anel entre processos)* Cliente de referência em Python, só biblioteca padrão
- [x] 10.5 *(`modb_rpc_bench`: CSV com ops/s e p50/p99/p99,9 por transporte e tamanho)* Medidor TCP × anel
- [ ] 10.6 Medir na máquina dedicada (sem máquina hoje). Predição: o anel tira as 4 trocas de thread e as 4 syscalls de cada chamada; payload pequeno deve ficar uma ordem de grandeza abaixo do TCP em latência
- [ ] 10.7 RDMA de verdade (verbs, `rdma-core`) sobre o mesmo layout de anel — condicional a uma máquina com placa RDMA; antes, teste funcional com Soft-RoCE num Linux
- [ ] 10.8 *(opcional)* Gateway HTTP/JSON genérico, para navegador e `curl` (a `biblioteca-web` já é um, específico)

#### S11 — Fora do escopo (registrar no ADR da S1.4)

- vários bancos por servidor; sandbox de procs; procs carregadas em tempo de execução (`.dll`/`.so`); multi-writer.

## Ordem sugerida

S1 → S2 → S3 → S4 → S7 → S8 → S5 → S6 → S9 → S10.

A biblioteca (S7/S8) entra logo depois da API de procs porque ela é o teste real
do desenho; a operação (S5) vem antes de usar o servidor fora da máquina local.

## Registro de execução

| Data | Tarefa | Commit | Resultado |
|---|---|---|---|
| 2026-09-27 | Plano | — | Levantamento do servidor, do protocolo e dos módulos por leitura de código |
| 2026-09-27 | S1 | (este commit) | `modb::server_host` + `modb_add_server`; exemplo `notas-server`; teste ponta a ponta com o servidor em outro processo, morto à força e reaberto (nota confirmada sobrevive). ADR-025. Achado: o move de `net::Server` perdia o registro de procs e os limites — corrigido |
| 2026-09-27 | S2 | (este commit) | `ops::Value` (binário versionado com limites contra entrada hostil, JSON), `ops::Args`; `notas` passa a usar `{texto}`/`{id}`. Achado: parar o servidor com cliente ocioso conectado leva até 30 s → S5.5 |
| 2026-09-27 | — | — | Mecanismos de segurança (autenticação, tokens, TLS) retirados do plano a pedido; tarefas renumeradas (S6→S5 … S12→S11) |
| 2026-09-27 | S3–S4 (P0 concluído) | (este commit) | `ModuleBuilder` (procs como função, tipos, índices), `Context` (leitura, consulta, índice, escrita, `set`, coleções), erros `conflict`/`internal_error`, exceção vira `internal_error` com rollback, `modb call`. `notas` reescrito: 6 procs, testadas pela rede e pelo CLI |
| 2026-09-27 | S5–S6 (P1 concluído) | (este commit) | Arquivo de configuração, log por chamada, tempo limite cooperativo com rollback (`operation_timeout`), parada ativa (no Windows `shutdown` não acorda `recv` bloqueado: `CancelIoEx`), `sys.procs` + `modb procs`, operação como serviço documentada. `modb.server_host`: 31 s → 2,4 s |
| 2026-09-27 | S7–S8 (P2 concluído) | biblioteca 18cf90c | Biblioteca em dois processos: `biblioteca-server` (31 procs) e `biblioteca-web` (gateway HTTP → procs). Regras testadas pela rede (60) e web + servidor com queda e volta do servidor (20). Achado: `modb::app_client` puxa a biblioteca `modb` inteira; separar o cliente do motor fica para quando houver cliente fora do repositório |
| 2026-09-27 | S10 (10.1–10.5) | (este commit) | Protocolo nativo publicado + anel de memória compartilhada (ADR-026), cliente Python de referência, `modb_rpc_bench`. Achados: o `Hello`/`HelloOk` sem `minor` decodificava com o minor do próprio build (virou 1 com esta mudança; o teste de compatibilidade pegou): peer antigo agora é minor 0. No desktop (não vale como número), janelas curtas de espera derrubavam payloads de 64 KiB de ~29 mil para ~3 mil chamadas/s: espera ajustada para girar 100 µs e ceder a CPU até 5 ms antes de dormir |
