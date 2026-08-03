# Profiler in-process — análise e desenho

- Estado: **passos 1–9 implementados e medidos** (ver §9, Andamento). Falta o
  passo 10 (campanha 4:1 vs 10:1 em RelWithDebInfo) e o 11 (histograma, opcional)
- Versão: 2
- Data de abertura: 2026-07-30
- Data da execução: 2026-08-02
- Branch: `subphase/H-calibration-250k`
- Plano de caça a gargalos (a pergunta): [PLANO_PROFILING.md](PLANO_PROFILING.md)
- Resultados já medidos: [RESULTADOS_PROFILING.md](RESULTADOS_PROFILING.md)

Este documento é sobre a **ferramenta**, não sobre os gargalos.
[PLANO_PROFILING.md](PLANO_PROFILING.md) pergunta *onde está o tempo*;
aqui a pergunta é *o que o instrumento precisa ter para responder isso*, e
o que ele ainda não tem.

Escopo adicional pedido junto: a matriz de carga precisa parametrizar a
**proporção entre leitura e escrita**, com padrão de **10 leituras por
escrita** (§4). As duas metades são o mesmo trabalho, e a §4.6 explica por
quê: com o padrão em 10:1 a maioria absoluta das operações passa por um
caminho de código que hoje tem **zero estágios instrumentados**.

## 1. O profiler já existe — parcialmente

A Etapa 1 do plano de profiling entregou a espinha dorsal, e ela funciona:
foi o que localizou o laço de candidatas do `TableHeap` em 80–88% das fases
de update e habilitou o ganho de 3,5× registrado em
[RESULTADOS_PROFILING.md §4.2](RESULTADOS_PROFILING.md). O desenho está certo
e não precisa ser refeito:

| peça | onde | avaliação |
|---|---|---|
| taxonomia fixa de estágios | [stage_profile.hpp:34](../include/modb/diag/stage_profile.hpp) | nomes estáveis, tratados como `scenario_id` — correto |
| coleta | [stage_profile.cpp:50](../src/diag/stage_profile.cpp) | atômicos relaxed, duração estática — correto |
| `ScopedStage` | [stage_profile.hpp:85](../include/modb/diag/stage_profile.hpp) | RAII, classe vazia quando desligado — custo zero real |
| custo quando ligado | — | medido dentro do ruído (6,9 s vs 6,62 s limpo) |
| emissão | `stage_profile_json`, [campaign.cpp](../loadtests/campaign.cpp) | um registro por fase, com `unattributed_ns` **explícito** |
| build | `MODB_ENABLE_STAGE_PROFILING`, preset `stage-profile` | não precisa de knob novo |

Duas escolhas de desenho merecem ser preservadas conscientemente, porque são
a diferença entre um profiler e um relatório bonito:

1. **`unattributed_ns` é gravado, não absorvido.** O resíduo é resultado.
2. **Ausência do registro ≠ linha de zeros.** Fase não instrumentada não
   produz registro, para não se ler como "medi e não achei nada".

Portanto o trabalho não é *criar* um profiler: é fechar seis lacunas
concretas de um que já mede.

## 2. Diagnóstico — as seis lacunas

Cada linha foi verificada no código, não inferida do plano.

| # | Lacuna | Evidência | Consequência |
|---|---|---|---|
| **P1** | **Nenhum estágio no caminho de leitura.** Os 7 estágios declarados são todos de escrita | [stage_profile.hpp:34-53](../include/modb/diag/stage_profile.hpp) | leitura é 100% resíduo; a fase `read` do `crud_full` não tem atribuição nenhuma |
| **P2** | `object_encode` está **declarado no enum e não tem nenhum sítio de chamada** | `grep -rn "Stage::" src include` → 6 sítios, nenhum de `object_encode` | o comentário do header promete um estágio que não mede nada; parte dos 39% de resíduo de `create` é exatamente o encode que se acredita instrumentado |
| **P3** | **Cinco estágios do plano nunca foram declarados**: `buffer_pool_miss`, `buffer_pool_writeback`, `index_update`, `identity_map`, `validate` | [PLANO_PROFILING.md §Etapa 1](PLANO_PROFILING.md) lista 11; o enum tem 7 | é onde está o resíduo de `create` (58,6%) e de `delete` (15,7%) — o critério de aceite de ≥90% não fecha por falta destes |
| **P4** | **Agregação só por fase.** `stage_reset()` no começo, `stage_snapshot()` no fim | [target_embedded.cpp:109](../loadtests/target_embedded.cpp) e [:131](../loadtests/target_embedded.cpp) | num workload de fase única com mix (`mixed_oltp`) é **impossível** separar custo de leitura de custo de escrita — os dois somam no mesmo contador |
| **P5** | **Só totais; nenhuma cauda.** `StageTotals` tem `elapsed_ns`, `calls`, `units` | [stage_profile.hpp:61](../include/modb/diag/stage_profile.hpp) | um estágio com média boa e p99 ruim é indistinguível de um estágio uniforme. As fases já reportam p99 da operação inteira, mas não *de qual estágio veio* |
| **P6** | **Sem estágio de commit.** `wal_append`/`wal_sync` existem; a transação inteira não | [database.cpp:575](../src/object/database.cpp) `Transaction::commit` sem instrumentação | `mixed_oltp` commita por operação — o custo dominante do workload é justamente o que não tem nome |

P1, P4 e P6 são exatamente os três que a mudança de §4 torna urgentes.

## 3. Desenho do que falta

### 3.1 Fechar a taxonomia (P1, P2, P3)

Estágios novos, com o sítio de chamada já localizado. `units` tem significado
próprio por estágio, como já é a convenção.

**Caminho de leitura** (o que P1 pede):

| estágio | sítio | `units` |
|---|---|---|
| `identity_lookup` | [identity_map.cpp:410](../src/object/identity_map.cpp) `IdentityMap::find` | páginas de diretório tocadas |
| `heap_record_read` | [table_heap.cpp:513](../src/storage/table_heap.cpp) `TableHeap::read` | bytes do registro |
| `buffer_pool_miss` | [page_file.cpp:328](../src/storage/page_file.cpp) (o caminho de read-ahead, **depois** do miss de [buffer_pool.cpp:11](../src/storage/buffer_pool.cpp)) | bytes lidos do disco |
| `object_decode` | [object_codec.cpp:261](../src/object/object_codec.cpp) `decode_object` | bytes do payload |
| `materialize` | [projection_plan.cpp:189](../src/object/projection_plan.cpp) e [binding.cpp:96](../src/object/binding.cpp) | campos materializados |

`materialize` é o estágio que testa **H6** (dívidas de CPU do Binding —
`std::function` por campo, `to_field_values` sem zero-copy). H6 está aberta
desde o começo do plano e nunca foi medida; este é o instrumento que a mede.

**Caminho de escrita** (o que P2, P3 e P6 pedem):

| estágio | sítio | nota |
|---|---|---|
| `object_encode` | [object_codec.cpp:248](../src/object/object_codec.cpp) `encode_object` | **já declarado**; só falta o `ScopedStage` |
| `index_update` | `index_maintain`, [object_store.cpp:305](../src/object/object_store.cpp) | uma chamada por create/update/delete |
| `tx_commit` | [database.cpp:575](../src/object/database.cpp) `Transaction::commit` | **envelope**, contém `wal_append`/`wal_sync` — ver a ressalva abaixo |
| `buffer_pool_writeback` | flush de página suja em [page_file.cpp](../src/storage/page_file.cpp) | bytes escritos |

**Ressalva de aninhamento, que precisa ficar no código.** A soma dos estágios
hoje é válida como cobertura porque **nenhum estágio contém outro**. Um
`tx_commit` que envolve `wal_append` quebra isso: `attributed_ns` passaria de
100% e `unattributed_ns` viraria lixo. Duas saídas, e a segunda é a
recomendada:

1. medir `tx_commit` **exclusivo** (subtraindo os filhos) — exige que o escopo
   saiba dos filhos, acoplamento ruim;
2. **marcar o estágio como envelope no enum** e excluir envelopes de
   `attributed_ns` ([target.hpp:85](../loadtests/target.hpp)), emitindo-os no
   JSONL num bloco separado (`envelopes`). Custa um `bitset` constante e uma
   linha na soma. A cobertura continua significando o que significa hoje.

### 3.2 Atribuição por classe de operação (P4)

É a lacuna que a §4 torna bloqueante: com 10:1, ~91% das operações são
leituras, e hoje elas somam no mesmo balde que as escritas.

Três desenhos considerados:

| desenho | custo no caminho quente | veredito |
|---|---|---|
| `thread_local` apontando o balde corrente; `stage_record` escreve nele | uma leitura de TLS por escopo | **rejeitado**: mexe no caminho quente do motor para um problema do harness |
| balde por classe dentro de `diag`, com `stage_set_class()` | idem, mais estado global mutável | rejeitado, mesmo motivo |
| **diferença de snapshot na fronteira do harness** | 0 no motor | **recomendado** |

O terceiro funciona porque `stage_snapshot()` é barato e o harness já sabe qual
operação vai emitir. No worker do `mixed_oltp`
([target_embedded.cpp:1335](../loadtests/target_embedded.cpp)) a operação
inteira já roda sob `state.mutex` — então tirar snapshot antes e depois, e
acumular a diferença no balde da classe, é correto sem sincronização nova:

```
// esboço, dentro do lock que já existe
const auto before = diag::stage_snapshot();
... a operação (create | read | update | delete) ...
state.by_class[op_class].accumulate(diag::stage_snapshot(), before);
```

Custo: 2 × 7 estágios × 3 atômicos relaxed ≈ 42 loads por operação, na casa de
50–100 ns. Contra os ~27 µs/op de uma leitura, é 0,2–0,4% — menor que a
distorção que a instrumentação já tem, e **zero** quando o build está sem
`MODB_ENABLE_STAGE_PROFILING` (o snapshot devolve zeros e a diferença é
elidida).

Emissão: `stage_profile` ganha um campo opcional `by_operation_class` com
`read`/`create`/`update`/`delete`, cada um com os mesmos totais e a própria
contagem de operações. Aditivo ao schema — o dashboard atual continua lendo o
que já lê.

### 3.3 Caudas por estágio (P5)

O que **não** fazer: percentis por estágio via reservoir sampling. Custa
alocação e um RNG no caminho quente para responder uma pergunta que o passo
mais barato já responde.

O que fazer, em dois degraus:

1. **`max_ns` por estágio** — um `fetch_max` relaxed por escopo, praticamente
   grátis. Já separa "média ruim" de "cauda ruim".
2. **Histograma log₂ opcional** — 32 contadores `uint64` por estágio
   (`bucket = 63 - countl_zero(elapsed_ns)`), 8 KiB de estado total para 7
   estágios. Dá p50/p99/p999 *por estágio* com erro de ±100% no valor do
   bucket, o que é mais que suficiente para dizer *qual* estágio produz a
   cauda — que é a pergunta. Um `option` de build separado
   (`MODB_ENABLE_STAGE_HISTOGRAM`) mantém o `stage-profile` barato.

### 3.4 O que fica deliberadamente de fora

- **Profiler amostral.** O motivo já está registrado em
  [stage_profile.hpp:6](../include/modb/diag/stage_profile.hpp) e não mudou:
  nesta toolchain não há profiler com símbolos para o binário Windows (MinGW
  emite DWARF em PE; VTune/WPA querem PDB), e um profiler amostral de CPU não
  vê espera de `fsync` — que é metade da pergunta. A Etapa 2 do plano de
  profiling (gprof/`perf`) segue **opcional**, como já foi reordenada.
- **`rdtsc` no lugar de `steady_clock`.** QPC custa ~20–25 ns; o ganho não
  paga a perda de portabilidade e a conversão de frequência.
- **Percentis exatos por estágio.** Ver §3.3.

## 4. A dimensão leitura/escrita

### 4.1 O que existe hoje, e o que a documentação promete

A proporção do `mixed_oltp` é **literal no código**: `r < 0.05` create,
`r < 0.85` read, `r < 0.95` update, resto delete
([target_embedded.cpp:1347-1400](../loadtests/target_embedded.cpp)) — 5/80/10/5,
ou seja **4 leituras por escrita**.

E [docs/PLANO_TESTES_DE_CARGA.md:167](../docs/PLANO_TESTES_DE_CARGA.md) já
descreve o workload como *"at a configured ratio (default 5/80/10/5)"*. A
palavra `configured` não tem implementação — a documentação está adiantada em
relação ao código. Fechar isso é parte da mudança, não um extra.

### 4.2 Semântica escolhida

**Dimensão:** `reads_per_write`, inteiro, **padrão 10**.

Seleção por inteiro, não por limiar em ponto flutuante:

```
const auto slot = rng() % (reads_per_write + 1);
if (slot != 0) → leitura
else           → escrita, repartida na composição interna abaixo
```

Assim as frações são racionais exatas, não arredondamentos de FP, e
`reads_per_write=0` significa **só escrita** — um valor legítimo e útil.
Leitura pura não se expressa nesta dimensão (não há inteiro para "infinito") e
não precisa: já existe `read_hotspot`.

**Composição interna das escritas preservada.** Hoje create/update/delete são
5/10/5, isto é 25%/50%/25% das escritas. Mantida como está, para que mudar a
razão mude *uma* variável:

| `reads_per_write` | read | create | update | delete |
|---|---|---|---|---|
| 4 (comportamento atual) | 80,00% | 5,00% | 10,00% | 5,00% |
| **10 (novo padrão)** | **90,91%** | **2,27%** | **4,55%** | **2,27%** |
| 0 | 0% | 25,00% | 50,00% | 25,00% |

Create e delete continuam simétricos, então o conjunto vivo segue estável ao
longo da fase — a invariante de reconciliação de contagem
([target_embedded.cpp:1575](../loadtests/target_embedded.cpp)) não muda de
natureza.

### 4.3 Onde toca

| arquivo | mudança |
|---|---|
| [matrix.hpp:47](../loadtests/matrix.hpp) | campo `std::uint64_t reads_per_write{10}` em `Case` |
| [matrix.cpp:156](../loadtests/matrix.cpp) | sufixo de variante `rw<N>` quando `!= 10`, no mesmo padrão de `c<N>`/`payload_<x>`/`batch<N>` |
| [matrix.cpp:208](../loadtests/matrix.cpp) | `unimplemented_dimension_reason` recusa `reads_per_write != 10` fora do `mixed_oltp` — a mesma disciplina que já vale para `concurrency` (dívida D1) |
| [matrix.cpp:321](../loadtests/matrix.cpp) | selector de cross-product, ao lado de `concurrency`/`payload` |
| [modb_load.cpp:155](../loadtests/modb_load.cpp) | flag `--reads-per-write a,b,c` + linha no `usage` |
| [target.hpp:105](../loadtests/target.hpp) | campo em `WorkloadParams` |
| [workloads/mixed_oltp.cpp:23](../loadtests/workloads/mixed_oltp.cpp) | repassa `c.reads_per_write` |
| [target_embedded.cpp:1347](../loadtests/target_embedded.cpp) | a seleção da §4.2, e os contadores da §4.4 |
| [docs/PLANO_TESTES_DE_CARGA.md:450](../docs/PLANO_TESTES_DE_CARGA.md) | linha na tabela §4.5 de dimensões secundárias |
| [docs/PLANO_TESTES_DE_CARGA.md:167](../docs/PLANO_TESTES_DE_CARGA.md) | corrigir o padrão descrito (5/80/10/5 → 10:1) |
| [target_embedded.hpp:56](../loadtests/target_embedded.hpp) | o comentário diz "proporção fixa 5/80/10/5" |
| `tests/` | um caso de mix alcançado (§4.4) e um de recusa por `unimplemented_dimension_reason` |

### 4.4 Mix configurado vs mix alcançado — não é opcional

O worker atual degrada em silêncio: quando `live_ids` está vazio, read, update
e delete **não fazem nada** e a operação conta como executada
([target_embedded.cpp:1367](../loadtests/target_embedded.cpp), `&& !state.live_ids.empty()`).
Com a razão configurável, isso deixa de ser detalhe: sem contadores, não há
como saber se a corrida realmente rodou 10:1.

Por isso a `PhaseMetrics` do `mixed_oltp` passa a emitir as contagens
**alcançadas** por classe (`operations_read`, `operations_create`,
`operations_update`, `operations_delete`, `operations_noop`), e o relatório
reporta a razão medida ao lado da configurada. Um caso cuja razão alcançada
desvia da configurada além de uma tolerância declarada é um **aviso no
registro**, não um número silenciosamente errado. Isto casa direto com o balde
por classe da §3.2: os mesmos contadores servem às duas coisas.

### 4.5 Identidade de caso e série histórica — e por que **não** mexer no `series_key`

Este é o ponto onde a decisão barata e a decisão consistente divergem, e vale
declarar a escolha.

O `series_key` já hasheia `payload`, `batch` e `concurrency` como campos
próprios ([series_key.hpp:31](../loadtests/history/series_key.hpp)) *além* de
hasheá-los indiretamente via `case_id` — o sufixo de variante já os codifica. A
prática vigente é, portanto, redundante.

Seguir a prática (acrescentar `reads_per_write` a `SeriesKeyInput`) mudaria a
string hasheada de **todos** os pontos, forçando `series_key_version` de 2 para
3. Custo verificado: `load-history/series.jsonl` tem **64 pontos na versão 2** —
o baseline recém-coletado inteiro perderia comparabilidade com o futuro, e a
série reiniciaria pela segunda vez em cinco dias.

Não é necessário. Uma variante `rw20` produz um `case_id` diferente, e
`case_id` **já entra no hash** — os casos fora do padrão se separam sozinhos.
O único problema real é o outro: **o padrão muda de 4:1 para 10:1 com o mesmo
`case_id`**, então pontos antigos descreveriam um workload que não existe mais.

Verificado: os **únicos 2 pontos de `mixed_oltp` da série estão na versão 1** —
os 64 pontos da versão 2 são todos de outros workloads. Ou seja, a janela para
mudar o padrão do `mixed_oltp` sem custo histórico nenhum é **agora**, e ela
fecha na próxima campanha que rodar `load-behavior`.

Mesmo assim, o mecanismo correto deve entrar junto, porque a próxima mudança de
semântica não vai ter essa sorte. O campo certo já existe e está inerte:
`workload_version` está **hardcoded em 1** com o comentário *"sem versionamento
de workload ainda"* ([rollup.cpp:299](../loadtests/history/rollup.cpp)).

Recomendação:

1. `workload_version` passa a vir de uma tabela por workload;
2. `mixed_oltp` vai para **2**; todo o resto fica em **1**;
3. `series_key_version` **permanece 2**.

Resultado: os 64 pontos da versão 2 continuam comparáveis, os 2 pontos antigos
de `mixed_oltp` ficam separados por duas razões independentes em vez de uma, e a
mudança de semântica fica registrada onde se procura por ela. Custo: ~20 linhas.

### 4.6 Por que as duas metades são o mesmo trabalho

Com o padrão em 10:1, ~91% das operações do `mixed_oltp` são leituras — e o
caminho de leitura tem **zero estágios instrumentados** (P1). Sem P1, o novo
padrão produziria um `stage_profile` cuja atribuição cai de ~90% para perto de
9%. E sem P4 (balde por classe) os poucos estágios de escrita que existem
apareceriam diluídos por 11 operações, sem que se possa dividir pelo número
certo.

Ou seja: **entregar a razão configurável sem P1 e P4 torna o profiler menos
útil do que ele é hoje.** A ordem da §6 não é preferência de estilo.

## 5. Predições pré-registradas

Escritas antes de rodar, como manda a §4 do plano de profiling. Uma predição
errada é resultado.

**Pred. 1 — o efeito do mix em `mixed_oltp` é dominado pelo commit.** O
workload commita por operação; leituras não commitam. Passando de 20% para
9,1% de escritas, as transações por operação caem ~2,2×. Partindo dos 1.121
ops/s registrados a 100k (**Debug** — ver ressalva), e tratando leitura a ~27
µs, o custo implícito de uma escrita com commit sai em ~4,35 ms, o que prevê
**~2,1× de vazão** ao trocar 4:1 por 10:1.

- **Refutação:** se a vazão subir menos de ~1,3×, o custo por operação **não**
  é dominado pelo commit, e `tx_commit` (P6) vai mostrar onde está.
- **Ressalva honesta:** 1.121 ops/s é número de Debug/`-O0`, e
  [RESULTADOS_PROFILING.md §6 A4](RESULTADOS_PROFILING.md) já pede que
  `mixed_oltp` seja refeito em RelWithDebInfo. A predição vale como ordem de
  grandeza da **razão**, não como valor absoluto.

**Pred. 2 — sem o commit no meio, o mix quase não importa.** Pelos números
isolados pós-A2 (read 26,8 µs; create 30,1; update 37,1; delete 21,8 µs/op), a
média ponderada dá 27,75 µs/op a 4:1 e 27,2 µs/op a 10:1 — **~2% de
diferença**. Se o `mixed_oltp` com `batch` alto se mover muito mais que isso,
existe uma interação (retenção MVCC, pressão de cache, manutenção de índice)
que os workloads de fase separada não conseguem ver — e é achado novo.

**Pred. 3 — a atribuição de leitura fecha em `materialize` ou em
`object_decode`.** A fase `read` não degrada com o volume (1,05× de 10k para
100k), então o custo é por operação, não por tamanho da estrutura:
`identity_lookup` e `buffer_pool_miss` devem ficar pequenos, e o peso deve cair
no decode/materialize — que é exatamente H6.

- **Refutação:** se `buffer_pool_miss` dominar, o custo é de I/O e H6 é
  irrelevante para leitura.

## 6. Ordem de execução e esforço

| # | Passo | Depende | Esforço |
|---|---|---|---|
| 1 | `object_encode` ganha sítio de chamada (P2) | — | ~15 min |
| 2 | Estágios de leitura: `identity_lookup`, `heap_record_read`, `buffer_pool_miss`, `object_decode`, `materialize` (P1) | — | ~2 h |
| 3 | Estágios de escrita que faltam: `index_update`, `buffer_pool_writeback` (P3) | — | ~1 h |
| 4 | Envelope `tx_commit` + `bitset` de envelopes fora de `attributed_ns` (P6, §3.1) | 3 | ~1 h |
| 5 | `max_ns` por estágio (P5, primeiro degrau) | — | ~30 min |
| 6 | Dimensão `reads_per_write` ponta a ponta (§4.3) | — | ~2 h |
| 7 | Contadores de mix alcançado (§4.4) | 6 | ~1 h |
| 8 | Balde por classe de operação (P4, §3.2) | 2, 7 | ~2 h |
| 9 | `workload_version` por workload; `mixed_oltp` → 2 (§4.5) | 6 | ~30 min |
| 10 | Medir: `mixed_oltp` a 100k em RelWithDebInfo, 4:1 vs 10:1, um processo por caso, 3 repetições | 1–9 | tempo de máquina |
| 11 | Histograma log₂ opcional (P5, segundo degrau) | 5 | ~2 h |

Os passos 1–5 valem por si, independentemente da §4 — fecham o critério de
aceite de ≥90% de cobertura que `create` (58,6%) e `delete` (15,7%) não
atingem. O passo 11 só se as caudas realmente virarem a pergunta.

**Regra herdada e mantida:** nenhuma otimização entra só com perfil; precisa de
comparação de benchmark antes/depois registrada
([PLANO_BENCHMARKS.md §12](../docs/PLANO_BENCHMARKS.md)).

## 9. Andamento (execução de 2026-08-02)

Legenda: ⬜ não começado · 🔄 em andamento · ✅ concluído

| # | Passo | Estado | Nota |
|---|---|---|---|
| 1 | `object_encode` ganha sítio (P2) | ✅ | + `object_decode` no mesmo arquivo |
| 2 | Estágios de leitura (P1) | ✅ | com dois desvios do desenho — §9.1 |
| 3 | `index_update`, `buffer_pool_writeback` (P3) | ✅ | + `page_file_sync` e `wal_open`, §9.2 |
| 4 | Envelope `tx_commit` + máscara (P6) | ✅ | e mais 4 envelopes que o desenho não previu — §9.1 |
| 5 | `max_ns` por estágio (P5) | ✅ | `compare_exchange` relaxed (não há `fetch_max` antes de C++26) |
| 6 | Dimensão `reads_per_write` (§4.3) | ✅ | todos os 11 sítios da tabela |
| 7 | Contadores de mix alcançado (§4.4) | ✅ | emitidos no `phase_summary`, não no `stage_profile` — §9.3 |
| 8 | Balde por classe (P4, §3.2) | ✅ | diferença de snapshot, como desenhado |
| 9 | `workload_version` por workload (§4.5) | ✅ | `mixed_oltp`→2; `series_key_version` segue 2 |
| 10 | Medir 4:1 vs 10:1 em RelWithDebInfo | ⬜ | tempo de máquina; Pred. 1 e 2 seguem por verificar |
| 11 | Histograma log₂ (P5, 2º degrau) | ⬜ | opcional; `max_ns` já responde "qual estágio produz a cauda" |

138/138 testes passam em `stage-profile`, `relwithdebinfo` e `sanitizers`.

Custo da instrumentação depois de dobrar o número de estágios, medido em
`crud_full.embedded.10k`, 3 repetições: **1,381 s ligado vs 1,337 s desligado
(+3,3%)** — na mesma faixa dos +4,2% medidos com 7 estágios (§1), apesar de
`buffer_pool_hit` ser chamado ~6 vezes por operação. Continua sendo diagnóstico
comparativo consigo mesmo, nunca gate.

### 9.1 Desvio: o desenho subestimou o aninhamento

A §3.1 previa **um** envelope (`tx_commit`). A verificação sítio a sítio achou
cinco, e um deles já existia e já estava contando errado:

| envelope | contém | situação |
|---|---|---|
| `heap_candidate_try` | `heap_page_write`, `persist_root`, leitura de página | **já era folha antes desta execução** — `try_insert` ([table_heap.cpp:364](../src/storage/table_heap.cpp)) faz as três coisas |
| `identity_lookup` | leitura de página | previsto como folha pela §3.1 |
| `heap_record_read` | leitura de página | idem |
| `index_update` | leituras/escritas de nós de BTree | idem |
| `tx_commit` | `wal_append`, `wal_sync`, `buffer_pool_writeback`, `page_file_sync`, `wal_open` | único previsto |

Consequência a registrar: **o `attributed_ns` das fases de update publicado
antes desta execução estava inflado** por `heap_candidate_try` conter
`heap_page_write` + `persist_root`. Os 93% de cobertura de update citados na
§1 e em [PLANO_PROFILING.md §8](PLANO_PROFILING.md) contavam tempo duas vezes.

Segundo desvio: a §3.1 listava `buffer_pool_miss` como a folha do caminho de
leitura, mas com as leituras vindo quase todas do cache ela fica em ~0 e o
custo real da cópia de página não teria folha nenhuma. Foi acrescentado
`buffer_pool_hit`, mutuamente exclusivo com `buffer_pool_miss` dentro de
`PageFile::read`. Ele está abaixo do limiar de ~1 µs da §7 e o header declara
isso no ponto de declaração.

Terceiro: `materialize` e `object_bind` foram instrumentados em
`Database::materialize_decoded`/`create`/`update`, e **não** nos dois
`::materialize` que a §3.1 apontava — um binder de campo embutido chama
`Binding::materialize` de dentro de `ProjectionPlan::materialize`
([binding.hpp:236](../include/modb/object/binding.hpp)), então o estágio se
aninharia em si mesmo.

### 9.2 O que a instrumentação encontrou de imediato

Com a taxonomia fechada, `mixed_oltp` a 1k saiu de **28,6% para 94,8%** de
cobertura — acima do critério de aceite. O que apareceu no resíduo de
`tx_commit` não estava em nenhuma hipótese do plano:

| estágio | ns/op (`mixed_oltp` 1k) | observação |
|---|---|---|
| `wal_open` | **148.102** | **o WAL é reaberto a cada commit** (`Wal::open_durable`, [database.cpp:393](../src/object/database.cpp)): stat + open por transação. Maior custo isolado do workload |
| `page_file_sync` | 87.081 | `fsync` do arquivo de **dados**, distinto do WAL; `commit_transaction` chama `file_->flush()` 3–4 vezes |
| `wal_sync` | 74.735 | o `fsync` do WAL, o único que já tinha nome |
| `wal_append` | 22.225 | |

`tx_commit` inteiro é **97% da fase**. Nada disso é otimização ainda — a regra
de §6 continua valendo — mas `wal_open` e o `page_file_sync` repetido são
candidatos com teto grande e correção barata, e nenhum dos dois aparecia em H1–H6.

`object_bind` (a metade de escrita de H6) mediu **0,3 pontos percentuais** de
`create`: a conversão do Binding é barata. Predição implícita de H6 para o
caminho de escrita, refutada.

### 9.3 Onde a cobertura ainda não fecha, e por quê

`crud_full` a 10k, depois da correção de aninhamento:

| fase | cobertura | antes |
|---|---|---|
| `update_shrink` | 85,5% | 93% (inflado) |
| `delete` | 81,0% | 15,7% |
| `update_inplace` | 67,0% | 93% (inflado) |
| `update_grow` | 66,3% | 93% (inflado) |
| `create` | 47,7% | 58,6% (inflado) |
| `read` | 17,0% | — (não havia estágio de leitura) |

`create` e `read` não atingem 90%, e a instrumentação diz onde não está: em
`read`, os estágios do motor somam ~2,9 µs dos 10,3 µs/op da fase; os 7,4 µs
restantes estão **fora do motor**, no laço do próprio harness. Ou seja, uma
parte relevante do que a série histórica chama de "vazão de leitura" é custo do
medidor, não do medido. É um achado sobre o harness, não sobre o motor, e não
foi perseguido nesta execução.

Achado colateral do mesmo bloco: `identity_lookup` e `heap_record_read` têm
`calls_per_operation = 2,00` na fase `read` — o caminho de leitura **resolve o
id e lê o registro duas vezes por operação** (`peek_type` seguido de `get`).
Não corrigido aqui; registrado para não se perder.

## 7. Riscos

1. **Distorção da instrumentação em estágios finos.** `identity_lookup` pode
   ficar na casa de 100–300 ns; a ~25 ns de relógio, isso é 10–25% de erro
   *naquele estágio*. Regra a escrever no header, que já existe em espírito:
   estágio abaixo de ~1 µs se mede por **contagem**, não por tempo. Se
   `identity_lookup` ficar nessa faixa, ele vira contador.
2. **Aninhamento silencioso.** Qualquer estágio novo dentro de outro quebra
   `attributed_ns` sem aviso. Mitigação: o `bitset` de envelopes da §3.1 e um
   teste que falhe se `attributed_fraction > 1,0`.
3. **M5 (contaminação por ordem)** continua não resolvido
   ([RESULTADOS_PROFILING.md §5](RESULTADOS_PROFILING.md)): qualquer medição de
   4:1 vs 10:1 tem que ser um processo por caso, ou a comparação não vale.
4. **Conflito de calibração ainda aberto** (A7): a tabela de
   [windows-x86_64.json](../loadtests/calibration/windows-x86_64.json) é
   medida em Debug e os scripts agora preferem RelWithDebInfo. Mudar o mix
   padrão do `mixed_oltp` **muda a duração real** do caso e afasta ainda mais a
   estimativa — `--max-duration`/`--max-disk-gb` podem emitir
   `skipped_budget` errado. A opção 2 de A7 (calibração por classe de build)
   resolve os dois de uma vez.
5. **Máquina de desenvolvimento ruidosa** (`environments.json` marca
   `desktop-windows` como *"noisy, don't use for gating"*): ≥3 repetições e
   ordem alternada, nunca como gate.
6. **`--no-index` em toda execução exploratória** (A7). Uma sessão anterior já
   depositou um ponto contaminado na série permanente.

## 8. O que este documento não afirma

- Não afirma que o profiler atual está errado. Ele achou o gargalo dominante e
  habilitou um ganho de 3,5× medido. As seis lacunas são de **cobertura**, não
  de desenho.
- Não afirma que 10:1 é mais representativo que 4:1 de qualquer carga real. É
  o padrão pedido; a dimensão existe justamente para que a razão deixe de ser
  uma opinião embutida no código.
- Não afirma que a Pred. 1 vai se confirmar. O número de partida é de Debug e
  está declarado como tal.
- Não resolve M5, nem o conflito de calibração de A7 — os dois seguem abertos e
  são pré-requisitos de qualquer comparação, não deste desenho.
- Não decide o page size padrão (A5) nem toca em H5 (retenção MVCC). H6 passa
  de "não testada" para "testável" pelo estágio `materialize`, o que não é o
  mesmo que testada.
