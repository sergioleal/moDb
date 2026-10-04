# Pacote para o dev: aplicação React + Node sobre o Ring0 (com proxy)

O que é preciso para reescrever uma aplicação React + Node usando o Ring0 como
backend, acessado através do `modb-proxy` (2026-09-30).

- **Ring0 / moDb:** https://github.com/sergioleal/moDb, tag
  [`v0.3.0`](https://github.com/sergioleal/moDb/releases/tag/v0.3.0) (protocolo
  minor 3, páginas de 16 KB), com o **SDK binário para Linux x86_64** anexado à release.
- **biblioteca (exemplo):** https://github.com/sergioleal/biblioteca0, commit
  [`7178285`](https://github.com/sergioleal/biblioteca0/tree/71782858f75a311971b9897f80bef3bfce7c0a3c).

Os documentos deste pacote são cópias dos `.md` dessa tag e desse commit.
**Todo código é referenciado só no remoto**, com links fixados na tag e no
commit acima, para que não mudem quando os repositórios avançarem.

**Não é preciso compilar o Ring0.** O SDK traz o `modb-proxy`, o CLI `modb` e
as bibliotecas; o dev compila só o módulo de procs da aplicação
(`00-sdk/SDK.md`).

## Arquitetura alvo

```
React (navegador)
   │ HTTPS/JSON
Node (gateway fino: rota HTTP → proc, JSON ↔ Value, erro → 400/404/409)
   │ TCP, protocolo nativo (minor 2) + token
modb-proxy (autenticação, política, auditoria, limites)
   │ link local AF_UNIX
<app>-server = engine Ring0 + stored procedures em C++ (as regras de negócio)
```

As regras de negócio **saem do Node** e viram procs em C++ compiladas com o
servidor (ADR-025). O Node só chama procs pelo nome.

## Ordem de leitura

| Passo | Documento (no pacote) | Código (no remoto) | Para quê |
|---|---|---|---|
| 0 | `00-sdk/SDK.md` | [`examples/sdk_app/`](https://github.com/sergioleal/moDb/tree/v0.3.0/examples/sdk_app) | Baixar o SDK da release, compilar o servidor da aplicação só com o próprio módulo e subir engine + proxy com token. **Comece por aqui.** |
| 1 | `01-protocolo-cliente/PROTOCOLO_CLIENTES.md` | [`include/modb/error.hpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/include/modb/error.hpp) | Especificação byte a byte do cliente: frame, `Hello`, `Authenticate`, `OpCall`/`OpResult`, `Value`, códigos de erro. **É o que o cliente Node precisa implementar.** |
| 2 | — | [`clients/node/`](https://github.com/sergioleal/moDb/tree/v0.3.0/clients/node) (e o Python, [`clients/python/modb_client.py`](https://github.com/sergioleal/moDb/blob/v0.3.0/clients/python/modb_client.py)) | **Cliente Node de referência**, pronto: `Value`, conexão com token, pool e as regras de repetição. Use-o no gateway em vez de escrever outro. |
| 3 | `03-procs/ADR-025-…md` | [`include/modb/server/module.hpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/include/modb/server/module.hpp), [`examples/server_procs/`](https://github.com/sergioleal/moDb/tree/v0.3.0/examples/server_procs), [`cmake/ModbServer.cmake`](https://github.com/sergioleal/moDb/blob/v0.3.0/cmake/ModbServer.cmake) | Como escrever um módulo de procs (`ModuleBuilder`, `Context`, `ops::Args`, erros de regra). O `notas-server` é o modelo mínimo. |
| 4 | `04-operacao/OPERACAO.md` | [`examples/server_procs/deploy/`](https://github.com/sergioleal/moDb/tree/v0.3.0/examples/server_procs/deploy) | Subir engine + proxy: configuração, `--local`, tokens (`modb-proxy hash-token`), unidades systemd, sonda (`modb ping`). Leia as seções "Servidor de aplicação" e "Proxy de acesso remoto". |
| 5 | `05-proxy/ADR-028-…md`, `05-proxy/ADR-029-…md`, `05-proxy/networking-protocol.md` (§2.7, §2.8) | [`include/modb/proxy/`](https://github.com/sergioleal/moDb/tree/v0.3.0/include/modb/proxy), [`apps/modb_proxy/main.cpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/apps/modb_proxy/main.cpp) | Como o proxy funciona e as políticas prontas: token, allowlist de procs, `read_only`, rate limit, auditoria; delegação (`delegate`), `detail` e chave de idempotência (minor 3). |
| 6 | `06-concorrencia-erros/ADR-027-…md` | [`include/modb/error.hpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/include/modb/error.hpp) | Semântica das escritas concorrentes e a tabela de `ErrorCode` (os números do `OpResult`). |
| 7 | `07-exemplo-biblioteca/README.md` | [`modulo/biblioteca_procs.cpp`](https://github.com/sergioleal/biblioteca0/blob/71782858f75a311971b9897f80bef3bfce7c0a3c/modulo/biblioteca_procs.cpp), [`servidor/`](https://github.com/sergioleal/biblioteca0/tree/71782858f75a311971b9897f80bef3bfce7c0a3c/servidor), [`web/gateway.cpp`](https://github.com/sergioleal/biblioteca0/blob/71782858f75a311971b9897f80bef3bfce7c0a3c/web/gateway.cpp) | O caso real mais próximo: 31 procs, o servidor gerado por `modb_add_server` e o gateway web (rota → proc, pool de conexões). |

## Estado atual: o que existe e o que falta

Pronto e testado: servidor com procs, proxy com autenticação por token,
políticas e auditoria, engine atrás do proxy, anel de memória compartilhada
atendido pelo proxy.

O cliente Node de referência existe desde a `v0.1.3` (`clients/node/`), testado
contra o `notas-server` atrás do `modb-proxy`.

Falta:

1. **TLS no proxy** (tarefa X8). O token viaja em claro: deixe o Node e o
   proxy na mesma máquina ou numa rede confiável, e coloque o TLS voltado ao
   navegador no Node ou num nginx.
2. **Medições de desempenho** (direto × proxy, TCP × anel). As ferramentas
   existem, mas nada foi medido em máquina dedicada ainda.

## Regras para o cliente Node

O `clients/node/` já segue estas regras; elas valem para quem escrever outro
cliente ou usar a `Connection` sem o `Pool`.

- **Conecte sempre pelo proxy.** O TCP direto no engine (`--listen-tcp`) vai
  ser removido (tarefa X12).
- **Use TCP.** O anel de memória compartilhada exigiria um addon nativo para
  `mmap` em Node. Fica fora por ora.
- **Use um pool de conexões.** As respostas de uma conexão saem na ordem dos
  pedidos; a concorrência vem de várias conexões.
- **Autentique logo após o `HelloOk`** (mecanismo `token`). Antes disso, todo
  pedido volta com `unauthenticated` (73); três recusas fecham a conexão.
- **Idle timeout:** reconecte quando o servidor fechar. Repita automaticamente
  leituras e **só as escritas que levam chave de idempotência**: sem chave, a
  escrita pode ter sido confirmada antes da queda.
- **Mapeamento de erros para HTTP:** `invalid_argument` (1) → 400,
  `record_not_found` (30) → 404, `conflict` (70) → 409,
  `unauthenticated` (73) → 401, `permission_denied` (74) → 403,
  `operation_timeout` (72) → 504, `internal_error` (71) → 500, servidor
  indisponível → 503.
- **Números de erro:** use a tabela do `PROTOCOLO_CLIENTES.md` §3 (e o
  Apêndice A). Desde a `v0.1.2` os números são estáveis: código novo só entra
  no fim, nenhum é renumerado. Decida pelo `code` e pelo `detail.reason`, não
  pela mensagem.

## O que a `v0.3.0` muda: páginas de 16 KB

O SDK passa a usar páginas de 16 KB (antes 8 KB). Cada objeto persistido, com
todos os campos, precisa caber numa página: o limite sobe de 8156 para 16348
bytes por objeto codificado.
Bancos criados com SDKs anteriores não abrem nesta versão; recrie-os do zero.
O protocolo não muda.

## O que a `v0.2.0` acrescenta: protocolo minor 3

Clientes de minor 2 continuam funcionando com a `v0.2.0`, e o cliente Node da
`v0.2.0` funciona com servidores de minor 2 (sem o que é novo). Detalhes em
`01-protocolo-cliente/PROTOCOLO_CLIENTES.md` §3.1 e em
`05-proxy/ADR-029-delegacao-detalhe-e-idempotencia.md`.

- **Em nome de quem (delegação).** O token do gateway ganha a role `delegate`,
  e cada chamada leva `actingAs: "user:<id>"` (e atributos, se quiser). A proc
  lê o usuário em `c.caller().subject()`, e só o proxy decide quem pode delegar.
  Substitui o argumento `{user}` que cada proc conferia.
- **Erros estruturados.** A proc responde com
  `c.fail(conflict("..."), Value::object({{"reason", ...}, {"field", ...}}))`,
  e o gateway lê `ModbError.detail.reason` e `detail.field`. Substitui o
  código escrito dentro da mensagem.
- **Escritas repetíveis.** `idempotencyKey` (um UUID por escrita lógica): o
  servidor devolve o resultado já confirmado em vez de executar de novo, e o
  pool pode repetir a escrita depois de uma queda. A chave vale por 24 h
  (`idempotency_retention_s`). Um resultado acima de meia página (8 KB no SDK)
  guarda só a chave.
- **Limites por usuário.** Com delegação, `max_calls_per_second` vale por
  usuário, e `max_delegated_calls_per_second` limita a soma do gateway.

## O que a `v0.1.2` acrescenta

Nada muda no protocolo nem no formato do arquivo: quem já usa a `v0.1.1`
continua funcionando.

- **Configuração da aplicação no `.conf`.** O módulo declara
  `.setting(nome, padrão, descrição, validador)`, o servidor lê
  `<módulo>.<nome>` do `.conf` ou `--<módulo>.<nome>` e valida na subida, e a
  proc lê com `c.setting(nome)`. Substitui variáveis de ambiente
  (`04-operacao/OPERACAO.md`, "Configurações dos módulos").
- **Parada limpa sem sinal.** `--stop-on-stdin-eof on` no servidor e no proxy:
  um supervisor em Node para com `child.stdin.end()` em vez de `kill()`, que
  no Windows é à força (`OPERACAO.md`, "Windows").
- **Faixa e prefixo pelo índice nas procs.** `c.range<T>(campo, de, ate)` e
  `c.prefix<T>(campo, texto)` substituem a varredura com `c.all<T>()` para
  buscas por prefixo e por faixa.

## Limites do servidor

- **Uma proc de escrita por vez** (ADR-027). As procs de leitura rodam em
  paralelo, e as escritas se repetem automaticamente em conflito de snapshot.
- **Um banco por servidor.**
- **Um objeto cabe numa página.** No SDK, o objeto codificado tem no máximo
  16348 bytes, contando strings, `bytes` e campos `ops::Value`; acima disso,
  `record_too_large` (28). Conteúdo grande vai num blob (`BlobStore`) ou numa
  coleção persistente, e listas pequenas de valores, num campo `ops::Value`.
- **Procs compiladas no servidor.** Mudar uma regra é recompilar e trocar o
  executável; não há carga em tempo de execução nem sandbox.
- **Sem esquema formal de argumentos.** A descrição de cada proc
  (`sys.procs` / `modb procs`) diz o que ela espera.
- **Autorização por linha ou objeto fica nas procs**, que recebem o principal
  da sessão pelo `Context`. A política do proxy vê a mensagem, não os dados.
