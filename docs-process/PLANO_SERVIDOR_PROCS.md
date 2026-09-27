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
- **autenticação**, que não existe: sem usuário e sem TLS;
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
6. **Autenticação mínima**: chave compartilhada (token) no `Hello`, conferida antes de qualquer outra mensagem; o padrão continua escutando em `127.0.0.1`. TLS fica documentado como túnel (SSH/stunnel) até uma tarefa própria.

## Tarefas

### P0 — servidor com procs utilizável

#### S1 — `modb::server_host` e `modb_add_server` *(médio)*

- [ ] 1.1 Biblioteca `modb_server_host`: `run(argc, argv, modulos)` com `--db`, `--host`, `--porta`, `--config ARQ`, sinais (SIGINT/SIGTERM → `request_stop`), log em stderr, `READY <porta>`
- [ ] 1.2 Função CMake `modb_add_server(<alvo> MODULES <libs>)` que gera o `main`
- [ ] 1.3 `examples/server_procs/`: servidor mínimo com um módulo de exemplo e um cliente que chama uma proc; teste de ponta a ponta (servidor em processo filho, cliente real)
- [ ] 1.4 ADR "Servidor de aplicação com procedures compiladas" (este desenho, isolamento sem sandbox, um banco por servidor)

#### S2 — Valores autodescritos para argumentos e resultados *(médio)*

- [ ] 2.1 `ops::Valor` (nulo, bool, int64, double, texto, id, lista, mapa) + codificação binária versionada + testes de ida e volta
- [ ] 2.2 Conversão `Valor` ↔ JSON (para gateways web e para o CLI)
- [ ] 2.3 `Args` com leitura tipada e erro claro (`a.id("exemplar")`, `a.texto_ou("nome", "")`)

#### S3 — Procs declaradas como função, com erros de regra *(médio)*

- [ ] 3.1 `Modulo::proc(nome, modo, descricao, fn)` sobre o `OperationRegistry` existente (sem mudar o protocolo)
- [ ] 3.2 Códigos de erro de regra (`invalido`, `nao_encontrado`, `conflito`, `interno`) no `OpResult`; exceção vira `interno` com rollback
- [ ] 3.3 `modb call <host> <porta> <proc> '<json>'` no CLI, para testar procs à mão

#### S4 — API de dados das procs *(médio)*

- [ ] 4.1 `Contexto` expõe, dentro da transação da chamada: `ler/criar/atualizar/set/remover`, `consulta<T>()` (com `where`/`equals`/`between`), `por_indice<T>(campo, valor)`, coleções (`PersistentVector`/`BlobStore`) e o relógio do servidor
- [ ] 4.2 Procs `read_only` rodam sob snapshot, sem abrir transação de escrita
- [ ] 4.3 Tipos e índices declarados pelo módulo (`modulo.tipo(binding)`, `modulo.indice<T>(campo)`) e aplicados na abertura pelo `server_host`

### P1 — seguro de operar

#### S5 — Autenticação por token *(pequeno)*

- [ ] 5.1 Campo de credencial no `Hello` (versão menor nova do protocolo, compatível); servidor recusa sem token válido quando configurado
- [ ] 5.2 Tokens no arquivo de configuração (nunca em argumento de linha de comando, que aparece em `ps`); cliente lê de variável de ambiente
- [ ] 5.3 Documentar TLS por túnel até existir TLS nativo

#### S6 — Operação *(médio)*

- [ ] 6.1 Arquivo de configuração (banco, host, porta, tokens, limites de conexões e de tempo de proc)
- [ ] 6.2 Log estruturado por chamada (proc, duração, resultado, código de erro)
- [ ] 6.3 Rodar como serviço: unidade systemd e Windows Service (ou NSSM) documentados em `docs/OPERACAO.md`; teste de reinício depois de matar o processo (o WAL recupera)
- [ ] 6.4 Tempo máximo por proc (cooperativo: o `Contexto` checa o prazo a cada acesso ao banco) e desfaz a transação ao estourar

#### S7 — Descoberta de procs *(pequeno)*

- [ ] 7.1 `sys.procs` devolve nome, modo, descrição e argumentos esperados de cada proc
- [ ] 7.2 `modb procs <host> <porta>` no CLI

### P2 — biblioteca no novo modelo

#### S8 — `biblioteca-server`: módulo de procs *(médio)*

- [ ] 8.1 Reorganizar o repositório `biblioteca`: `modulo/` (modelo + procs), `servidor/` (o `main` via `modb_add_server`), `web/` (cliente)
- [ ] 8.2 Portar as regras de `src/biblioteca.cpp` para procs: `autores.listar/obter/criar/atualizar/remover` (e o mesmo para editoras, livros, exemplares e leitores), `emprestimos.listar/emprestar/devolver`, `resumo`, `exemplo.carregar`
- [ ] 8.3 Testes das procs contra o servidor de verdade (processo filho, banco em arquivo temporário): as 55 verificações de hoje, agora pela rede

#### S9 — `biblioteca-web`: só cliente *(pequeno)*

- [ ] 9.1 A web deixa de linkar o motor: só `modb::app_client` + cpp-httplib; cada rota HTTP vira uma chamada de proc (`Valor` ↔ JSON da S2.2), códigos de erro → 400/404/409
- [ ] 9.2 Configuração do endereço e do token do servidor; reconexão se o servidor reiniciar
- [ ] 9.3 Teste: web + servidor em processos separados, derrubar o servidor no meio e ver a web se recuperar

### P3 — depois

#### S10 — Concorrência *(ver PLANO_CONCORRENCIA.md)*

- [ ] 10.1 Hoje o servidor serializa tudo (`engine_mutex_`), igual ao mutex da biblioteca: correto, mas uma proc por vez. Procs `read_only` concorrentes dependem das C6–C8; as de escrita seguem uma por vez (C10.2: fila de escritores)

#### S11 — Clientes em outras linguagens *(opcional)*

- [ ] 11.1 Gateway HTTP/JSON genérico (`/proc/<nome>` → `OpCall`) como executável à parte, reaproveitando a S2.2; ou cliente do protocolo em outra linguagem, se aparecer a necessidade

#### S12 — Fora do escopo (registrar no ADR da S1.4)

- vários bancos por servidor; sandbox de procs; procs carregadas em tempo de execução (`.dll`/`.so`); multi-writer.

## Ordem sugerida

S1 → S2 → S3 → S4 → S8 → S9 → S5 → S6 → S7 → S10 → S11.

A biblioteca (S8/S9) entra logo depois da API de procs porque ela é o teste real
do desenho; autenticação e operação vêm antes de usar em qualquer lugar fora da
máquina local.

## Registro de execução

| Data | Tarefa | Commit | Resultado |
|---|---|---|---|
| 2026-09-27 | Plano | — | Levantamento do servidor, do protocolo e dos módulos por leitura de código |
