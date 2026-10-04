# Biblioteca

Aplicação de biblioteca sobre o **moDb**, em dois processos: o
**biblioteca-server** é o banco com as regras da aplicação compiladas dentro
(stored procedures), e a **biblioteca-web** é só um cliente, que traduz a API
REST/JSON em chamadas de proc e serve o frontend em HTML/JS puro.

```
navegador ── HTTP ──▶ biblioteca-web ── protocolo moDb ──▶ biblioteca-server ─┬─ procs (modulo/)
                      (web/, sem banco)    (OpCall/OpResult)                   └─ moDb: biblioteca.modb (+ .wal)
```

Cada chamada de escrita roda numa transação no servidor: se uma regra falha no
meio, nada do que a chamada escreveu fica. O servidor é gerado por
`modb_add_server` (moDb, ADR-025); veja [`docs/OPERACAO.md` do moDb](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/OPERACAO.md) para
configuração, log de chamadas, tempo limite de procs e como rodar como serviço.

## Entidades

| Entidade | Campos | Relações |
|---|---|---|
| **Autor** | nome, nacionalidade, ano de nascimento | — |
| **Editora** | nome, cidade | — |
| **Livro** | título, ISBN (único), ano | 1 editora (`Ref`), N autores (coleção persistente `PersistentVector<Ref<Autor>>`) |
| **Exemplar** | código de tombo (único), estado (`disponivel`/`emprestado`/`baixado`) | 1 livro (`Ref`) |
| **Leitor** | nome, e-mail (único), telefone, ativo | — |
| **Empréstimo** | data do empréstimo, data prevista, data de devolução | 1 exemplar, 1 leitor (`Ref`) |

ISBN, tombo e e-mail têm índice (B+ tree do moDb), usado para garantir que sejam
únicos. Datas são texto ISO (`AAAA-MM-DD`): o moDb não tem tipo de data.

## Regras

- Um empréstimo só sai com exemplar **disponível** e leitor **ativo**, com até
  **3** empréstimos abertos por leitor e prazo de 1 a 60 dias (padrão 14).
- Emprestar e devolver mudam o estado do exemplar **na mesma transação (a da chamada)** que
  gravam o empréstimo: ou os dois ficam, ou nenhum.
- Empréstimo com a data prevista vencida aparece como **atrasado**.
- Não se remove o que ainda é referenciado: autor ou editora com livros, livro
  com exemplares, exemplar ou leitor com histórico de empréstimos (use "baixado"
  ou "inativo").

## Compilar e rodar

Pré-requisitos: CMake ≥ 3.30, Ninja, g++ ≥ 13 (ou outro compilador C++23) e o
repositório do moDb em `../moDb2` ([moDb no GitHub](https://github.com/sergioleal/moDb/tree/v0.3.0)) (ou `-DMODB_SOURCE_DIR=<caminho>`). Na
primeira configuração o CMake baixa o [cpp-httplib](https://github.com/yhirose/cpp-httplib)
v0.18.1 (header-only, MIT).

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Os executáveis ficam em `build\debug\bin`. Suba o servidor e depois a web
(em dois terminais):

```powershell
.\build\debug\bin\biblioteca-server.exe --config .\build\debug\bin\biblioteca-server.conf
```

```powershell
.\build\debug\bin\biblioteca-web.exe --servidor 127.0.0.1:7474
```

Abra <http://127.0.0.1:8080> e use "Carregar acervo de exemplo" num banco vazio.

| biblioteca-web | padrão | |
|---|---|---|
| `--servidor HOST:PORTA` | `127.0.0.1:7474` | onde está o biblioteca-server |
| `--conexoes N` | `8` | conexões com o servidor usadas em paralelo (o servidor atende procs de leitura em paralelo) |
| `--host HOST` | `127.0.0.1` | use `0.0.0.0` para acesso pela rede |
| `--porta N` | `8080` | |
| `--web DIR` | `static/` ao lado do executável | o build copia [`web/static/`](https://github.com/sergioleal/biblioteca0/tree/71782858f75a311971b9897f80bef3bfce7c0a3c/web/static) para lá |

O biblioteca-server aceita as opções de qualquer servidor moDb (`--db`,
`--port`, `--config`, `--proc-timeout-ms`, `--log`, ...; `--help` lista tudo, e
as procs). A web sobe mesmo com o servidor fora do ar: responde 503 e conecta
quando ele voltar. Leituras são repetidas numa conexão nova se o servidor
reiniciou; escritas nunca são repetidas (se o servidor caiu depois de
confirmar, repetir duplicaria).

**Relógio:** a data de hoje é a do servidor (UTC). `BIBLIOTECA_HOJE=AAAA-MM-DD`
no ambiente do servidor fixa a data (os testes usam isso para "passar 20 dias").

No Windows com MinGW, o build copia o runtime da toolchain (libstdc++, libgcc,
winpthread) para `bin/`, e os executáveis rodam fora do shell do CLion.

## API

Todas as respostas são JSON; erros vêm como `{"erro": "..."}` com status 400
(dados inválidos), 404 (não encontrado), 409 (conflito com o estado, ex.:
exemplar já emprestado) ou 503 (servidor do banco fora do ar).

| método | rota | corpo |
|---|---|---|
| GET, POST | `/api/autores` | `{nome, nacionalidade, ano_nascimento}` |
| GET, PUT, DELETE | `/api/autores/{id}` | idem |
| GET, POST | `/api/editoras` | `{nome, cidade}` |
| GET, PUT, DELETE | `/api/editoras/{id}` | idem |
| GET, POST | `/api/livros` | `{titulo, isbn, ano, editora, autores: [ids]}` |
| GET, PUT, DELETE | `/api/livros/{id}` | idem |
| GET, POST | `/api/exemplares` | `{livro, codigo, estado}` |
| GET, PUT, DELETE | `/api/exemplares/{id}` | idem |
| GET, POST | `/api/leitores` | `{nome, email, telefone, ativo}` |
| GET, PUT, DELETE | `/api/leitores/{id}` | idem |
| GET | `/api/emprestimos[?abertos=1]` | — |
| POST | `/api/emprestimos` | `{exemplar, leitor, dias}` |
| GET | `/api/emprestimos/{id}` | — |
| POST | `/api/emprestimos/{id}/devolucao` | — |
| GET | `/api/resumo` | — |
| POST | `/api/exemplo` | — (acervo de exemplo num banco vazio) |

```powershell
Invoke-RestMethod http://127.0.0.1:8080/api/livros
```

```powershell
Invoke-RestMethod -Method Post http://127.0.0.1:8080/api/emprestimos -ContentType 'application/json' -Body '{"exemplar": 12, "leitor": 40}'
```

## Procs

A API HTTP é uma casca fina: cada rota chama uma proc com o corpo JSON (mais o
`{id}` do caminho). Pelo CLI do moDb dá para chamar as mesmas procs direto:

```powershell
modb procs 127.0.0.1 7474
modb call 127.0.0.1 7474 livros.listar
modb call 127.0.0.1 7474 emprestimos.emprestar '{\"exemplar\": 12, \"leitor\": 40}'
```

| procs | modo |
|---|---|
| `{autores,editoras,livros,exemplares,leitores}.listar`, `.obter {id}` | leitura (snapshot) |
| `{autores,editoras,livros,exemplares,leitores}.criar`, `.atualizar {id, ...}`, `.remover {id}` | escrita |
| `emprestimos.listar {abertos?}`, `emprestimos.obter {id}` | leitura |
| `emprestimos.emprestar {exemplar, leitor, dias?}`, `emprestimos.devolver {id}` | escrita |
| `resumo` | leitura |
| `exemplo.carregar` | escrita (uma transação: o exemplo entra inteiro ou nada) |

Erros de regra viram status HTTP: `invalid_argument` → 400, `record_not_found`
→ 404, `conflict` → 409, `operation_timeout` → 504, servidor fora do ar → 503.

## Como a aplicação usa o moDb

- [`modulo/modelo.hpp`](https://github.com/sergioleal/biblioteca0/blob/71782858f75a311971b9897f80bef3bfce7c0a3c/modulo/modelo.hpp): as structs e seus `BindingBuilder` — cada campo tem um id
  estável (`field<N>`) que nunca é reusado.
- [`modulo/biblioteca_procs.cpp`](https://github.com/sergioleal/biblioteca0/blob/71782858f75a311971b9897f80bef3bfce7c0a3c/modulo/biblioteca_procs.cpp): o módulo (`modb::server::ModuleBuilder`): os
  seis tipos, os índices dos campos únicos e as procs. As regras ficam numa
  classe `Acervo` sobre o `Context` da chamada (`read`, `all`, `find` pelo
  índice, `create`/`update`/`set`/`remove`, coleções persistentes).
- **Uma chamada por vez no banco.** O servidor do moDb serializa as chamadas; a
  web não precisa de mutex para o banco (só para a sua conexão).
- **Limite conhecido do motor:** trocar os autores de um livro grava uma lista
  nova e apaga a antiga, mas o `BlobStore` do moDb ainda não reaproveita o espaço
  de blobs apagados (sem lista livre). Irrelevante no volume de uma biblioteca.

## Estrutura

```
CMakeLists.txt              moDb (add_subdirectory) + cpp-httplib (FetchContent)
modulo/modelo.hpp           entidades e bindings
modulo/biblioteca_procs.*   as regras, como procs (o módulo do servidor)
servidor/                   biblioteca-server: modb_add_server + configuração de exemplo
web/main.cpp, gateway.*     biblioteca-web: rotas HTTP -> procs, reconexão
web/static/                 index.html, app.js, style.css
tests/procs_test.cpp        as regras pela rede, servidor em outro processo (60 verificações)
tests/web_test.cpp          web + servidor, com o servidor derrubado no meio (20 verificações)
tests/carga_test.cpp        8 leitores + 2 escritores pelo HTTP, com os invariantes do domínio
```
