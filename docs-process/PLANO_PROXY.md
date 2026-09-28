# Plano — acesso remoto por proxies (engine só local)

Criado em 2026-09-28. Decisão: [ADR-028](../docs/decisions/ADR-028-proxy-de-acesso-remoto.md).

O acesso remoto sai do processo do banco para **proxies** que falam com o
engine por um **link local multiplexado**. O objetivo é poder ter vários
proxies com implementações diferentes, inclusive com segurança na frente do
banco.

Escolhas (2026-09-28):

- o proxy **decodifica tudo** (pode filtrar e reescrever mensagens);
- as sessões dos clientes são **multiplexadas** no link, com crédito por stream;
- o engine só aceita TCP direto com `--listen-tcp`, e só durante a transição.

## Regras

- Cada tarefa deixa todos os testes existentes passando. O modo TCP direto
  continua funcionando até a P4.
- Medições de latência e vazão rodam só na máquina dedicada (droplet), nunca
  no desktop. O relatório dá ops/s por fase e compara TCP direto com o
  caminho via proxy.
- O TSan (preset `tsan`, WSL) roda nas tarefas que mexem em threads do engine
  ou do proxy.

## Tarefas

### P0 — sessão independente do socket (refatoração, sem mudar o comportamento)

#### X1 — `EngineSession` *(médio)* ✅

- [x] 1.1 *(`src/net/engine_session.{hpp,cpp}`: `handle_urgent` para o Cancel, `handle` para o resto, `finish` idempotente)* Extrair de `Server::handle_connection` uma `EngineSession`: estado da
  sessão (tokens, workers de consulta, shm), alimentada por uma fila de
  entrada e com uma saída abstrata (`SessionSink::send(Message)`).
- [x] 1.2 *(a fila e a thread leitora ficam no transporte, `DirectConnection` em `server.cpp`)* `SocketSink`: a saída de hoje (socket + `send_mu`). O modo TCP direto
  passa a ser uma `EngineSession` sobre um `SocketSink`.
- [x] 1.3 *(`SessionSink::send_stream_frame` e `stream_closed`; o socket usa o `send` comum)* Backpressure através do sink: `send` pode bloquear (socket) ou
  esperar crédito (link, X4). O worker de consulta não sabe qual dos dois é.
- [x] 1.4 *(149/149 no debug; TSan limpo nos 10 testes de servidor e concorrência)* Critério: `server_streaming`, `operation_server`, `facade_server`,
  `app_server_connection` e o stress de concorrência passam sem mudança; TSan limpo.

#### X2 — Endpoint local *(pequeno)* ✅

- [x] 2.1 *(mais `remove_local`; no Windows, AF_UNIX sob `%LOCALAPPDATA%` dá `WSAEINVAL` no `connect` nesta máquina, então os testes usam o diretório do build)* `NativeSocket::listen_local(path)` / `connect_local(path)`: AF_UNIX
  no Linux e no Windows (`afunix.h`), removendo um socket velho no caminho, com
  permissão `0600` no POSIX.
- [x] 2.2 *(`tests/local_socket_test.cpp`, também no TSan)* Teste: eco por AF_UNIX nas duas plataformas; `request_stop` acorda o
  `accept` local como acorda o TCP.

### P1 — link multiplexado

#### X3 — Codec do link *(médio)* ✅

- [x] 3.1 *(ordem trocada: `| session u32 | length u32 | type u8 | payload |`, e o que vem depois do `session` é o frame do cliente como hoje; controle do link com tipos ≥ 0x80; `SessionOpenError` virou `SessionOpenOk{ok = false}`)* `net/link_protocol.hpp`: frame `| length u32 | session u32 | type u8 | payload |`;
  mensagens de controle `LinkHello`, `LinkHelloOk` (versão do link, baseline,
  nome do banco, limites do engine), `SessionOpen` (principal, roles,
  atributos), `SessionOpenOk`/`SessionOpenError`, `SessionClose`, `StreamCredit`.
- [x] 3.2 As mensagens de sessão reusam o codec do protocolo do cliente
  (`encode_message`/`decode_message` do payload), sem compressão.
- [x] 3.3 *(`tests/link_protocol_test.cpp`)* Testes de codec: ida e volta, entradas hostis, `session` desconhecida,
  frame grande demais.

#### X4 — Engine atende o link *(grande)* ✅

- [x] 4.1 *(modo do `Server`: `Server::open` + `listen_tcp`/`listen_local`; `serve_forever` atende os dois; `src/net/engine_link.cpp`)* `Engine` (ou um modo do `Server`): aceita links no endpoint local;
  por link, uma thread leitora que despacha por `session` para a
  `EngineSession` certa; a escrita no link é serializada por link.
- [x] 4.2 `LinkSink`: envia com o `session` no frame; `ObjectFrame` só com
  crédito do stream (`StreamCredit`); um stream sem crédito espera sem
  bloquear as outras sessões do link.
- [x] 4.3 *(pool por link, `set_link_workers`; cada sessão é uma fila que um worker drena até 16 mensagens por vez)* OpCalls de sessões diferentes não se bloqueiam: execução num pool de
  workers do engine (o modo TCP de hoje executa na thread da sessão); por
  sessão, as respostas continuam na ordem das chamadas.
- [x] 4.4 *(e o engine manda `SessionClose` quando encerra uma sessão, ou quando chega mensagem para sessão desconhecida)* `SessionClose` e queda do link: cancela streams, junta workers,
  descarta as sessões do link.
- [x] 4.5 *(`ops::Caller` em `ExecutionContext::caller()` e `server::Context::caller()`; o log de chamadas ganha `by <principal>`)* Principal da sessão no `ExecutionContext` das procs (só leitura).
- [x] 4.6 *(`Server::set_link_secret`; erro novo `unauthenticated`, e `permission_denied` para a X7)* Segredo compartilhado opcional no `LinkHello` (arquivo com o segredo,
  comparado em tempo constante).
- [x] 4.7 *(`tests/engine_link_test.cpp`, também no TSan)* Testes: duas sessões num link com um stream lento e um rápido (o
  rápido não espera o lento); Cancel via link; queda do link no meio de um
  stream; limite de streams por sessão.

### P2 — proxy

#### X5 — `modb::proxy` (biblioteca) e `modb-proxy` passagem *(grande)* ✅

- [x] 5.1 *(`include/modb/proxy/proxy.hpp`, `src/proxy/proxy.cpp`; `net::negotiate_hello` é o mesmo do servidor direto; por cliente, uma leitora e uma escritora, e a thread do link só empilha na fila do cliente)* Laço do proxy: aceita clientes (TCP), faz `Hello`/`HelloOk` com cada
  um (codec, limites, idle timeout), abre a sessão no link e repassa nos dois
  sentidos, decodificando e recodificando.
- [x] 5.2 *(`Options::stream_credit`, padrão 8; um crédito volta ao engine a cada frame entregue)* Crédito: o proxy dá crédito inicial por stream e devolve à medida que
  entrega frames ao cliente; um cliente lento para só o seu stream.
- [x] 5.3 *(`include/modb/proxy/policy.hpp`: `authenticate`, `authorize` com reescrita no lugar, `on_response`, `audit`; `PassThroughPolicy`)* Interface de política: `authenticate(hello, credenciais)` →
  `Principal`, `authorize(principal, Message&)` → permite / recusa (com código) /
  reescreve, `audit(principal, pedido, resposta)`. A política de passagem
  aceita tudo.
- [x] 5.4 *(sem link o proxy recusa clientes novos; `reconnect_max_ms`)* Reconexão do link: se o engine cair, as sessões dos clientes recebem
  erro e fecham; o proxy tenta reabrir o link com espera crescente.
- [x] 5.5 *(`apps/modb_proxy/main.cpp`; `--secret-file`; e `modb serve --local SOCKET [--secret-file F]` como engine de teste, com TCP só se `--port` vier)* Executável `modb-proxy --engine unix:CAMINHO --listen HOST:PORTA
  [--policy ...] [--config ARQUIVO]`, com `READY <porta>` como o `server_host`.
- [x] 5.6 *(em vez de duplicar as quatro suítes: `tests/proxy_test.cpp` cobre pelo proxy o que elas cobrem (handshake, stream com RLE, várias streams, Cancel, OpCall, facades, erros do engine) e o que só existe aqui (política, auditoria, cliente parado com fila limitada no engine, queda e volta do engine); também no TSan)* Critério: a suíte de servidor (`server_streaming`, `operation_server`,
  `facade_server`, `app_server_connection`) roda também através do proxy (os
  testes ganham o modo "via proxy"), incluindo backpressure (8D) e Cancel (8E).

#### X6 — Autenticação no protocolo do cliente *(médio)* ✅

- [x] 6.1 *(tipos 17/18; os mecanismos vão depois do `minor` no `HelloOk` e só quando há algum; antes de autenticar, o proxy responde cada pedido com `unauthenticated` em vez de fechar; três recusas fecham; o engine direto responde `AuthenticateOk{ok = false}`)* Protocolo minor 2: `Authenticate{mecanismo, payload}` →
  `AuthenticateOk{principal}` / erro, logo depois do `HelloOk`; o `HelloOk`
  anuncia os mecanismos aceitos. Clientes minor ≤ 1 só entram em proxies com
  política anônima.
- [x] 6.2 *(`include/modb/proxy/token_policy.hpp`: SHA-256 próprio, `TokenStore` com comparação sem saída antecipada, `TokenPolicy` que autentica e delega o resto a outra política; `modb-proxy --tokens FILE` e `modb-proxy hash-token`)* Mecanismo `token` (bearer, comparado a um arquivo de tokens com hash) como
  primeiro mecanismo; outros mecanismos entram como políticas.
- [x] 6.3 *(`Client::authenticate`/`authenticate_token`, `ConnectionOptions::token`; o cliente Python ganhou `token=` e `authenticate()`; `docs/PROTOCOLO_CLIENTES.md` §2.1; `tests/proxy_auth_test.cpp`)* `Client`/`ServerConnection`: `authenticate(...)` opcional.

#### X7 — Políticas de referência *(médio)*

- [ ] 7.1 `read_only`: recusa procs `read_write` (o modo vem do catálogo de
  procs do engine, `sys.procs`).
- [ ] 7.2 `allowlist`: procs, facades e consultas permitidas por role, lidas de
  um arquivo de configuração.
- [ ] 7.3 Auditoria: uma linha por chamada (principal, operação, resultado,
  duração) no mesmo formato do log do `server_host`.
- [ ] 7.4 Limites por principal: chamadas/s e streams abertos.

#### X8 — TLS no proxy *(médio)*

- [ ] 8.1 Escolher a biblioteca (OpenSSL no Linux, Schannel no Windows, ou só
  OpenSSL) numa nota no ADR-028; o engine não depende dela.
- [ ] 8.2 `--tls-cert`/`--tls-key`; cliente com `--tls` e verificação do
  certificado.

### P3 — operação e desempenho

#### X9 — `server_host` como engine *(médio)*

- [ ] 9.1 `modb_add_server` gera um engine que escuta em `--local CAMINHO`;
  `--listen-tcp` mantém o modo atual.
- [ ] 9.2 Unidades systemd e compose: engine + proxy, com o socket num volume
  compartilhado; atualizar a imagem OCI e `serve --from-env`.
- [ ] 9.3 Probes: o proxy responde vivo/pronto conforme o estado do link.

#### X10 — Medição *(médio, máquina dedicada)*

- [ ] 10.1 `modb_rpc_bench` e o caso de consulta do `modb_load` com o alvo
  "direto" ou "via proxy".
- [ ] 10.2 Relatório: ops/s e p50/p99 por fase, direto vs proxy, 1 e N clientes.
  Se o salto pesar, avaliar o link sobre o anel shm (ADR-026).

#### X11 — shm no proxy *(pequeno)*

- [ ] 11.1 `ShmAttach` passa a ser atendido pelo proxy (cliente ↔ proxy); o
  proxy repassa as OpCalls pelo link, aplicando a política como no TCP.

### P4 — fim da transição

#### X12 — Engine só local *(pequeno)*

- [ ] 12.1 Remover `--listen-tcp` e o `SocketSink` externo do engine (o modo
  direto fica só para testes, se ainda servir).
- [ ] 12.2 Atualizar `docs/reference/networking-protocol.md`, o DEVELOPER_GUIDE,
  o treinamento (08, 09) e o RASTREADOR.

## Fora do escopo

- Réplica de leitura por WAL (ADR-016/020): tem protocolo próprio e não passa
  pelo `net::Server`.
- Sessões de cliente migrando entre engines (failover transparente).
- Autorização por linha ou por objeto: a política vê a mensagem, não os dados;
  as procs podem usar o principal da sessão para isso.
