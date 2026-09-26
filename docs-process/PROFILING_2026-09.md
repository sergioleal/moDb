# Profiling — ciclo P1 (2026-09)

- Plano: [PLANO_TAREFAS_DESEMPENHO.md](PLANO_TAREFAS_DESEMPENHO.md), tarefas T3–T10
- Código medido: branch `perf/plano-tarefas-desempenho`, a partir de `8aee7f1`
  (motor idêntico a `8260f59`; os commits entre eles são só documentação)
- Ambiente: Windows 11 25H2, AMD Ryzen 7 PRO 8840HS (8 físicos / 16 lógicos),
  28 GB, NTFS, GCC 15.2.0 (MinGW, CLion 2026.1.4), page size 8 KiB
- Runner: [`scripts/measure_load.py`](../scripts/measure_load.py) — um processo
  por repetição, work dir novo por execução, ordem alternada, `--no-index`
- Binários: `build/stage-profile` (atribuição por estágio; +3,3% de custo
  medido) e `build/relwithdebinfo` (vazão sem instrumentação)

Cada seção registra a **predição escrita antes de rodar**, o resultado e o
veredito. Uma predição errada é resultado.

## T3 — Rodada única de remedição com `stage-profile`

Casos: `mixed_oltp.embedded.10k`, `snapshot_hold.embedded.10k`,
`crud_full.embedded.100k`; 5 repetições; seed 1.

### Predições (escritas antes de rodar)

- **3.1 `tx_commit`.** Continua sendo ≥ 90% da fase `mixed_oltp` e da fase
  `hold`. Com o WAL mantido aberto e um flush a menos, `wal_open` sai do
  ranking e `page_file_sync` (2 chamadas por commit) é a maior folha, seguida
  de `wal_sync`. Juntos, os dois `fsync` passam de 70% de `tx_commit`.
  *Refutação:* se alguma folha que não seja `fsync` passar de 25% de `tx_commit`.
- **3.2 `update_shrink` a 8 KiB.** `heap_candidate_scan` cai abaixo dos 75,7%
  medidos a 4 KiB, porque a contagem de páginas caiu à metade e a fase ficou
  1,94× mais rápida; previsão: iterações/op em torno de metade das 6.723
  (≈ 3.400) e fração entre 40% e 65%. *Refutação:* fração acima de 70% ou
  iterações/op acima de 5.000.
- **3.3 fase `read`.** Entre os estágios do motor, `object_decode` +
  `materialize` somam mais que `identity_lookup` + `heap_record_read` +
  `buffer_pool_miss`, e `buffer_pool_miss` fica perto de zero (100k objetos
  cabem no cache `warm`). A cobertura contra o tempo de parede segue baixa
  (~17%), porque ~74% da fase é o harness.
- **3.4 WAL por operação.** No `mixed_oltp` a 10:1, ~2,6 KB de WAL por
  operação (medido no probe de 1k), o que dá ~29 KB por escrita — cerca de
  3–4 imagens de página de 8 KiB por commit. O valor não muda com a escala
  (10k vs 1k) além de ±15%.

### Resultado

`stage-profile`, 5 repetições, CV entre 2% e 11% nas vazões. Dados brutos e
tabela completa por estágio: [profiling/2026-09/t3-summary.md](profiling/2026-09/t3-summary.md) (e `.json`).

| caso · fase | ops/s | motor ops/s | WAL B/op | atribuído |
|---|---|---|---|---|
| `mixed_oltp.10k` · `mixed_oltp` | 6.569 (7,6%) | 6.575 | 2.687 | 94,2% |
| `snapshot_hold.10k` · `hold` | 805 (7,3%) | 945 | — | 94,7% |
| `crud_full.100k` · `create` | 57.798 (2,2%) | 153.046 | 481 | 39,8% |
| `crud_full.100k` · `read` | 113.102 (2,8%) | 372.576 | — | 15,9% |
| `crud_full.100k` · `update_inplace` | 65.048 (7,2%) | 100.427 | 954 | 57,4% |
| `crud_full.100k` · `update_grow` | 56.053 (5,9%) | 91.710 | 1.721 | 57,8% |
| `crud_full.100k` · `update_shrink` | 42.513 (10,6%) | 78.521 | 2.656 | 71,4% |
| `crud_full.100k` · `delete` | 128.531 (8,9%) | 188.756 | 2.720 | 66,2% |

`crud_full.embedded.100k` inteiro leva agora ~10,5 s (eram 105,6 s na abertura
do plano de profiling e ~30 s depois da A2).

**3.1 — confirmada.** Na fase `mixed_oltp`, `tx_commit` custa 142.100 ns por
operação contra 152.091 ns de tempo de motor por operação: **93,4%**.
`page_file_sync` (61.515 ns/op) é a maior folha, seguida de `wal_sync`
(58.578); juntos são **84,5% de `tx_commit`**. Na fase `hold`, 85,4%. Nenhuma
folha que não seja `fsync` passa de 10% de `tx_commit` (`wal_append`: 9,3%).

O número que muda a leitura: com 0,089 commits por operação, `page_file_sync`
e `wal_sync` fazem **2,00 chamadas cada por commit** — **quatro `fsync` por
commit**, a ~330 µs cada (337 µs na fase `hold`). O commit é, em primeira
ordem, 4 × custo de um `fsync`. Dois deles têm alvo conhecido:

- o 2º `page_file_sync` é o do checkpoint (T9, checkpoint preguiçoso);
- o 1º `wal_sync` é o que separa as imagens de página do registro de commit
  (`commit_transaction`, [database.cpp](../src/object/database.cpp)). Ele só é
  necessário se a recuperação puder aceitar um registro de commit durável cujas
  imagens anteriores não estão. Se cada registro do WAL tem checksum e a
  recuperação para no primeiro registro inválido, um único `fsync` depois do
  registro de commit basta. **Candidato novo, não previsto no plano** — vira
  a subtarefa T7.3.

**3.2 — refutada, e no sentido bom.** A 8 KiB, `heap_candidate_scan` em
`update_shrink` custa **95 ns/op, 1,0 iteração/op — 0,7% do tempo de motor**,
contra 75,7% e 6.723 iterações/op medidos a 4 KiB. A predição (40–65%) errou
por uma ordem de grandeza: o custo não caiu à metade, desapareceu. O que
domina `update_shrink` agora é o commit: `tx_commit` = 9.871 ns/op, **~42% do
tempo de parede da fase** *(corrigido: a primeira versão desta frase dizia "77%
do tempo de motor", mas o tempo de motor dessas fases excluía o commit — ver
"Defeito de medição" em T5)*, com `page_file_sync` a ~2 ms por chamada (máximo de 27 ms):
cada commit do shrink suja ~930 KB de páginas, e o flush do arquivo de dados
paga a escrita delas. Consequência: **T14.1 perde o objeto** (não há mais
varredura para explicar), e o resto de T14 vira custo de commit — o mesmo
alvo de T5/T7/T9.

**3.3 — refutada.** Tempo de motor da fase `read`: 2.684 ns/op. Folhas:
`buffer_pool_hit` 448, `object_decode` 402, `materialize` 290,
`buffer_pool_miss` 271 (0,013 chamadas/op — perto de zero, como previsto).
Envelopes: `heap_record_read` 872 e `identity_lookup` 792, que contêm as
leituras de página. Tempo exclusivo dos envelopes (descontadas as folhas de
página): ~945 ns. Então:

| onde | ns/op | fração do motor |
|---|---|---|
| localizar e ler o registro (exclusivo dos envelopes) | ~945 | 35% |
| cópias de página (`buffer_pool_hit` + `miss`) | 719 | 27% |
| `object_decode` + `materialize` | 692 | 26% |
| não atribuído | ~328 | 12% |

A leitura **não** fecha em decode/materialize (a predição dizia que eles
somariam mais que localizar+ler; somam 26% contra 62%). E um número salta:
`buffer_pool_hit` copia **24.466 bytes por leitura — três páginas inteiras de
8 KiB** para ler um registro de 375 bytes. As dívidas de CPU do Binding (T15)
miram `materialize`, que é 11% do motor: teto baixo. O teto maior no caminho
de leitura está na cópia de página e na navegação de identidade/slot.

**3.4 — confirmada.** 2.687 B de WAL por operação a 10k contra 2.617 a 1k
(+2,7%, dentro de ±15%). Por commit: 29,1 KB, **3,55 imagens de página de
8 KiB**. Na fase `hold`, 26 KB por commit.

## T4 e T5 — Varreduras

Todas com `build/relwithdebinfo` (sem instrumentação), exceto 4.1, que usa
`stage-profile` porque a Pred. 2 precisa de `tx_commit` por escrita. Três
repetições (cinco em 5.2). `durability` e `batch` passaram a ter seletor na CLI
nesta rodada (T5.1, abaixo).

### Predições (escritas antes de rodar)

- **4.1 Pred. 1 e 2 a 100k** (`mixed_oltp.100k`, 4:1 vs 10:1). A vazão sobe
  **2,2× ± 0,3** de 4:1 para 10:1, como a 10k (2,26×), porque o custo continua
  dominado pelo commit e a fração de escritas cai de 20% para 8,9%.
  `tx_commit` por escrita muda menos de 5% entre as duas razões.
  *Refutação:* razão abaixo de 1,6× ou acima de 2,8×.
- **4.2 Page size 16 KiB** (`crud_full.100k` e `mixed_oltp.10k`, 8 vs 16 KiB).
  `create` e `read` ficam dentro de ±10%. As fases de update ficam **10–30%
  mais lentas** a 16 KiB: agora o commit domina `update_shrink` (77%), e o
  flush de dados e o WAL escrevem páginas inteiras, então dobrar a página
  dobra os bytes por commit. `mixed_oltp` fica dentro de ±15%: lá o custo é o
  número de `fsync`, não os bytes.
- **4.3 Cache `warm` vs `oversubscribed`.** A dimensão `cache` não tem
  dispatch (a matriz recusa `cache != warm`); o cenário existe como o workload
  `oversubscribed_churn`, que fica para T12.4. Sem predição aqui.
- **4.4 Payload** (`crud_full.10k`, filler de 64 / 256 / 4096 B). `create` e
  `read` custam por operação, não por byte, entre slim e normal (±15%); `fat`
  (registro ~16× maior) é **2–4× mais lento** em `create` e nas fases de
  update, por bytes de WAL e de página por operação, e `read` cai menos de 2×.
- **4.5 Escala de `create_only`** (1k, 10k, 100k, 250k, 500k, 1M). Depois da
  A2 o custo por inserção não depende mais da contagem de páginas: a vazão
  fica **plana dentro de ±20% de 10k a 1M** (a 1k o custo fixo de abertura
  pesa e pode divergir). *Refutação:* queda maior que 1,5× entre 10k e 1M.
- **5.2 `durability`** (`sync_real` vs `disabled_diagnostic`). Pela atribuição
  da T3, os `fsync` são ~79% do tempo de uma operação em `mixed_oltp` e ~85%
  do commit do `hold`: **`mixed_oltp` 4–5× mais rápido** e `snapshot_hold`
  (`hold`) 4–6×. Com `batch=1000`, o ganho é menor: `create` ~1,4× e
  `update_shrink` ~1,75× em `crud_full.100k`. *Refutação:* `mixed_oltp` abaixo
  de 3× — nesse caso a atribuição por estágio está subestimando outro custo
  do commit que só aparece sem `fsync`.
- **5.3 `batch`** (`create_only.10k`, 1 / 10 / 100 / 1.000 / 10.000). O tempo
  por operação segue `a + C/batch`, com `C` ≈ custo de um commit pequeno (4
  `fsync` ≈ 1,3–1,5 ms): batch 1 ≈ **650–800 ops/s**, batch 10 ≈ 5–7k,
  batch 100 ≈ 25–40k, batch 1.000 ≈ 55–65k, e batch 10.000 igual ao de 1.000
  (±10%). Com `disabled_diagnostic`, batch 1 sobe para a casa de 10–30k ops/s.

## T7.3 — Um `wal_sync` por commit (ADR-022, parte A)

### Predição (escrita antes de rodar)

Sai 1 dos 4 `fsync` do commit. Com os `fsync` em ~85% do commit e o commit em
~93% de uma operação de `mixed_oltp`, o commit fica ~21% mais barato:
**`mixed_oltp` +20–30% de vazão** e `snapshot_hold` (`hold`) +20–30%.
`wal_sync` passa de 2,00 para 1,00 chamada por commit. Em `crud_full.100k`
(`batch=1000`) o efeito é pequeno: +2–8% nas fases com commit relevante
(`update_shrink`, `create`), dentro do ruído nas outras. *Refutação:*
`mixed_oltp` abaixo de +10% — o que significaria que o segundo sync fica mais
caro quando o primeiro sai (o dispositivo agrupa os dois).

Base de comparação: as execuções `sync_real` da T5.2 (mesmo binário, mesma
metodologia, 5 repetições).

### Resultados de T4.1 e T4.2

**4.1 — confirmada, no limite de baixo.** `mixed_oltp.100k`, `stage-profile`,
3 repetições ([dados](profiling/2026-09/t4-1-summary.md)):

| razão | ops/s | commits/op | `tx_commit` por commit | `fsync` / `tx_commit` |
|---|---|---|---|---|
| 4:1 | 3.066 (CV 12,6%) | 0,2003 | 1.429 µs | 85,4% |
| 10:1 | 5.868 (CV 12,6%) | 0,0901 | 1.491 µs | 84,8% |

A vazão subiu **1,91×** (previsto 2,2 ± 0,3; dentro, na borda). A fração de
escritas caiu 2,22× — a diferença para os 1,91× é o custo das leituras, que a
100k pesa um pouco mais que a 10k. **Pred. 2 confirmada:** o custo do commit
por escrita variou +4,3% entre as razões (limite: 5%). O CV alto vem da
primeira repetição de cada razão, mais lenta que as outras duas (194,6 s
contra 158,4/152,9 s a 4:1); a razão entre as medianas é a mesma.

**4.2 — 16 KiB é pior em tudo; o padrão de 8 KiB fica.** `relwithdebinfo` vs
`profile-16k`, 3 repetições cada. Ressalva de método: os dois binários rodaram
em blocos seguidos, não alternados ([8 KiB](profiling/2026-09/t4-2-8k-summary.md),
[16 KiB](profiling/2026-09/t4-2-16k-summary.md)).

| caso · fase | 8 KiB ops/s | 16 KiB ops/s | 16 vs 8 | previsto |
|---|---|---|---|---|
| `crud_full.100k` · `create` | 65.859 | 55.317 | **−16%** | ±10% ❌ |
| `crud_full.100k` · `read` | 125.693 | 104.110 | **−17%** | ±10% ❌ |
| `crud_full.100k` · `update_inplace` | 79.730 | 56.345 | −29% | −10 a −30% ✅ |
| `crud_full.100k` · `update_grow` | 67.917 | 51.530 | −24% | −10 a −30% ✅ |
| `crud_full.100k` · `update_shrink` | 60.080 | 37.371 | −38% | −10 a −30% ❌ (pior) |
| `crud_full.100k` · `delete` | 180.219 | 109.822 | −39% | — |
| `mixed_oltp.10k` · `mixed_oltp` | 8.285 | 7.368 | −11% | ±15% ✅ |

A queda de `read` não estava prevista e é a mais informativa: ela confirma por
outro caminho o achado de T3.3. A leitura copia páginas inteiras (três por
registro), e a 16 KiB cada cópia dobra. `mixed_oltp` quase não muda porque ali
o custo é o número de `fsync`, não os bytes — como previsto.

**4.3 — sem medição.** A dimensão `cache` não tem dispatch; o cenário
equivalente é o workload `oversubscribed_churn`, reclassificado para T12.4.

**4.4 — confirmada para slim/normal; `fat` pior que o previsto, e com dois
achados.** `create_only.10k` com os três payloads e `crud_full.10k` com
slim/normal, 3 repetições ([dados](profiling/2026-09/t4-4-summary.md)):

| caso · fase | slim motor ops/s | normal motor ops/s | fat motor ops/s | WAL B/op (s/n/f) |
|---|---|---|---|---|
| `create_only` · `create` | 193.397 | 170.103 | 37.000 | 283 / 483 / 8.316 |
| `crud_full` · `read` | 508.248 | 478.411 | — | — |
| `crud_full` · `update_shrink` | 139.724 | 104.777 | — | 1.210 / 2.706 / — |

- Entre slim e normal o motor varia **+14% em `create` e +6% em `read`**:
  custo por operação, não por byte, como previsto.
- `fat` é **4,6× mais lento** em `create` no motor (previsto 2–4×). O motivo
  aparece nos bytes: **8.316 B de WAL e ~8.245 B de arquivo por objeto de
  ~4,1 KB**. Dois registros `fat` não cabem numa página de 8 KiB, então cada um
  ocupa uma página inteira — amplificação de espaço de ~2×, e uma imagem de
  página inteira por objeto no WAL.
- **`crud_full` com `fat` não roda**: `update_grow` falha com `record exceeds
  the maximum TableHeap page payload` — não há registro de overflow. O caso
  `crud_full.embedded.250k.payload_fat` do perfil `load-heavy` falha sempre.
  Registrado em [PLANO_TESTES_DE_CARGA.md](../docs/PLANO_TESTES_DE_CARGA.md).
- Na vazão de parede (`ops_per_second`), slim é 1,6× (`create`) a 2,0×
  (`read`) mais rápido que normal — **quase tudo harness**, que gera e valida o
  payload dentro do laço cronometrado. Mais uma evidência para T13.

### Defeito de medição encontrado em T5: o commit ficava fora do tempo de motor

Com `batch 1`, `create_only.10k` mediu 741 ops/s de parede e **68.624
"ops/s de motor"** — noventa vezes mais. Nos três laços com lote
(`perform_create_phase`, `perform_delete_phase` e o das fases de update, em
[target_embedded.cpp](../loadtests/target_embedded.cpp)), o `tx->commit()` era
chamado **fora** do intervalo cronometrado de cada operação. Consequências:

- `engine_ops_per_second` e as latências por operação dessas fases **excluíam o
  commit**, e `harness_overhead` o contava como harness. Os "37–74% de overhead
  do harness" de [PLANO_PROFILER.md §9.3.1](PLANO_PROFILER.md) misturam as duas
  coisas; só `read` (que não commita) e `mixed_oltp` (que commita dentro da
  operação) estavam certos.
- Na T3 este relatório disse que o commit era 77% do tempo de motor de
  `update_shrink`; contra o tempo de parede são ~42%. Corrigido acima.
- **`ops_per_second` (parede) nunca foi afetado.** Todas as comparações de
  vazão deste relatório usam ele.

Corrigido nesta rodada: o tempo de `commit` + `begin` do lote seguinte entra
na amostra da operação que fecha o lote. A latência dessa operação passa a
mostrar o commit, que é o que um cliente de fato espera.

### Resultados de T4.5, T5.2 e T5.3 (vazão de parede)

**4.5 — confirmada, com folga.** `create_only`, 3 repetições
([dados](profiling/2026-09/t4-5-summary.md)):

| escala | 1k | 10k | 100k | 250k | 500k | 1M |
|---|---|---|---|---|---|---|
| ops/s | 60.315 | 59.916 | 61.872 | 61.828 | 61.169 | 58.541 |

Plana dentro de **±5% de 1k a 1M** (previsto ±20%). Antes da A2 a vazão caía
3,3× só de 10k para 100k.

**5.2 — refutada: o `fsync` pesa mais do que a atribuição mostrava.**
`sync_real` vs `disabled_diagnostic`, 5 repetições
([dados](profiling/2026-09/t5-2-summary.md)):

| caso · fase | com `fsync` | sem `fsync` | fator | previsto |
|---|---|---|---|---|
| `mixed_oltp.10k` · `mixed_oltp` | 7.699 | 68.567 | **8,9×** | 4–5× |
| `snapshot_hold.10k` · `hold` | 958 | 10.453 | **10,9×** | 4–6× |
| `crud_full.100k` · `create` | 64.506 | 74.329 | 1,15× | ~1,4× |
| `crud_full.100k` · `update_inplace` | 80.383 | 95.226 | 1,18× | — |
| `crud_full.100k` · `update_grow` | 66.500 | 79.575 | 1,20× | — |
| `crud_full.100k` · `update_shrink` | 56.124 | 73.658 | 1,31× | ~1,75× |
| `crud_full.100k` · `delete` | 156.556 | 240.141 | 1,53× | — |
| `crud_full.100k` · `read` | 122.665 | 123.763 | 1,01× | — |

Nos workloads que commitam por operação, sem `fsync` o motor é **9–11× mais
rápido** — os `fsync` são ~89% do tempo de uma operação de `mixed_oltp`, não os
~79% que a soma dos estágios indicava. A diferença sugere que o `fsync` também
encarece o que vem depois dele (as escritas seguintes no mesmo arquivo), custo
que nenhum estágio captura. Com `batch=1000` o efeito é pequeno (1,15–1,5×),
menor que o previsto, porque a previsão partia do tempo de motor que excluía o
commit. **Teto para T7.3 + T9 em `mixed_oltp`: até ~9×; o que é alcançável
depende de quantos `fsync` sobram.**

**5.3 — confirmada para `sync_real`; o commit sem `fsync` custa mais do que o
previsto.** `create_only.10k`, 3 repetições ([dados](profiling/2026-09/t5-3-summary.md)):

| batch | 1 | 10 | 100 | 1.000 | 10.000 | 1 sem `fsync` |
|---|---|---|---|---|---|---|
| ops/s | 741 | 6.384 | 32.887 | 64.489 | 70.061 | 7.249 |
| previsto | 650–800 ✅ | 5–7k ✅ | 25–40k ✅ | 55–65k ✅ | = 1.000 ±10% ✅ (+8,6%) | 10–30k ❌ |

Um commit de um objeto custa **~1.350 µs** com `fsync` — quatro de ~305 µs
mais ~130 µs de resto — e **~138 µs sem `fsync`**. Esses ~130 µs fixos por
commit, sem nenhum `fsync`, são o alvo de T7.1: o commit faz uma escrita de
sistema por registro do WAL (`begin`, cada imagem de página, `commit`) e outra
por página aplicada, com alocação e CRC byte a byte em cada registro.

## T9 — Checkpoint preguiçoso (ADR-022, parte B) e A/B limpo de T7.3

A comparação de T7.3 contra a base da T5.2 mediu a fase `read` — que não
commita — **7,6% mais lenta**: deriva da máquina entre sessões, do mesmo
tamanho do efeito procurado. Comparar blocos rodados em horas diferentes não
vale (regra 2 do plano). O A/B passa a ser **alternado na mesma sessão**, com
três variantes por caso, cada repetição um processo:

| variante | binário | `--checkpoint-interval` | `fsync` por commit |
|---|---|---|---|
| base | `build/ab-base` (T7.3 revertida) | 1 (= antes da T9) | 4 |
| T7.3 | `build/relwithdebinfo` | 1 | 3 |
| T7.3 + T9 | `build/relwithdebinfo` | 64 (padrão) | 1 (+2 a cada 64 commits) |

`checkpoint_interval=1` reproduz exatamente o commit anterior: a barreira e o
checkpoint rodam em todo commit, com os mesmos dois `flush` do arquivo de dados.

### Predição da T9 (escrita antes de rodar)

Saem os dois `fsync` do arquivo de dados de 63 em cada 64 commits; sobra um
`fsync` de WAL por commit. A T7.3 isolada rendeu pouco (+5%, ainda a
confirmar no A/B), sinal de que os `fsync` não custam de forma independente.
Mesmo assim: **`mixed_oltp` 2–3× sobre a variante T7.3** e `snapshot_hold`
(`hold`) na mesma faixa; em `crud_full.100k` (`batch=1000`), +10–30% nas fases
que commitam páginas (`update_shrink`, `delete`), `read` inalterado.
*Refutação:* `mixed_oltp` abaixo de 1,5× sobre T7.3.

## T7.1 — Custo de `wal_append` fora do `fsync`

Pela T3, `wal_append` custa ~27 µs por registro de 8 KiB (~300 MB/s): na
fase `create` é ~1,6 µs por operação, e um commit de um objeto sem `fsync`
ainda custa ~130 µs (T5.3). Por registro, `Wal::append` faz: um `std::vector`
novo do tamanho de uma página, a cópia do payload, um CRC-32 **byte a byte**
e um `WriteFile`. Correção aplicada, sem mudar o formato:

- CRC-32 "slicing-by-8" (8 bytes por iteração; mesmo polinômio e mesmo
  valor), com um teste que recalcula o CRC de cada registro gravado por uma
  implementação de referência byte a byte e confere o valor clássico
  `"123456789"` → `0xCBF43926`;
- buffer do registro reaproveitado entre `append`s (`Wal::record_buffer_`).

Juntar as escritas de um commit numa só mudaria a semântica de `Wal` (hoje
cada `append` chega ao sink na hora) e fica fora desta subtarefa.

### Predição (escrita antes de rodar)

`wal_append` cai **30–45% em ns/op** (o CRC era ~1/3 do custo e fica 3–4×
mais rápido; a alocação some). Vazão: `create` +3–8%; `mixed_oltp`, que depois
da T9 deixa de ser dominado por `fsync`, +5–15%. *Refutação:* `wal_append`
caindo menos de 15% — o custo estaria na escrita de sistema, não no CRC.

### Resultado do A/B alternado (T7.3 e T9)

5 repetições por variante, as três alternadas dentro de cada repetição
([dados](profiling/2026-09/ab-summary.md); atribuição da variante final em
[ab-sp](profiling/2026-09/ab-sp-summary.md)). Vazão de parede:

| caso · fase | base (4 `fsync`) | T7.3 (3) | T7.3+T9 (1) | T7.3 / base | T9 / T7.3 | total |
|---|---|---|---|---|---|---|
| `mixed_oltp.10k` · `mixed_oltp` | 7.024 | 9.165 | **19.126** | **1,31×** | **2,09×** | **2,72×** |
| `snapshot_hold.10k` · `hold` | 900 | 1.125 | **2.518** | 1,25× | 2,24× | **2,80×** |
| `crud_full.100k` · `create` | 62.229 | 64.489 | 68.647 | 1,04× | 1,06× | 1,10× |
| `crud_full.100k` · `read` | 114.155 | 118.407 | 119.159 | 1,04× | 1,01× | 1,04× |
| `crud_full.100k` · `update_inplace` | 78.623 | 75.997 | 85.612 | 0,97× | 1,13× | 1,09× |
| `crud_full.100k` · `update_grow` | 65.460 | 64.235 | 70.887 | 0,98× | 1,10× | 1,08× |
| `crud_full.100k` · `update_shrink` | 54.440 | 55.349 | 62.896 | 1,02× | 1,14× | 1,16× |
| `crud_full.100k` · `delete` | 164.286 | 166.207 | 196.216 | 1,01× | 1,18× | 1,19× |

**T7.3 — confirmada** (+31% e +25%, previsto +20–30%). O "+5%" da primeira
comparação era deriva entre sessões: repetida alternada, a fase `read` (que não
commita) fica igual entre as variantes, dentro de ±4%.

**T9 — confirmada** (2,09× e 2,24× sobre T7.3, previsto 2–3×). Em
`crud_full.100k`, +10–18% nas fases que commitam páginas, `read` igual — como
previsto. A atribuição confirma o mecanismo: **1,00 `wal_sync` e 0,031
`page_file_sync` por commit** (2 a cada 64).

**O commit agora:** 467 µs em `mixed_oltp` (eram ~1.430 µs na T4.1), dos quais
~293 µs são o único `fsync` e ~102 µs são `wal_append` (22%). O teto sem
nenhum `fsync` (T5.2) é ~68.500 ops/s; estamos em ~19.100 — o `fsync` que
resta é o preço da durabilidade, e o próximo alvo fora dele é `wal_append`
(T7.1).

## T8 — Group commit: desenho, teto e decisão

**O que é.** Com vários escritores concorrentes, um `fsync` do WAL torna
duráveis os registros de commit de todos que já foram anexados. Em vez de um
`fsync` por commit, um "líder" sincroniza e acorda os demais.

**Por que não cabe hoje sem mudar o modelo.** O motor tem um único escritor
([ADR-011](../docs/decisions/ADR-011-concorrencia-do-servidor.md)); as sessões
concorrentes do `mixed_oltp` são serializadas por um mutex que cobre a
transação inteira, commit incluído. Para dois commits dividirem um `fsync`, o
escritor precisa soltar o lock **depois de anexar o registro de commit e antes
do `fsync`**, e a transação seguinte passa a ler dados de um commit ainda não
durável. Se aquele `fsync` falhar, as transações que leram esses dados
precisam abortar em cascata — hoje uma falha de `fsync` já exige reabrir o
banco (`require_recovery`), mas nenhuma outra transação viu o dado.

**Teto.** Depois de T7.3 e T9, o único `fsync` é ~293 µs de um commit de
~467 µs (63%) em `mixed_oltp`. Com `c` escritores realmente concorrentes, o
custo de `fsync` por commit cai até `1/c`: com `c = 4`, o commit ficaria em
~175–250 µs (1,9–2,7×). Com `c = 1` — o padrão do `mixed_oltp` e o caso de uma
aplicação embedded típica — o ganho é zero.

**Decisão: não fazer agora.** O ganho só existe com escritores concorrentes,
que o motor hoje serializa por desenho, e o custo é mudar o modelo de
concorrência e o de falha do commit. Condição para reabrir: uma medição de
servidor com sessões escritoras concorrentes (`mixed_oltp` com
`--concurrency 4,16` no alvo `loopback`) mostrando o `fsync` do WAL como
maior custo. Registrado também no ADR-022, em "alternativas".

### Resultado da T7.1

A/B alternado, 5 repetições, `build/ab-t9` (T7.3+T9) contra o build com T7.1
([dados](profiling/2026-09/t7-1-summary.md); estágios em
[t7-1-sp](profiling/2026-09/t7-1-sp-summary.md) contra
[ab-sp](profiling/2026-09/ab-sp-summary.md)):

| caso · fase | antes | depois | fator | `wal_append` ns/op |
|---|---|---|---|---|
| `mixed_oltp.10k` · `mixed_oltp` | 17.997 | 19.670 | **1,09×** | 9.093 → 6.973 (−23%) |
| `mixed_oltp.10k` · `create` | 66.098 | 69.028 | 1,04× | 1.334 → 815 (−39%) |
| `snapshot_hold.10k` · `hold` | — | — | — | 74.188 → 56.398 (−24%) |
| `create_only.100k` · `create` | 63.234 | 65.694 | 1,04× | — |
| `crud_full.100k` · `create` | 64.673 | 69.034 | 1,07× | — |
| `crud_full.100k` · `update_grow` | 65.299 | 71.390 | 1,09× | — |
| `crud_full.100k` · `delete` | 174.633 | 192.705 | 1,10× | — |
| `crud_full.100k` · `read` | 115.287 | 112.450 | 0,98× (ruído; não usa o WAL) | — |

**Parcialmente confirmada.** `wal_append` caiu 23–39% (previsto 30–45%;
`create` dentro, `mixed_oltp`/`hold` um pouco abaixo, bem acima do limite de
refutação de 15%). A vazão bateu a predição: `mixed_oltp` +9% (previsto
+5–15%), `create` +4–7% (previsto +3–8%). O que sobra de `wal_append` é a
escrita de sistema por registro — juntar as escritas de um commit é o próximo
passo, e muda a semântica de `Wal` (ver T7.2).

### T9.4 — Custo na recuperação (`restart_recovery`)

**Predição (escrita antes de rodar).** Com `checkpoint_interval=64`, a
reabertura depois de uma queda reaplica até 63 commits a mais que com
intervalo 1 — ~3,5 páginas cada, ~220 escritas de página. A fase
`restart_recovery` fica **no máximo ~20 ms mais longa**; as fases de carga do
caso ficam mais rápidas, como no resto da T9.

**Resultado — confirmada.** `restart_recovery.10k`, 5 repetições alternadas
([dados](profiling/2026-09/t9-4-summary.md)): a fase de reinício passou de
**133,7 ms (intervalo 1) para 141,4 ms (intervalo 64), +7,7 ms**; a fase
`create` do mesmo caso ficou 9% mais rápida (164,5 → 150,0 ms).

**Achado colateral, não previsto:** mesmo com intervalo 1 o reinício leva
~134 ms para um banco de 10k objetos, porque `tx::recover` e `Database::open`
leem o **WAL inteiro** (`Wal::read_all`) — e o WAL nunca é truncado. O custo
de abrir o banco cresce com a idade dele, com ou sem checkpoint preguiçoso.
Truncar ou segmentar o WAL até o menor LSN ainda necessário (checkpoint e
réplicas) é trabalho novo, fora deste ciclo.

## T6 — Pred. 3: a leitura fecha em `materialize`/`object_decode`

Respondida pelos dados da T3.3, sem rodada nova. A fase `read` não commita,
então o defeito de medição da T5 não a afeta e o tempo de motor dela é válido.

**6.1 — cobertura contra o tempo de motor** (2.684 ns/op em
`crud_full.100k`): as folhas somam 1.411 ns (**52,6%**); com o tempo exclusivo
dos envelopes `identity_lookup` e `heap_record_read` (~945 ns, que
`attributed_ns` não soma por serem envelopes), **88%**. Contra o tempo de
parede eram 15,9%: a diferença é o harness (~70% da fase).

**6.2 — veredito: refutada.** A leitura não fecha em decode/materialize (26%
do motor); fecha em localizar e ler o registro (35%) e em copiar páginas
inteiras (27%). `buffer_pool_miss` também não domina (0,013 chamadas/op). O
teto está na cópia de página (T15.5), não no Binding (T15.1–15.3).

## T10 — Resumo do ciclo, baselines e gates

### Por gargalo

| gargalo | evidência | ação | ganho medido (A/B alternado) | custo | decisão |
|---|---|---|---|---|---|
| 4 `fsync` por commit | T3.1: 85% do commit; T5.2: sem `fsync` 9–11× | T7.3: 1 sync de WAL (CRC já protege) | `mixed_oltp` 1,31×, `hold` 1,25× | pequeno; formato igual | **feito** |
| 2 `fsync` de dados por commit | T3.1, T5.2 | T9: checkpoint preguiçoso (ADR-022) | `mixed_oltp` 2,09×, `hold` 2,24× sobre T7.3; `crud_full` +10–18% | grande: ADR, tolerância a superbloco além do fim, checkpoint no rollback/fechamento/bootstrap; recuperação +7,7 ms | **feito** |
| `wal_append` (~22% do commit pós-T9) | T3, T5.3 (~130 µs fixos por commit sem `fsync`) | T7.1: CRC slicing-by-8 + buffer reaproveitado | `wal_append` −23 a −39%; `mixed_oltp` 1,09×, `create` +4–7% | pequeno; formato igual, teste de compatibilidade | **feito**; juntar escritas por commit fica em T7.2 |
| cópia de página inteira na leitura | T3.3 (3 × 8 KiB por leitura, 27% do motor); T4.2 (`read` −17% a 16 KiB) | — | — | médio | **novo: T15.5** |
| WAL lido inteiro na abertura | T9.4 (134 ms a 10k, cresce com a idade) | — | — | médio (retenção/segmentação) | **novo**, próximo ciclo |
| `crud_full` com payload `fat` | T4.4: `record exceeds the maximum TableHeap page payload` | — | — | grande (overflow records) | registrado; caso de `load-heavy` falha sempre |
| commit fora do tempo de motor no harness | T5.3 (`batch 1`: 741 vs 68.624 "motor") | corrigido nos 3 laços com lote | — | pequeno | **feito** |
| group commit | T8 | — | até 1/c do `fsync` com c escritores | grande (modelo de concorrência) | **adiado** |

**Total do ciclo** (base = motor da abertura do ciclo, `fsync` ×4; final =
T7.3 + T9 + T7.1; produto dos A/B): `mixed_oltp.10k` **~2,97×** (7.024 →
~20.900 ops/s), `snapshot_hold` (`hold`) **~2,8–3,0×**, `crud_full.100k` +10 a
+30% por fase de escrita, `read` igual. Fora do motor: a série histórica
reinicia neste ponto (novas baselines), o harness mede o commit onde ele
acontece, e `--batch`/`--durability`/`--checkpoint-interval` passaram a ter
seletor.

### 10.2 — Baselines

Cinco execuções indexadas de cada caso (um processo e um work dir por
execução), binário final `relwithdebinfo`, ambiente `desktop-windows`,
marcadas em [load-history/baselines.json](../load-history/baselines.json):

| caso | `run_id` da baseline |
|---|---|
| `load.mixed_oltp.embedded.10k` | `run-20260926T141357.119Z-62cd805a` |
| `load.snapshot_hold.embedded.10k` | `run-20260926T141401.602Z-581b26cc` |
| `load.crud_full.embedded.100k` | `run-20260926T141407.612Z-66c525ce` |
| `load.create_only.embedded.100k` | `run-20260926T141417.245Z-0c008744` |

Uma primeira rodada de 20 execuções não entrou no índice: com `--case`, a
campanha não atribui ambiente e o rollup rejeita o ponto (`rollup sem
'environment'`). É preciso passar `--environment` junto de `--case` para
indexar.

### 10.3 — Gates

Os quatro casos têm série suficiente para o gate pontual (mediana das 5
anteriores); a deriva precisa de ~25 pontos e está `insufficient`. Comandos,
por métrica e fase:

```text
modb_load gate --case load.mixed_oltp.embedded.10k    --metric ops_per_second --phase mixed_oltp
modb_load gate --case load.snapshot_hold.embedded.10k --metric ops_per_second --phase hold
modb_load gate --case load.crud_full.embedded.100k    --metric ops_per_second --phase update_shrink
modb_load gate --case load.create_only.embedded.100k  --metric ops_per_second --phase create
```

Todos passam agora. **Não são gate de CI:** `desktop-windows` está marcado como
ruidoso em `environments.json`, e este ciclo mediu deriva de ~7% entre sessões
na própria máquina. Viram gate de verdade num ambiente calibrado.
