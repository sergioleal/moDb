# Exemplos do Ring0

Este diretorio concentra exemplos usados pelo projeto e por aplicacoes
consumidoras. Ha dois tipos de exemplo:

- modulos de dominio reutilizados pelos testes, CLI e servidor;
- programas executaveis em `server/by_phase`, um por fase ate a Fase 13.

## Modulos de dominio

| Pasta | O que demonstra | Como e usado |
|---|---|---|
| `transfer_funds/` | Uma operacao de dominio atomica, `TransferFunds`, que debita uma conta e credita outra dentro de uma transacao. | Usado pela Fase 9, pelos testes de operacao e pelos exemplos de servidor que chamam `ServerConnection::call`. |
| `accounts_facade/` | Uma facade tipada `AccountsFacade` sobre a operacao `TransferFunds`. | Usado pela Fase 11 para demonstrar descoberta de facades, negociacao de versao e `open_facade<T>()`. |

Essas pastas nao geram executaveis proprios. Elas entram como codigo de exemplo
linkado na biblioteca principal e nos exemplos de servidor.

## Exemplos por fase

Os exemplos executaveis ficam em `server/by_phase`. Cada pasta tem seu proprio
`README.md` com os comandos de build e execucao para Windows/CLion e Linux.

| Fase | Arquivo | Alvo CMake | O que demonstra |
|---|---|---|---|
| 00 | `server/by_phase/phase_00/version_compatibility.cpp` | `ring0_server_phase_00_version_compatibility` | Nome/versao do projeto e negociacao de compatibilidade major/minor. |
| 01 | `server/by_phase/phase_01/bind_type.cpp` | `ring0_server_phase_01_bind_type` | Binding de um tipo C++ e registro no catalogo. |
| 02 | `server/by_phase/phase_02/persist_reopen.cpp` | `ring0_server_phase_02_persist_reopen` | Criar objeto, fechar banco, reabrir e materializar pelo `ObjectId`. |
| 03 | `server/by_phase/phase_03/handle_update.cpp` | `ring0_server_phase_03_handle_update` | Uso de `Handle<T>` para atualizar campo tipado em transacao. |
| 04 | `server/by_phase/phase_04/relationships.cpp` | `ring0_server_phase_04_relationships` | `Ref<T>` e `OwnedRef<T>` entre objetos de dominio. |
| 05 | `server/by_phase/phase_05/transaction_recovery.cpp` | `ring0_server_phase_05_transaction_recovery` | Commit duravel, WAL e recovery apos reabertura. |
| 06 | `server/by_phase/phase_06/snapshot_read.cpp` | `ring0_server_phase_06_snapshot_read` | Snapshot/MVCC: leitura estavel frente a commit posterior. |
| 07 | `server/by_phase/phase_07/streaming_query.cpp` | `ring0_server_phase_07_streaming_query` | Query tipada com filtro, limite e streaming local. |
| 08 | `server/by_phase/phase_08/connect_query.cpp` | `ring0_server_phase_08_connect_query` | Aplicacao conectando ao servidor, negociando protocolo e coletando objetos. |
| 09 | `server/by_phase/phase_09/call_operation.cpp` | `ring0_server_phase_09_call_operation` | Chamada remota de operacao de dominio com `ServerConnection::call`. |
| 10 | `server/by_phase/phase_10/handshake_capabilities.cpp` | `ring0_server_phase_10_handshake_capabilities` | Capacidades publicas negociadas no handshake. |
| 11 | `server/by_phase/phase_11/open_facade.cpp` | `ring0_server_phase_11_open_facade` | Facade remota tipada e invocacao por `open_facade<T>()`. |
| 12 | `server/by_phase/phase_12/graph_traversal.cpp` | `ring0_server_phase_12_graph_traversal` | Travessia BFS de grafo usando a API de grafos. |
| 13 | `server/by_phase/phase_13/async_io.cpp` | `ring0_server_phase_13_async_io` | I/O posicional assincrono com `AsyncFile`, barreira e backend nativo. |

## Como rodar

Configure o projeto uma vez:

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --preset debug
```

Compile o alvo desejado:

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_08_connect_query
```

No Windows com MinGW do CLion, inclua as DLLs no `PATH` antes de executar:

```powershell
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_08_connect_query.exe
```

No Linux:

```bash
cmake --preset debug
cmake --build --preset debug --target ring0_server_phase_08_connect_query
./build/debug/ring0_server_phase_08_connect_query
```
