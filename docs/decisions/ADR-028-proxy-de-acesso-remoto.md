# ADR-028 — Acesso remoto por proxies: o engine só fala localmente

- Estado: aceito (implementado em `feat/proxy`, PLANO_PROXY X1–X7, X9–X11)
- Data: 2026-09-28
- Relacionados: ADR-010 (protocolo binário), ADR-011 (concorrência do servidor),
  ADR-025 (servidor com procs compiladas), ADR-026 (memória compartilhada),
  ADR-027 (leitores concorrentes), `docs-process/PLANO_PROXY.md`

## Contexto

Hoje o `net::Server` faz tudo num processo só:

1. **transporte externo**: listener TCP, compressão RLE, limites de frame,
   idle timeout, anel de memória compartilhada;
2. **sessão e protocolo**: `Hello`, streams multiplexadas por `query_id`,
   `Cancel`, backpressure pela janela TCP;
3. **motor**: `Database`, `OperationRegistry`, `FacadeCatalog`, procs.

Queremos pôr **segurança na frente do banco** (autenticação, autorização por
operação, auditoria, TLS, limites por cliente) e poder ter **várias
implementações** dessa frente — um proxy só de leitura, outro com TLS e
autenticação, outro para outra linguagem ou outro transporte — sem mexer no
motor nem recompilar o servidor de cada aplicação.

## Decisão

Separar em dois papéis, em processos distintos:

```
cliente ──TCP/TLS──► proxy A (TLS + auth) ─┐
cliente ──TCP──────► proxy B (só leitura) ─┼── link local ──► engine
cliente ──shm──────► proxy C               ─┘  (multiplexado)   Database + procs
```

### Engine

- É o `net::Server` de hoje **sem o transporte externo**: dono do arquivo, das
  procs e das sessões.
- Escuta **só um endpoint local**: socket AF_UNIX (Linux; Windows 10 1803+
  também tem AF_UNIX). O caminho do socket é protegido pelas permissões do
  sistema de arquivos (dono do processo, `0600`/ACL do diretório).
- Na transição, `--listen-tcp HOST:PORTA` mantém o modo atual (cliente direto
  no engine) para testes, benchmarks e aplicações existentes. Sai quando os
  proxies cobrirem os usos (P4 do plano).

### Link local proxy ↔ engine (multiplexado)

Uma conexão de link carrega **muitas sessões de cliente**. Cada frame ganha o
número da sessão **na frente** do frame do cliente:

```
| session u32 | length u32 | type u8 | payload |
```

O que vem depois do `session` é um frame do protocolo do cliente sem mudança,
então as mensagens de sessão passam por `encode_message`/`decode_message` sem
cópia nem outro codec. As mensagens de controle do link usam tipos a partir de
`0x80`, que o protocolo do cliente não usa.

- `session = 0` é o canal de controle do link: `LinkHello`/`LinkHelloOk`
  (versão do link, segredo compartilhado opcional, nome do proxy).
- `SessionOpen{principal, roles, atributos}` → `SessionOpenOk{ok, code}`
  (na sessão nova): o proxy já autenticou o cliente e diz **quem** é; o
  engine não conhece credenciais.
- `SessionClose{code, message}`, de qualquer lado: o cliente saiu, ou o engine
  encerrou a sessão; o engine cancela os streams e
  descarta o estado da sessão.
- As mensagens de sessão são as do protocolo do cliente (`Query`, `OpCall`,
  `Cancel`, `Facade*`, e as respostas), com os mesmos ids: `query_id` e
  `call_id` continuam escopados pela sessão, então **o proxy não reescreve ids**.
- `StreamCredit{query_id, frames}`: controle de fluxo por stream.
  Num link compartilhado a janela TCP não serve de backpressure — um cliente
  lento pararia todos. O engine só envia `ObjectFrame` com crédito; o proxy
  devolve crédito à medida que entrega ao cliente (como a janela do HTTP/2).
- O link não usa compressão: é local. A compressão fica entre proxy e cliente.

### Proxy

- **Decodifica e recodifica** cada mensagem do cliente (escolha deliberada:
  permite filtrar e reescrever argumentos, não só aceitar/recusar).
- Faz a negociação `Hello`/`HelloOk` com o cliente (codec, limites, timeout) e
  a autenticação (mensagens novas no protocolo do cliente, minor 2).
- Passa cada mensagem por uma **política** (interface C++):
  `authenticate`, `authorize(principal, mensagem)` → permite / recusa /
  reescreve, `audit(principal, mensagem, resultado)`.
- Implementações vêm de combinar transporte + política. A biblioteca
  `modb::proxy` traz o laço, o link e políticas básicas (passagem, só leitura,
  lista de procs permitidas); cada proxy é um `main` pequeno.

### Onde fica cada coisa

| Responsabilidade | Hoje | Depois |
|---|---|---|
| TCP externo, TLS | Server | proxy |
| Compressão RLE, limites de frame, idle timeout do cliente | Server | proxy |
| Autenticação, autorização, auditoria | — | proxy (política) |
| Sessão, streams, Cancel, crédito | Server | engine (sessão do link) |
| Procs, facades, transações | Server | engine |
| Principal da sessão visível às procs | — | engine (`ExecutionContext`) |
| Anel shm (ADR-026) | cliente ↔ Server | cliente ↔ proxy |
| Réplica por WAL (ADR-016) | protocolo próprio | inalterado (fora do escopo) |

## Alternativas avaliadas

| Opção | Por que não |
|---|---|
| Proxy L4 (só repassa bytes) | não autoriza por operação; só TLS e IP |
| Proxy que só lê o cabeçalho do frame | não reescreve argumentos; descartado pelo pedido |
| Uma conexão local por cliente | mais simples (sem crédito, sem `session`), mas um descritor e uma thread no engine por cliente, e um proxy com 10 mil clientes abriria 10 mil conexões locais |
| Segurança dentro do engine | cada política nova exigiria recompilar o servidor de cada aplicação; um defeito na borda roda no processo que tem o arquivo |
| Proxy dentro do processo do engine (plugin) | mesmo problema de isolamento; fica só como modo de transição (`--listen-tcp`) |

## Notas da implementação

- **Catálogo no link.** O `LinkHelloOk` leva as operações do engine e o modo de
  cada uma (`OperationRegistry::list`); o proxy o entrega às políticas por
  `Policy::on_engine` a cada abertura do link. É o que a política `read_only`
  usa, sem depender de `sys.procs`.
- **Autenticação no protocolo do cliente (minor 2).** `Authenticate` /
  `AuthenticateOk` logo depois do `HelloOk`, que lista os mecanismos. Antes de
  autenticar, o proxy responde cada pedido com `unauthenticated` (um cliente
  antigo recebe erros claros, não uma conexão fechada); três recusas fecham.
  O engine direto responde `AuthenticateOk{ok = false}`.
- **Anel shm no proxy.** O anel de um cliente ganha uma sessão própria no
  engine, com o mesmo chamador; as respostas dela saem pelo anel.
- **Execução no engine.** Por link, um pool de workers; cada sessão é uma fila
  que um worker drena (as respostas saem na ordem das chamadas), e sessões
  diferentes rodam em paralelo. `Cancel` e crédito não entram na fila.
- **Windows.** `connect` num AF_UNIX sob `%LOCALAPPDATA%` falhou com
  `WSAEINVAL` na máquina de desenvolvimento; os testes põem o socket no
  diretório do build.
- **Pendente.** TLS no proxy (X8: escolha da biblioteca), a medição direto ×
  proxy na máquina dedicada (X10.2) e o fim do TCP direto no engine (X12.1).

## Consequências

- Um salto a mais por chamada (proxy). O link local é AF_UNIX, sem compressão
  e sem TLS; o custo precisa ser medido (máquina dedicada) contra o TCP direto
  de hoje. Se pesar, o link pode passar para o anel shm da ADR-026.
- O engine confia no link: quem abre o socket local fala como qualquer
  principal. A proteção é a permissão do socket + o segredo do `LinkHello`.
- A sessão do engine deixa de ser "uma thread por socket": vira um objeto
  alimentado por uma fila de entrada e com uma saída (socket direto ou link).
  É essa separação que permite os dois modos durante a transição.
- `Hello` do cliente muda de dono (proxy). O `HelloOk` que o cliente recebe
  continua o mesmo; `baseline` e nome do banco vêm do `LinkHelloOk`.
- Crédito por stream substitui a janela TCP como backpressure no link; o teste
  de backpressure da Fase 8D passa a valer também através do proxy.
