# Modelo de falhas do runtime de módulos (Fase 9)

Este documento completa a tarefa 9.9 do plano ODB++. A decisão de
arquitetura está em
[ADR-012](decisions/ADR-012-runtime-de-modulos-no-processo.md).

## Fronteira de execução

- Módulos correm **no processo** do servidor, a partir de origem
  administrativa (`ModuleLoader` + allowlist de hash).
- O cliente envia apenas `OpCall` (id + args serializados); nunca binários
  nem caminhos de carga.
- `ExecutionContext` é a única porta: objetos tipados, transação/snapshot e
  logger. Páginas, WAL, Buffer Pool e índices físicos não são expostos.

## Contrato transacional

| Desfecho | Efeito |
|---|---|
| `Operation::execute` retorna sucesso | `commit` |
| `Result` de erro | `rollback` |
| Exceção C++ capturada pelo registry | `rollback` + `OpResult` de erro; o motor segue utilizável |

## Crash e recuperação

1. Código nativo defeituoso pode corromper memória ou derrubar o processo —
   não há sandbox no MVP.
2. A instância deve rodar sob **supervisor externo** (systemd ou Windows
   Service).
3. Após restart, a abertura do banco executa **WAL recovery** (Fase 5):
   commits duráveis reaparecem; trabalho sem commit não aparece.
4. O teste `modb.operation_server` documenta o ciclo: `client.call` →
   commit → reabrir o arquivo → saldos consistentes.

## Isolamento futuro

O codec versionado de argumentos/resultados e o manifesto por id/hash
preservam a fronteira para workers isolados ou sandbox sem mudar o
contrato da aplicação.
