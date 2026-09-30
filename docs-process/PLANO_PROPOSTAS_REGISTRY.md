# Plano — propostas do backend do registry

Criado em 2026-09-30. Origem: `PROPOSTAS_RING0.md`, escrito durante a
construção do backend do registry sobre o Ring0 (base: moDb `568d959`, mesmo
protocolo da `v0.1.1`). As nove propostas foram conferidas no código. O
diagnóstico de todas se confirmou; as mudanças de forma estão anotadas em cada
tarefa.

| # | Proposta | Tarefas |
|---|---|---|
| 1 | Identidade do usuário final | R9 |
| 2 | Erros estruturados | R10 |
| 3 | Tabela estável de `ErrorCode` | R1 |
| 4 | Índices: unicidade, prefixo, multivalorado, texto | R4, R13, R16, R17 |
| 5 | Tipos de campo: instante, opcional, lista, `Value` | R14, R15, R16 |
| 6 | Configuração dos módulos pelo `.conf` | R2 |
| 7 | Parada limpa no Windows e socket local | R3 |
| 8 | Cliente Node de referência | R6 |
| 9 | Validar conexão ociosa | R11 |

## Regras

- Cada tarefa deixa todos os testes passando (preset `debug` no Windows e
  build Linux no WSL). As que mexem em threads do engine ou do proxy rodam
  também no TSan (preset `tsan`, WSL).
- Cada etapa termina numa release por tag. O workflow `release-sdk` gera,
  testa e publica o SDK; `project(VERSION)` sobe junto com a tag.
- **P0 e P1 não mudam o protocolo nem o formato do arquivo.** O registry só
  precisa mudar quando for adotar o que é novo.
- **P2 é um minor novo do protocolo (3), num lançamento só.** Clientes de minor
  2 continuam funcionando sem mudança (ADR-015).
- **P3 muda o formato do arquivo.** Cada mudança tem ADR própria, uma entrada
  no `FORMATO_DE_ARQUIVO.md` e um teste de abertura de um arquivo da versão
  anterior.
- Medições de desempenho só na máquina dedicada (droplet).

## Tarefas

### P0 — sem mudar o protocolo *(release `v0.1.2`)*

#### R1 — `ErrorCode` estável *(pequeno)* ✅

- [x] 1.1 *(75 códigos, 0 a 74)* Valores explícitos em todo o enum de `include/modb/error.hpp`, iguais
  aos de hoje (`invalid_identifier = 0`, `invalid_argument = 1`, …,
  `permission_denied = 74`).
- [x] 1.2 *(`modb.error_code_values`: `static_assert` por valor e tabela contínua; um código novo sem documentação é pego pelo `modb.error_codes`, porque o C++ não enumera um enum)* Teste que fixa cada valor numa tabela (`tests/error_code_test.cpp`) e
  falha se algum mudar ou se aparecer um código sem entrada na tabela.
- [x] 1.3 Regra no `docs/COMPATIBILIDADE.md`: código novo entra no fim, e
  nenhum número é reusado nem renumerado.
- [x] 1.4 *(§3 com os 14 códigos que um cliente de procs encontra e a ação; Apêndice A com os 75, gerado dos comentários do header)* Tabela completa em `docs/PROTOCOLO_CLIENTES.md` §3, com a coluna "o
  que o cliente faz": repetir (só leitura), reconectar, mostrar ao usuário ou
  tratar como defeito.
- [x] 1.5 *(`modb_client.ERROR_CODES`; `modb.error_codes` confere header × Apêndice A × §3 × cliente)* Constantes do cliente Python geradas ou conferidas contra a tabela
  (um teste compara as duas).

#### R2 — Configurações declaradas pelos módulos *(pequeno)* ✅

- [x] 2.1 `ModuleBuilder::setting(nome, padrão, descrição, validador?)`.
- [x] 2.2 *(`resolve_module_settings` roda no começo do `start`, antes de abrir o banco)* `server_host`: aceita `<módulo>.<nome> = valor` no `.conf` e
  `--<módulo>.<nome> valor` na linha de comando. Chave desconhecida ou valor
  inválido falha na subida, com a mesma mensagem de hoje para chaves do engine.
- [x] 2.3 *(nome não declarado lança: vira `internal_error`, com rollback)* Na proc, `c.setting("nome")` devolve o texto já validado.
- [x] 2.4 *(`sys.settings` nova, sem o valor em uso: pode haver configuração que não é de todos os clientes)* `--help` e `sys.procs` (ou um `sys.settings`) listam nomes, padrões e
  descrições.
- [x] 2.5 Teste no `server_host_test`: padrão, valor pelo `.conf`, valor pela
  linha de comando (que vence o `.conf`), valor inválido derruba a subida.
- [x] 2.6 *(`notas.max_texto`)* `notas` ganha uma configuração de exemplo; `OPERACAO.md` e
  `SDK.md` documentam.

#### R3 — Parada limpa sem sinal e Windows no `OPERACAO.md` *(pequeno)* ✅

- [x] 3.1 *(`on\|off`, como `--tcp`: `--stop-on-stdin-eof on`; `include/modb/net/stdin_eof.hpp`)* `--stop-on-stdin-eof` no `server_host` e no `modb-proxy`: quando a
  entrada padrão fecha, chama `request_stop` (mesma parada do `SIGTERM`, com
  checkpoint).
- [x] 3.2 *(`modb.stdin_eof`, Python como o `modb.python_client`; confere `stopped`, saída 0 e a nota confirmada na reabertura, e que sem a opção o fim da entrada não para nada)* Teste nas duas plataformas: o supervisor fecha o pipe, o processo sai
  com 0 e o arquivo abre sem recuperação.
- [x] 3.3 `OPERACAO.md`, subseção "Windows": o que para limpo (NSSM, Ctrl+C no
  console, `--stop-on-stdin-eof`), o que não para (`TerminateProcess`,
  `child.kill` do Node) e a restrição do socket local sob `%LOCALAPPDATA%`
  (inclui o `%TEMP%` padrão). O que hoje está só na ADR-028 passa para lá.

#### R4 — Faixa e prefixo nas procs *(pequeno)* ✅

- [x] 4.1 *(`Database::indexed_object_ids_between` para a proc de escrita; a de leitura usa `between` no snapshot)* `Context::range<T>(campo, lo, hi)` e `Context::prefix<T>(campo,
  texto)`, pelo índice e no snapshot da chamada (como o `find`). O prefixo é
  `between(texto, texto + "\xFF")`: o índice guarda texto em bytes crus, e
  UTF-8 nunca tem o byte 0xFF.
- [x] 4.2 *(`modb.proc_range`)* Teste: prefixo com acentos, prefixo vazio, campo sem índice (erro
  claro, como o `find`).
- [x] 4.3 *(documentado em `module.hpp` e nas armadilhas da referência; esconder o `database()` quebraria procs que usam `blobs()` por ele)* Fechar ou documentar o risco de `c.database().query<T>()`: numa proc
  de leitura, ele lê fora do snapshot da chamada. Decidir entre esconder o
  `database()` das procs ou avisar no `module.hpp` e na referência.
- [x] 4.4 *(§3.10)* `docs/reference/queries-indexes.md`: seção de procs com `find`,
  `range` e `prefix`.

#### R5 — Release `v0.1.2` *(pequeno)* ✅

- [x] 5.1 `VERSION 0.1.2`, `SDK.md` (tabela de versões), links do
  `pacote-dev-node` para `v0.1.2`.
- [x] 5.2 Tag `v0.1.2`; o workflow publica a release; conferir o download.

### P1 — cliente Node *(release `v0.1.3`)*

#### R6 — `clients/node/` *(médio)* ✅

- [x] 6.1 *(de `agentikalreg/packages/modb-client`, só lido; `package.json` e `tsconfig` sem o monorepo; Node ≥ 22.18 roda os `.ts` direto)* Trazer o cliente TypeScript para `clients/node/`: `Value` v1,
  conexão com `Hello`, `Authenticate` e várias chamadas em voo, e pool.
- [x] 6.2 *(`modb.node_value` e `modb.node_client`, ligados só com Node ≥ 22.18; a queda é do proxy, parado pelo fim da entrada padrão e religado na mesma porta. No Linux, roda no `sdk-smoke.sh` da release, com `setup-node`)* Teste no CTest, como o `modb.python_client`, contra o
  `notas-server` atrás do `modb-proxy`: recusa sem token (73), erros de proc
  (1, 30, 48, 70), queda do servidor no meio (leitura repetida, escrita não).
- [x] 6.3 *(o `modb.error_codes` confere também o `errors.ts`; entraram 44 e 50)* Constantes de `ErrorCode` conferidas contra a tabela da R1.
- [x] 6.4 `PROTOCOLO_CLIENTES.md` e o pacote passam a citar o cliente Node.
- [ ] 6.5 *(opcional)* Publicar no npm.

### P2 — protocolo minor 3 *(release `v0.2.0`)*

#### R7 — ADR-029 *(pequeno)* ✅

- [x] 7.1 *(`docs/decisions/ADR-029-delegacao-detalhe-e-idempotencia.md`)* Decidir e registrar: delegação por chamada (R9), `detail` nos erros
  (R10), validação de conexão ou idempotência (R11, decisão D3), e o formato
  de cada campo novo no fio.
- [x] 7.2 *(o proxy negocia `min(cliente, proxy, engine)`)* Negociação: o servidor só manda os campos novos com minor ≥ 3; um
  cliente de minor 2 continua lendo `code` e `message`.

#### R8 — Minor 3 no codec *(médio)* ✅

- [x] 8.1 *(campos no fim, gravados só quando preenchidos: sem eles, os bytes do minor 2)* `Hello`/`HelloOk` negociam minor 3 no engine, no link e no proxy.
- [x] 8.2 *(`modb.protocol_minor3`: ida e volta, frame do minor 2, bits desconhecidos, limites, negociação; `modb.minor3_e2e` com um cliente de minor 2 feito à mão)* Testes de compatibilidade: cliente de minor 2 contra servidor de
  minor 3, e o contrário (`compatibility_test`, `protocol_test`).

#### R9 — Delegação: em nome de quem *(grande)* ✅

- [x] 9.1 Campo `acting_as` + atributos em **`OpCall`, `Query` e nas mensagens
  de facade**. Uma consulta em stream também é feita em nome de alguém; só no
  `OpCall` ela voltaria a ser "do gateway".
- [x] 9.2 *(D2: role `delegate`; a regra fica no proxy, fora da cadeia de políticas; o engine direto recusa)* Proxy: só um principal com a role de delegar (nome na ADR-029) pode
  preencher o campo. Os outros recebem `permission_denied` sem chegar ao
  engine.
- [x] 9.3 *(limites por delegado e `max_delegated_calls_per_second` por principal; a allowlist segue pelo principal e roles)* Política: allowlist e rate limit veem o par (principal, delegado). O
  limite por delegado **soma** a um teto por principal, para um gateway
  comprometido não contornar o limite inventando usuários.
- [x] 9.4 *(também na recusa: a auditoria mostra em nome de quem tentaram falar)* Auditoria do proxy e log de chamadas do engine: `by <principal> as
  <delegado>`.
- [x] 9.5 `Caller` ganha `acting_as`, `acting_attributes` e `subject()`.
- [x] 9.6 *(`modb.minor3_e2e`; TSan não rodado: nenhuma thread nova, só dados por pedido)* Testes: delegação permitida, recusada, auditada e com limite; proc
  lendo `subject()`; TSan.

#### R10 — Erros com `detail` *(médio)* ✅

- [x] 10.1 `detail` (um `Value`) no `OpResult` de erro, só com minor ≥ 3.
- [x] 10.2 *(`Context::fail(error, Value)`; o detalhe vive no `ExecutionContext` e volta pelo `CallExtras`)* O `detail` fica **na camada das procs**, não no `modb::Error`. O
  `Error` é o tipo base do motor, e pôr um `ops::Value` nele faria a camada
  mais baixa depender de `ops`. Exemplo: `invalid(...).with_detail(...)` em
  `module.hpp`, carregado pelo `OperationRegistry` até a resposta.
- [x] 10.3 *(o log grava `reason <r>`; o `notas` manda `texto_vazio`/`texto_repetido`)* Convenção documentada: `reason` (`[a-z_]+`, estável) e `field`
  (caminho com `.`). O log de chamadas grava só o `reason`.
- [x] 10.4 Testes: proc com e sem `detail`, cliente de minor 2 recebendo só
  `code` e `message`.

#### R11 — Conexão ociosa *(médio; D3: chave de idempotência)* ✅

- [ ] 11.1 *(não feita: D3 escolheu só a chave)* Opção A, `Ping`/`Pong`: o proxy responde sem ir ao engine. Estreita
  a janela, mas não permite repetir uma escrita.
- [x] 11.2 *(`sys.Idempotency` gravado na transação da escrita, por principal; resultado acima de 4 KB guarda só a chave e a repetição volta `conflict` com `reason = idempotent_result_too_large`; até 16 vencidos apagados por escrita)* Opção B, chave de idempotência por `OpCall`: o engine guarda o
  resultado das últimas chamadas por chave e, numa repetição, devolve o
  resultado confirmado em vez de executar de novo. Resolve a causa: o pool
  passa a poder repetir escritas.
- [x] 11.3 *(a queda entre commit e resposta é coberta pela reabertura do servidor com a mesma chave)* Testes da opção escolhida, incluindo queda do servidor entre o
  commit e a resposta.

#### R12 — Clientes, documentação e release *(médio)* ✅

- [x] 12.1 *(C++ `net::CallOptions`; Python `call(..., acting_as=, idempotency_key=)` e `ModbError.detail`; Node `{ actingAs, idempotencyKey }`, `ModbError.detail`, e o pool repete escritas com chave)* Clientes Python e Node com minor 3: `acting_as`, `detail` e a
  opção da R11.
- [x] 12.2 `PROTOCOLO_CLIENTES.md`, `networking-protocol.md`, `OPERACAO.md`,
  `SDK.md` e o pacote.
- [x] 12.3 `VERSION 0.2.0`, tag `v0.2.0`, conferir a release.

### P3 — modelo de dados *(muda o formato do arquivo)*

#### R13 — Índice único *(médio)*

- [ ] 13.1 `ModuleBuilder::unique<T>(campo)`: `create` e `update` recusam um
  segundo valor igual com `conflict` e, com a R10, `detail{reason:
  "unique_violation", field}`.
- [ ] 13.2 Decisão D4: valores vazios (`""`, ref 0) ficam fora da restrição,
  como o `NULL` do SQL.
- [ ] 13.3 A marca de unicidade no catálogo do índice; um banco antigo abre
  como índice comum.
- [ ] 13.4 Criar um índice único sobre dados que já têm duplicatas falha com a
  lista das duplicatas.

#### R14 — `Value` embutido *(pequeno; sem mudar o formato)* ✅

- [x] 14.1 *(ponto de extensão genérico `object::bytes_codec<M>`, para a camada de objetos não depender de `ops`; `ops::Value` traz o seu)* Utilitário de binding que grava um `ops::Value` codificado num
  campo de bytes e devolve o `Value` na leitura. Resolve `tags`, `tools` e
  `inputSchema` sem JSON em texto e sem parse.
- [x] 14.2 *(`modb.stored_value`. Achado: o limite não é do campo, é do objeto inteiro, que precisa caber numa página do heap, cerca de 8 KB; o erro é `record_too_large` (28), não `value_too_large`. Documentado na referência do modelo de objetos)* Teste de ida e volta e de limite de tamanho.

#### R15 — Instante e opcional *(grande)*

- [ ] 15.1 ADR: unidade do instante (proposta: µs UTC, int64) e como ele chega
  ao cliente. Se for uma tag nova no `Value`, isso é protocolo (entra num
  minor); se for inteiro com convenção documentada, não é.
- [ ] 15.2 Tipo `timestamp` no catálogo, no codec e no índice;
  `Context::now()`.
- [ ] 15.3 `std::optional<T>` nos bindings, gravando ausência de verdade;
  `null` no `Value`.
- [ ] 15.4 Evolução de esquema: campo novo opcional num tipo já persistido.

#### R16 — Lista embutida e índice multivalorado *(grande)*

- [ ] 16.1 `std::vector<std::string>` e `std::vector<std::int64_t>` no próprio
  objeto, com limite de elementos e de bytes.
- [ ] 16.2 Índice multivalorado: uma entrada por elemento. Consulta "contém
  todos" (o filtro por tag).
- [ ] 16.3 Medição na máquina dedicada: busca por tag com índice contra a
  varredura atual.

#### R17 — Índice de texto *(fora por ora)*

- [ ] 17.1 Só depois da R16, e se a aplicação precisar. Proposta: o motor
  expõe o índice multivalorado, e tokenização e ranking ficam na aplicação.

## Decisões pendentes

| # | Pergunta | Bloqueia |
|---|---|---|
| D1 | ~~O cliente TypeScript do registry pode ser trazido para o moDb?~~ Sim (pedido da P1, 2026-09-30). | R6 ✅ |
| D2 | ~~Nome da role e alcance da delegação.~~ `delegate`; vale para `OpCall`, `Query` e facades (2026-09-30). | R9 ✅ |
| D3 | ~~`Ping`/`Pong` ou chave de idempotência?~~ Só a chave (2026-09-30). | R11 ✅ |
| D4 | Valores vazios ficam fora do índice único? | R13 |
| D5 | O instante é uma tag nova do `Value` (protocolo) ou um inteiro com convenção? | R15 |

## Ordem

R1 → R2 → R3 → R4 → R5 (P0; pode começar já). R6 quando D1 estiver resolvida.
R7 → R8 → R9 → R10 → R11 → R12 (P2). R14 pode entrar a qualquer momento; R13,
R15 e R16 depois da P2, porque o `detail` do `unique` depende da R10.

## Registro

| Data | Tarefa | Commit | Nota |
|---|---|---|---|
| 2026-09-30 | R1 | 6252983 | `ErrorCode` com valores explícitos, fixados no C++ e conferidos contra a documentação e o cliente Python |
| 2026-09-30 | R2 | a1cca2f | Configurações de módulo validadas na subida; `sys.settings` |
| 2026-09-30 | R3 | 68ca1d7 | `--stop-on-stdin-eof on` no servidor e no proxy; seção Windows no `OPERACAO.md` |
| 2026-09-30 | R4 | 1f3c838 | `Context::range` e `Context::prefix`; aviso sobre `c.database()` fora do snapshot |
| 2026-09-30 | R5 | ccce319 | `v0.1.2`: P0 concluída, sem mudar protocolo nem formato |
| 2026-09-30 | R6 | 3282796 | `v0.1.3`: cliente Node em `clients/node/`, no CTest e no teste da release |
| 2026-09-30 | R14 | 1ece70a | Campo `ops::Value` persistido via `bytes_codec`; objeto limitado a uma página |
| 2026-09-30 | R7–R8 | 4c6a2b4 | ADR-029 e o codec do minor 3 |
| 2026-09-30 | R9–R11 | bcd80a0 | Delegação, `detail` e chave de idempotência, de ponta a ponta |
| 2026-09-30 | R12 | (release) | `v0.2.0`: clientes Python e Node no minor 3, documentação |
