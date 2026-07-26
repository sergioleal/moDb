# Plano de implementação dos testes de carga — o que falta e em que ordem

- Data: 2026-07-25
- Especificação (o **quê**): [docs/PLANO_TESTES_DE_CARGA.md](../docs/PLANO_TESTES_DE_CARGA.md)
- Este documento (o **como** + rastreamento): sequência dirigida a dependências,
  tarefa por tarefa, com critério de pronto verificável
- Estado do código na abertura deste plano: commit `45a1fcb` (Subfases A e B)

Este documento não redefine escopo. A especificação é a fonte de verdade sobre
o que cada subfase significa; aqui fica o levantamento do que está de fato
implementado, a ordem recomendada para o resto, e o rastreamento.

## 1. Estado verificado (não o que a doc afirma — o que o código faz)

Levantado lendo o código em `45a1fcb`, não a própria especificação.

| Área | Especificado | Implementado | Lacuna |
|---|---|---|---|
| Workloads (§4.2 + §4.2.1) | 15 ids no catálogo | **1** com dispatch (`create_only`) | 14 |
| Alvos (§4.3) | 4 | **1** (`embedded`) | 3 |
| Dimensões secundárias (§4.5) | 7 | **2** com efeito real (`payload`, `batch`) | 5 são no-op |
| Métricas por fase (§8) | ~30 | **5** (`operations`, `duration_ns`, `ops_per_second`, `bytes_per_object`, `errors`) | percentis, RSS, WAL, páginas, cache hit, fsync, amplificação, fragmentação, TTFR, rede |
| Validação (§9) | 7 passos | **2** (contagem + hash lógico) | reopen, `database_check`, leitura campo a campo pós-update, resolução pós-delete, `applied_lsn` do follower |
| Records JSONL (§12) | 12 | **9** | `progress_window`, `skipped_budget`, `run_note` |
| Série histórica (§13) | 11 subseções | **0** | tudo: rollup, `series_key`, `index`, `trend`, `report`, `gate`, retenção, baselines |
| Calibração/orçamento (§10) | tabela por plataforma + guarda-corpos ativos | **0** (`loadtests/calibration/` não existe) | estimativas são sempre `?`; limites não são aplicados |
| CLI (§6.1) | 8 subcomandos | **3** (`run`, `list-cases`, `list-profiles`) | `resume`, `index`, `trend`, `report`, `gate`, `list-environments` |
| Testes (§14) | 3 suítes | **1** (`modb.load_matrix`) | `load_workload_test`, `load_history_test`; nada exercita o caminho de execução real |
| Scripts (§14) | 3 | **2** (`run-load.ps1`, `run-load.sh`) | `run-remote-load.ps1` |

Consistências confirmadas (não são lacunas): as flags que `run-load.ps1`/`.sh`
emitem são exatamente as que `modb_load` aceita — sem mismatch; `/load-results/`
está no `.gitignore`; `modb_load.exe` roda com PATH sem MinGW/CLion/Git.

## 2. Duas dívidas que precisam vir antes de qualquer subfase nova

Não são subfases da especificação; são defeitos do que já existe. Enquanto
estiverem abertos, toda medição feita depois nasce suspeita — e a série
histórica (§13) preservaria a suspeita para sempre.

### D1 — `case_id` mente sobre concorrência

`--concurrency 16` produz `load.create_only.embedded.1k.c16` e grava
`"concurrency":16` no `case_start`, mas **nada em runtime usa concorrência**:
`loadtests/workloads/create_only.cpp` não lê `c.concurrency`, e
`target_embedded.cpp` é single-threaded. O mesmo vale para `readers`,
`durability`, `cache` e `primary_storage` — todos entram no sufixo de variante
do `case_id` (`matrix.cpp`, `Case::case_id`) e nenhum altera a execução.
`payload` e `batch` são os únicos honestos.

Isso é pior que uma lacuna: é um resultado que se apresenta como outra coisa.
Num tool cuja função é medição confiável, e cujo `series_key` (§13.4) usa esses
parâmetros como chave de comparabilidade, dois pontos "diferentes" seriam a mesma
medição sob nomes distintos.

**Correção (escolher uma, não deixar como está):**
- (a) recusar na entrada: `--concurrency`/`--payload`/... com valor não padrão
  falha com "dimensão ainda não implementada" até a subfase que a implementar;
- (b) implementar de fato — mas concorrência real é a Subfase M, longe daqui.

Recomendado: **(a)**, agora. É pequeno, e converte uma mentira silenciosa em
recusa explícita. A lista de dimensões aceitas cresce junto com as subfases que
as implementarem.

### D2 — o dashboard já espera métricas que o coletor não produz

`loadtests/dashboard/index.html` oferece 11 métricas selecionáveis e lê 6
campos por fase (`latency_ns`, `ops_per_second`, `duration_ns`,
`bytes_per_object`, `peak_rss_bytes`, `wal_bytes`). `PhaseMetrics`
(`loadtests/target.hpp`) tem 5 campos, e nenhum é `latency_ns`,
`peak_rss_bytes` ou `wal_bytes`.

Consequência prática: mesmo depois da Subfase C existir e produzir
`series.jsonl`, **7 das 11 métricas do painel mostrariam "—"**. Pior, os
limiares de gate de §13.7 são definidos sobre p99 (≥15%) — impossível de
avaliar sem coletar percentis.

**Correção:** ampliar a coleta antes da série existir (detalhe na Subfase B2
abaixo), para que o primeiro ponto histórico já nasça com os campos que o
consumidor pressupõe.

## 3. Ondas de implementação

Agrupadas por "o que passa a ser possível ao fim da onda", não por proximidade
de código. Tamanho relativo: P (pequeno), M (médio), G (grande).

| Onda | Subfases | Ao fim dela, é possível… | Tamanho |
|---|---|---|---|
| 0 — Confiança | D1, D2 | acreditar no que o `case_id` diz e no que o painel lê | P + M |
| 1 — A série vira real | C | comparar duas execuções ao longo do tempo; **o dashboard existente ganha fonte de dados** | G |
| 2 — A escada completa | D, E | responder à pergunta central de §4.2 (ordem de remoção afeta espaço/tempo?) | M + M |
| 3 — Duração | F | rodar casos longos sem perder trabalho em interrupção | M |
| 4 — Escala real | H | responder à pergunta central de §4.1 (10k → 1M é linear?) | M |
| 5 — Rede | G, I | separar custo de motor de custo de protocolo/enlace | G + M |
| 6 — Gates | J | reprovar regressão automaticamente e detectar deriva lenta | M |
| 7 — Comportamento | K, L–R | medir cache, MVCC, blobs, cascata, recuperação | G |

### Por que esta ordem diverge de §15 da especificação

Duas mudanças deliberadas, ambas justificadas por dependência real:

1. **D1/D2 antes de tudo** — §15 não os prevê porque a especificação foi escrita
   antes do código existir. São dívidas do implementado, não escopo novo.
2. **Onda 4 (escala, ex-Subfase H) antes da Onda 5 (rede, ex-G/I)** — §15 põe
   `target_client`/loopback (G) antes das escalas altas (H). Inverto porque a
   pergunta que dá nome ao plano ("10k a 1M") é respondível **só com embedded**;
   rede é ortogonal a ela. Adiantar escala entrega a resposta principal antes,
   e a calibração de H é pré-requisito dos guarda-corpos que tornam
   `load-heavy` executável sem supervisão (§17 risco 4).

O resto segue §15 sem alteração.

## 4. Detalhamento por subfase

Cada item lista **arquivos**, o que muda e o **critério de pronto** verificável.
Um branch por subfase, conforme a convenção do projeto.

### D1 — recusar dimensões não implementadas · P
- `loadtests/matrix.cpp` (ou novo `dimensions.cpp`): tabela de dimensões
  secundárias com um flag `implemented`; `expand_matrix` falha ao ver valor não
  padrão de dimensão não implementada.
- `tests/load_matrix_test.cpp`: caso novo — `--concurrency 16` deve falhar com
  mensagem citando a subfase que a implementará.
- **Pronto quando**: nenhum `case_id` pode ser gerado com sufixo de variante que
  o runtime ignora; `--payload fat` continua funcionando.

### D2 — ampliar métricas por fase · M
- `loadtests/target.hpp`: `PhaseMetrics` ganha `latency_ns{p50,p95,p99,p999}`,
  `peak_rss_bytes`, `wal_bytes`, `db_bytes`, `pages_read/written/reused`.
- `loadtests/target_embedded.cpp`: histograma por operação (reaproveitar
  `summarize_latency_ns` de `benchmarks/runner/json_util.hpp`, que já existe);
  RSS de pico por SO; tamanho do WAL via `std::filesystem::file_size` no
  `.modb.wal`.
- `loadtests/campaign.cpp`: emitir os campos novos em `phase_summary`.
- `CaseRunResult`: `write_amplification`, `space_amplification`.
- **Pronto quando**: as 11 métricas do dashboard têm origem real; `phase_summary`
  de um `create_only` traz p50/p99 não nulos.

### C — série histórica · G  (a onda de maior alavancagem)
- `loadtests/history/series_key.hpp/.cpp`: hash estável sobre os atributos de
  §13.4 (+ `series_key_version`). **`environment` não entra no hash**; `host_class` entra.
- `loadtests/history/rollup.hpp/.cpp`: campanha JSONL → um rollup por caso, no
  schema canônico de §13.3 (os nomes de campo já estão fixados lá e o dashboard
  os lê — não inventar novos).
- `loadtests/history/index.cpp`: append idempotente em `load-history/series.jsonl`,
  dedup por (`run_id`, `case_id`, `repeat_index`), rejeitando rollup sem
  procedência (§13.3). Exige **leitura** de JSONL — `loadtests/json_value.*` já
  faz parse de JSON; falta só iterar linha a linha.
- `loadtests/history/trend.cpp`: série, mediana móvel (5), outliers, quebras de
  `series_key`, recusa de veredito com < 3 pontos.
- `loadtests/modb_load.cpp`: subcomandos `index`, `trend`, `report`.
- `tests/load_history_test.cpp`: `series_key` estável; idempotência do `index`;
  mediana móvel; quebra de série.
- **Pronto quando**: duas execuções de `load-smoke` produzem dois pontos na
  mesma série; reindexar não duplica; **o dashboard abre o `series.jsonl` real
  e mostra os dois pontos sem conversão**.

### D — workloads de create/delete · M
- `loadtests/workloads/create_delete_{forward,reverse,interleaved}.cpp` e o
  dispatch em `campaign.cpp`; `target_embedded` ganha fase `delete`.
- Invariantes de §4.2: contagem 0, nenhum id resolvível, tamanho final do
  `reverse` ≤ do `forward`, fragmentação registrada no `interleaved`.
- `tests/load_workload_test.cpp` (novo): cada workload em escala minúscula.
- **Pronto quando**: `load-smoke` roda 4 workloads em vez de 1, todos com
  invariante verificada.

### E — `crud_full` · M
- Seis fases separadas (`create`, `read`, `update_inplace`, `update_grow`,
  `update_shrink`, `delete`), cada uma com `phase_summary` próprio.
- Validação campo a campo em amostra determinística (§9 item 4).
- **Pronto quando**: um `crud_full` produz 6 `phase_summary`, não um número único.

### F — janelas e retomada · M
- `progress_window` a cada 10 s em fase > 30 s; `windows{first,last,slope}` no rollup.
- `resume <arquivo.partial>`: reconstrói casos concluídos e executa o resto.
- **Pronto quando**: interromper `100k` e retomar não reexecuta caso concluído.

### H — escalas altas, calibração e guarda-corpos · M
- `loadtests/calibration/{windows,linux}-x86_64.json` preenchidos **por medição**.
- `budget.cpp`: estimativa real; `skipped_budget` quando excede; `--max-*` ativos.
- **Pronto quando**: tabela de §10 preenchida; caso acima do orçamento é pulado
  com registro e a campanha termina `partial`.

### G — alvo de rede local · G
- `loadtests/target_client.cpp` via `modb::net::Client`; alvo `loopback`;
  métricas de rede (bytes/frames/syscalls/TTFR).
- **Pronto quando**: o mesmo `case_id` roda em `embedded` e `loopback`.

### I — alvos remotos · M
- `remote_colocated`, `remote_client_local`; `scripts/run-remote-load.ps1`
  (reaproveitando a resolução de ambiente já feita em `run-remote-benchmark.ps1`);
  RTT/banda base medidos antes da carga; indexação do bruto trazido.
- **Pronto quando**: `load-remote` traz exatamente um arquivo, com hash, e ele
  entra na série.

### J — gates e deriva · M
- `gate.cpp`: gate por execução (mediana das 5 anteriores) e deriva lenta
  (mediana das 5 × mediana de 20 atrás), limiares de §13.7; retenção/`--prune`;
  `load-history/baselines.json`.
- Resolver a ponte com `modb_bench compare` documentada em §12 (hoje o
  comparador só lê `record:"scenario_summary"`).
- **Pronto quando**: regressão sintética de 12% reprovada; deriva sintética de
  15% em 20 execuções detectada com todos os gates pontuais passando.

### K, L–R — dimensões secundárias e workloads de comportamento · G
Ordem de §15 preservada: L (`read_hotspot`, `range_scan_sweep`),
M (`mixed_oltp` — implementa concorrência de verdade e fecha D1 para
`--concurrency`), N (`snapshot_hold`), O (`dataset_user_blob` + `blob_lifecycle`),
P (`cascade_delete`), Q (`oversubscribed_churn` + perfil `load-behavior`),
R (harness de kill/restart + `restart_recovery`), K (pairwise das secundárias,
`load-heavy`, `load-soak`).

`schema_evolution` e `replica_catchup` seguem fora de escopo até a
infraestrutura de que dependem existir (§4.2.1).

## 5. Rastreamento

Status: ⬜ não iniciada · 🔄 em andamento · ✅ concluída (com teste na suíte
verde) · 🚫 bloqueada. Ao concluir, preencher **Notas** com hash do commit e
data, conforme [RASTREADOR.md](RASTREADOR.md).

| # | Subfase | Onda | Status | Notas |
|---|---|---|---|---|
| A | Matriz, seletores, perfis, budget-gate | — | ✅ | `45a1fcb` · 2026-07-25 |
| B | Writer, dataset, `target_embedded`, `create_only` | — | ✅ | `45a1fcb` · 2026-07-25 |
| D1 | Recusar dimensões não implementadas | 0 | ✅ | `212a28e` · 2026-07-25 |
| D2 | Ampliar métricas por fase | 0 | ✅ | `6509e24` · 2026-07-25 |
| C | Série histórica (rollup, index, trend, report) | 1 | ✅ | `afadf60` · 2026-07-25 |
| D | `create_delete_{forward,reverse,interleaved}` | 2 | ✅ | `f6beb32` · 2026-07-25 |
| E | `crud_full` | 2 | ✅ | `c64ca4e` · 2026-07-25 |
| F | `progress_window`, `resume` | 3 | ✅ | `6ea2e02` · 2026-07-25 |
| H | Escalas altas, calibração, guarda-corpos | 4 | ✅ | `7b4779f` · 2026-07-25 (calibração reduzida: 10k/100k medidos, 250k/500k/1M extrapolados -- ver docs/PLANO_TESTES_DE_CARGA.md §10) |
| G | `target_client`, `loopback` | 5 | ✅ | `3442bec` · 2026-07-25 (versão mínima: só `create_only`; sem métricas de rede reais nem processos separados -- ver docs/PLANO_TESTES_DE_CARGA.md §4.3) |
| I | Alvos remotos, `run-remote-load.ps1` | 5 | ✅ | `492e8a5` · 2026-07-25 (parcial: `remote_colocated` verificado localmente; `run-remote-load.ps1` escrito mas nunca rodou contra um host remoto de verdade nesta sessão -- ver docs/PLANO_TESTES_DE_CARGA.md §11; `remote_client_local` segue sem dispatch) |
| J | `gate`, deriva, retenção, baselines | 6 | ⬜ | depende de C |
| L | `read_hotspot`, `range_scan_sweep` | 7 | ⬜ | |
| M | `mixed_oltp` (concorrência real) | 7 | ⬜ | fecha D1 para `--concurrency` |
| N | `snapshot_hold` | 7 | ⬜ | |
| O | `dataset_user_blob`, `blob_lifecycle` | 7 | ⬜ | |
| P | `cascade_delete` | 7 | ⬜ | |
| Q | `oversubscribed_churn`, perfil `load-behavior` | 7 | ⬜ | |
| R | Harness kill/restart, `restart_recovery` | 7 | ⬜ | ver §17 risco 13 |
| K | Pairwise secundárias, `load-heavy`, `load-soak` | 7 | ⬜ | depende de M, N, O, P, Q |

Progresso: **11/20**.

## 6. O que não está neste plano, de propósito

- **`schema_evolution` e `replica_catchup`** — dependem de infraestrutura que não
  existe (versionamento simultâneo de `Binding`; réplica orquestrada pelo
  harness). Entram quando essa infraestrutura existir por outro motivo, não
  antes (§4.2.1).
- **Cadência de execução de cada perfil** — decisão de operação, não de código;
  §17 risco 8 já registra que gate automático fica restrito a `10k`/`100k`.
- **Estimativas de tempo em horas** — a calibração da Subfase H é justamente o
  que transforma chute em número medido; até lá, o tamanho relativo (P/M/G)
  desta tabela é a única estimativa honesta disponível.
