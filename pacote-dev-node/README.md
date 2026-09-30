# Pacote para o dev: aplicação React + Node sobre o Ring0 (com proxy)

O que é preciso para reescrever uma aplicação React + Node usando o Ring0 como
backend, acessado através do `modb-proxy` (2026-09-30).

- **Ring0 / moDb:** https://github.com/sergioleal/moDb, tag
  [`v0.1.0`](https://github.com/sergioleal/moDb/releases/tag/v0.1.0) (protocolo
  minor 2), com o **SDK binário para Linux x86_64** anexado à release.
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
| 0 | `00-sdk/SDK.md` | [`examples/sdk_app/`](https://github.com/sergioleal/moDb/tree/v0.1.0/examples/sdk_app) | Baixar o SDK da release, compilar o servidor da aplicação só com o próprio módulo e subir engine + proxy com token. **Comece por aqui.** |
| 1 | `01-protocolo-cliente/PROTOCOLO_CLIENTES.md` | [`include/modb/error.hpp`](https://github.com/sergioleal/moDb/blob/v0.1.0/include/modb/error.hpp) | Especificação byte a byte do cliente: frame, `Hello`, `Authenticate`, `OpCall`/`OpResult`, `Value`, códigos de erro. **É o que o cliente Node precisa implementar.** |
| 2 | — | [`clients/python/modb_client.py`](https://github.com/sergioleal/moDb/blob/v0.1.0/clients/python/modb_client.py) | Cliente de referência em Python (só biblioteca padrão), para portar para Node (`net` + `Buffer`). |
| 3 | `03-procs/ADR-025-…md` | [`include/modb/server/module.hpp`](https://github.com/sergioleal/moDb/blob/v0.1.0/include/modb/server/module.hpp), [`examples/server_procs/`](https://github.com/sergioleal/moDb/tree/v0.1.0/examples/server_procs), [`cmake/ModbServer.cmake`](https://github.com/sergioleal/moDb/blob/v0.1.0/cmake/ModbServer.cmake) | Como escrever um módulo de procs (`ModuleBuilder`, `Context`, `ops::Args`, erros de regra). O `notas-server` é o modelo mínimo. |
| 4 | `04-operacao/OPERACAO.md` | [`examples/server_procs/deploy/`](https://github.com/sergioleal/moDb/tree/v0.1.0/examples/server_procs/deploy) | Subir engine + proxy: configuração, `--local`, tokens (`modb-proxy hash-token`), unidades systemd, sonda (`modb ping`). Leia as seções "Servidor de aplicação" e "Proxy de acesso remoto". |
| 5 | `05-proxy/ADR-028-…md`, `05-proxy/networking-protocol.md` (§2.7) | [`include/modb/proxy/`](https://github.com/sergioleal/moDb/tree/v0.1.0/include/modb/proxy), [`apps/modb_proxy/main.cpp`](https://github.com/sergioleal/moDb/blob/v0.1.0/apps/modb_proxy/main.cpp) | Como o proxy funciona e as políticas prontas: token, allowlist de procs, `read_only`, rate limit, auditoria. |
| 6 | `06-concorrencia-erros/ADR-027-…md` | [`include/modb/error.hpp`](https://github.com/sergioleal/moDb/blob/v0.1.0/include/modb/error.hpp) | Semântica das escritas concorrentes e a tabela de `ErrorCode` (os números do `OpResult`). |
| 7 | `07-exemplo-biblioteca/README.md` | [`modulo/biblioteca_procs.cpp`](https://github.com/sergioleal/biblioteca0/blob/71782858f75a311971b9897f80bef3bfce7c0a3c/modulo/biblioteca_procs.cpp), [`servidor/`](https://github.com/sergioleal/biblioteca0/tree/71782858f75a311971b9897f80bef3bfce7c0a3c/servidor), [`web/gateway.cpp`](https://github.com/sergioleal/biblioteca0/blob/71782858f75a311971b9897f80bef3bfce7c0a3c/web/gateway.cpp) | O caso real mais próximo: 31 procs, o servidor gerado por `modb_add_server` e o gateway web (rota → proc, pool de conexões). |

## Estado atual: o que existe e o que falta

Pronto e testado: servidor com procs, proxy com autenticação por token,
políticas e auditoria, engine atrás do proxy, anel de memória compartilhada
atendido pelo proxy.

Falta:

1. **Cliente Node.** Não existe. É a primeira tarefa (base: passos 1 e 2).
2. **TLS no proxy** (tarefa X8). O token viaja em claro: deixe o Node e o
   proxy na mesma máquina ou numa rede confiável, e coloque o TLS voltado ao
   navegador no Node ou num nginx.
3. **Medições de desempenho** (direto × proxy, TCP × anel). As ferramentas
   existem, mas nada foi medido em máquina dedicada ainda.

## Regras para o cliente Node

- **Conecte sempre pelo proxy.** O TCP direto no engine (`--listen-tcp`) vai
  ser removido (tarefa X12).
- **Use TCP.** O anel de memória compartilhada exigiria um addon nativo para
  `mmap` em Node. Fica fora por ora.
- **Use um pool de conexões.** As respostas de uma conexão saem na ordem dos
  pedidos; a concorrência vem de várias conexões.
- **Autentique logo após o `HelloOk`** (mecanismo `token`). Antes disso, todo
  pedido volta com `unauthenticated` (73); três recusas fecham a conexão.
- **Idle timeout:** reconecte quando o servidor fechar. Repita automaticamente
  **só leituras**, porque uma escrita pode ter sido confirmada antes da queda.
- **Mapeamento de erros para HTTP:** `invalid_argument` (1) → 400,
  `record_not_found` (30) → 404, `conflict` (70) → 409,
  `unauthenticated` (73) → 401, `permission_denied` (74) → 403,
  `operation_timeout` (72) → 504, `internal_error` (71) → 500, servidor
  indisponível → 503.

## Limites do servidor

- **Uma proc de escrita por vez** (ADR-027). As procs de leitura rodam em
  paralelo, e as escritas se repetem automaticamente em conflito de snapshot.
- **Um banco por servidor.**
- **Procs compiladas no servidor.** Mudar uma regra é recompilar e trocar o
  executável; não há carga em tempo de execução nem sandbox.
- **Sem esquema formal de argumentos.** A descrição de cada proc
  (`sys.procs` / `modb procs`) diz o que ela espera.
- **Autorização por linha ou objeto fica nas procs**, que recebem o principal
  da sessão pelo `Context`. A política do proxy vê a mensagem, não os dados.
