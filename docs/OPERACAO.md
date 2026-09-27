# Operação — backup, restauração, supervisor e diagnóstico

Fase **10F**. Complementa [OPERACAO_MODULOS.md](OPERACAO_MODULOS.md) (falhas do
runtime de módulos) com o ciclo operacional do arquivo.

## Papéis dos arquivos

| Arquivo | Conteúdo |
|---|---|
| `<db>` (ex.: `shop.modb`) | páginas do banco (superbloco + dados) |
| `<db>.wal` | log de escrita antecipada; só existe com trabalho pendente ou após crash |

Nunca copie só um dos dois se o WAL existir: o par deve ser consistente.

## Supervisor

O processo do servidor/CLI deve rodar sob um supervisor externo:

- Linux: systemd
- Windows: Serviço Windows / agendador equivalente

Após crash, a **próxima abertura** do banco executa recovery do WAL (Fase 5):
commits duráveis reaparecem; transações sem commit não. Não há sandbox para
módulos nativos no MVP — um módulo defeituoso pode derrubar o processo; o
supervisor reinicia e o recovery restaura o estado durável.

## Backup quiescente

1. Pare escritores (pare o `modb serve` / feche o processo dono do arquivo).
2. Confirme que não há outro processo com o arquivo aberto.
3. Copie **atomicamente o par**:
   - `shop.modb`
   - `shop.modb.wal` (se existir)
4. Guarde os dois no mesmo snapshot (mesmo diretório/timestamp).

Exemplo (PowerShell, banco parado):

```powershell
New-Item -ItemType Directory -Force backup\2026-07-19 | Out-Null
Copy-Item shop.modb backup\2026-07-19\
if (Test-Path shop.modb.wal) { Copy-Item shop.modb.wal backup\2026-07-19\ }
```

Não faça backup “a quente” sem coordenação: páginas e WAL podem divergir.

**O WAL não é opcional no backup.** Com o checkpoint preguiçoso
([ADR-022](decisions/ADR-022-menos-fsync-por-commit.md)), o arquivo de dados só
fica completo num checkpoint: a cada `checkpoint_interval` commits (padrão 64)
e no fechamento limpo do banco. Depois de uma queda, ou com o processo ainda
aberto, os últimos commits podem estar só no WAL. Copiar o par cobre os dois
casos. Quem precisa copiar só o arquivo de dados deve chamar
`Database::checkpoint()` antes, com escritores parados.

## Restauração

1. Pare qualquer processo usando o destino.
2. Restaure `<db>` e, se existir no backup, `<db>.wal` para o mesmo prefixo.
3. Abra o banco (CLI ou API) — a recovery corre na abertura se o WAL estiver
   presente.
4. Rode diagnóstico:

```powershell
.\build\debug\modb.exe db check shop.modb
```

## Diagnóstico — `modb db check`

```powershell
modb db check <file>
```

Camadas:

1. Superbloco / abertura (`PageFile::open`)
2. Classificação por magic (DBRT, IDMD, IDMP, BLBP, IXDR, BTLF, BTIN, SLPG, THRP)
3. Validação de cabeçalhos (versão, length de blob, …)
4. Cadeias de TableHeap e registros legíveis em SLPG

Saída não zero indica páginas/registros inválidos. Bancos só com páginas
reconhecidas e sem erros de heap/registro são considerados verdes para o
critério operacional desta fase.

## Fluxo validado (demo)

```powershell
cmake --build --preset debug
.\build\debug\modb.exe demo employee shop.modb --force
.\build\debug\modb.exe db check shop.modb

# backup
New-Item -ItemType Directory -Force .\op-backup | Out-Null
Copy-Item shop.modb, shop.modb.wal -Destination .\op-backup -ErrorAction SilentlyContinue

# “desastre”: remove o original e restaura
Remove-Item shop.modb, shop.modb.wal -ErrorAction SilentlyContinue
Copy-Item .\op-backup\* -Destination .
.\build\debug\modb.exe db check shop.modb
.\build\debug\modb.exe oo employee get shop.modb 1 --schema 2
```

(Ajuste o ObjectId conforme a saída do demo.)

## Servidor de aplicação (`modb_add_server`)

Um servidor gerado por `modb_add_server` (ADR-025; exemplo: `notas-server`) é o
banco como processo, com as procs da aplicação compiladas dentro. Ele é
operado como qualquer serviço.

### Configuração

`--config ARQUIVO` lê linhas `chave = valor` (`#` comenta); as flags valem mais
que o arquivo, em qualquer ordem. Caminhos relativos no arquivo são relativos à
pasta dele. Exemplo completo: [`examples/server_procs/deploy/notas-server.conf`](../examples/server_procs/deploy/notas-server.conf).

| Chave / flag | Padrão | Efeito |
|---|---|---|
| `db` / `--db` | — (obrigatório) | arquivo do banco; criado se não existir |
| `host` / `--host` | `127.0.0.1` | endereço de escuta |
| `port` / `--port` | `7474` | porta TCP (`0` = qualquer livre; a escolhida sai em `READY <porta>`) |
| `max_streams` / `--max-streams` | do `net::Server` | streams simultâneos por conexão |
| `idle_timeout_ms` / `--idle-timeout-ms` | `30000` | fecha conexões ociosas |
| `proc_timeout_ms` / `--proc-timeout-ms` | `0` (sem limite) | chamada que passar disso falha com `operation_timeout` e é desfeita |
| `log` / `--log` | stderr | log de chamadas: arquivo (acrescenta), vazio = stderr, `off` = nenhum |

O tempo limite é cooperativo: o `Context` confere o prazo a cada acesso ao banco
(`read`, consultas, `find`, `create`, `update`, `set`, `remove`) e, ao fim, o
`OperationRegistry` desfaz a transação de qualquer chamada que tenha passado do
prazo, mesmo que a proc não tenha notado. Uma proc que calcula muito sem tocar
no banco pode conferir por conta própria com `c.in_time()`.

### Log de chamadas

Uma linha por chamada, começando pelo instante UTC:

```
2026-09-27T16:32:12.754Z call notas.criar write 3.502ms ok
2026-09-27T16:32:12.759Z call notas.editar write 2.981ms error 70 já existe uma nota com esse texto
2026-09-27T16:32:12.749Z call nao.existe - 0.000ms error 48 operation not found: nao.existe
```

O número depois de `error` é o `ErrorCode` que o cliente recebe. As mensagens das
procs (`c.log()`) vão para o mesmo destino, como `info`/`warn`/`error`.

### Parada

`SIGINT`/`SIGTERM` (Ctrl+C; `SIGBREAK` no Windows) chamam `request_stop`: o
servidor para de aceitar, fecha as sessões abertas (inclusive clientes ociosos),
termina a chamada em curso, imprime `stopped` e sai com 0. Um crash ou
`kill -9` não perde nada confirmado: na próxima abertura o WAL recupera (coberto
por `modb.server_host`, que mata o processo à força e reabre o mesmo banco).

### Como serviço — Linux (systemd)

Unidade de exemplo: [`examples/server_procs/deploy/notas-server.service`](../examples/server_procs/deploy/notas-server.service)
(`Restart=on-failure`, `KillSignal=SIGTERM`).

```bash
sudo cp notas-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now notas-server
journalctl -u notas-server -f
```

### Como serviço — Windows (NSSM)

O servidor é um programa de console; o [NSSM](https://nssm.cc) o roda como
serviço e o para com Ctrl+C (parada limpa):

```powershell
nssm install notas-server C:\notas-server\notas-server.exe --config C:\notas-server\notas-server.conf
nssm set notas-server AppDirectory C:\notas-server
nssm set notas-server AppStopMethodConsole 10000
nssm set notas-server AppExit Default Restart
nssm set notas-server AppStdout C:\notas-server\notas-server.out.log
nssm set notas-server AppStderr C:\notas-server\notas-server.err.log
Start-Service notas-server
```

### Descoberta de procs

`sys.procs` (módulo de sistema, carregado em todo servidor) devolve
`[{name, mode, module, description}]`. Pelo CLI:

```powershell
modb procs 127.0.0.1 7474
modb call 127.0.0.1 7474 notas.listar '{\"contem\": \"café\"}'
```

Não há esquema formal de argumentos: por convenção a descrição da proc cita os
argumentos (`{texto}`, `{id}`), e um argumento ausente ou de tipo errado volta
explicado no erro (`argument 'id' is required`).

## Relacionados

- Transações / crash: `modb demo tx`, `modb tx crash`, `modb tx wal-info`
- API: [API_PUBLICA.md](API_PUBLICA.md)
- Formato: [FORMATO_DE_ARQUIVO.md](FORMATO_DE_ARQUIVO.md)
