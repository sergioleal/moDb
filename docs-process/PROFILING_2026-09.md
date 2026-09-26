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
T7.3 + T9 + T7.1): `mixed_oltp.10k` **~2,65×**, `snapshot_hold` (`hold`)
**~3,5×**, `crud_full.100k` +6 a +34% por fase. *(Corrigido depois da T11:
a primeira versão dizia ~2,97× e ~2,8–3,0×, medidos sob power throttling — ver
"P1 revisado".)* Fora do motor: a série histórica
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

---

# Ciclo P2

## T11 — Causa de M5 (contaminação por ordem)

M5 foi medido em julho, antes da A2 e do ADR-022: dentro de um mesmo
processo, um caso de 100k antes cortava pela metade a vazão do caso de 10k
seguinte, de forma estável. Três candidatos: estado do processo
(heap/allocator), cache de escrita do SO, metadados NTFS do work dir.

Experimentos (`create_only`, `relwithdebinfo`, 3 repetições de cada):

| exp. | processos | sequência | isola |
|---|---|---|---|
| E1 | 1 | 10k ×6 | controle |
| E2 | 1 | 100k, depois 10k ×5 | M5 original |
| E3a | 2 | processo A: 100k; processo B: 10k no **mesmo** work dir | SO e NTFS, sem estado de processo |
| E3b | 2 | processo A: 100k; processo B: 10k em work dir **novo** | só cache do SO |

### Predição (escrita antes de rodar)

M5 continua existindo dentro do processo: em **E2 os casos de 10k ficam ≥30%
abaixo de E1**. Entre processos ele some: **E3a e E3b ficam dentro de ±10% de
E1**. Isso aponta estado de processo (allocator), a hipótese mais antiga.
*Refutação:* se E3a ou E3b ficarem lentos, é SO/NTFS; se E2 não ficar lento,
M5 deixou de existir com as otimizações.

### Resultado — a predição acertou o "onde" e errou o "porquê"

**11.1/11.2 — o efeito existe e é de processo.** Três repetições de cada
experimento (vazão de `create` dos casos de 10k, em k ops/s):

| exp. | resultado |
|---|---|
| E1 (10k ×6, um processo) | ~73k, estável |
| E2 (100k, depois 10k ×5, um processo) | **26k–54k** (média ~38k, **−48%**), sem recuperar |
| E3a (100k e 10k em processos separados, mesmo work dir) | 65k–73k |
| E3b (idem, work dir novo) | 65k–75k |

Como previsto, M5 existe dentro do processo e some entre processos. Mas a
causa não é o allocator:

- o RSS volta a 33 MB depois do caso de 100k (igual a E1) — não há memória
  retida;
- com `stage-profile`, a queda é **uniforme**: todo estágio fica 2,2–2,6× mais
  lento, inclusive CPU pura (`object_encode` 916 → 2.380 ns/op) e o próprio
  laço do harness (2,4×). Fragmentação ou um contêiner inchado deixariam
  alguns estágios mais lentos, não todos;
- o efeito é intermitente: uma sequência de 3 rodadas de "100k + 10k ×3" não
  o mostrou, e rodadas seguidas da mesma sequência mostraram;
- **um processo que roda só casos de 10k também cai**: `10k ×25` despencou de
  ~78k para ~30k a partir do 10º caso — ~1,3 s de vida do processo, nenhum
  caso de 100k. O caso de 100k só fazia o processo durar o bastante.

**11.3 — causa: power throttling do Windows (EcoQoS).** O Windows rebaixa
threads de processos de console em segundo plano, e o processo inteiro passa
a rodar mais devagar. Teste direto: o `modb_load` passou a pedir para não ser
rebaixado (`SetProcessInformation(ProcessPowerThrottling)`, com
`EXECUTION_SPEED` controlado e desligado). `10k ×25`, 4 execuções de cada,
alternadas:

| modo | casos abaixo de 50k (de 25) | menor vazão |
|---|---|---|
| throttling permitido (`MODB_LOAD_ALLOW_POWER_THROTTLING=1`) | **16, 16, 17, 17** | 24k–28k |
| opt-out (novo padrão) | **0, 0, 0, 0** | 57k–63k |

**11.4 — não é hipótese de produto do motor**, e sim do ambiente de medição;
mas vale para qualquer serviço Windows de longa vida que rode como processo
de console em segundo plano: ele deve fazer o mesmo opt-out.

**Consequência para o ciclo P1.** Toda medição com processo de mais de ~1,3 s
pode ter rodado em parte rebaixada. Os A/B do P1 alternaram as variantes, mas a
variante mais lenta vive mais e passa mais tempo rebaixada, o que **tende a
inflar os ganhos**. Os números de T7.3/T9/T7.1 foram refeitos com o opt-out
nos dois lados — ver "P1 revisado" abaixo.

## T15.5 — Leitura sem copiar a página inteira

T3.3 mostrou `buffer_pool_hit` copiando 24.466 B por leitura de um registro de
375 B: `IdentityMap::find` lia a página IDMP por valor (1 cópia) e
`TableHeap::read` copiava a página para um buffer de rascunho e de novo para um
`SlottedPage` (2 cópias). Mudança: `PageFile::view(PageId)` devolve um
`const Page*` para a página no buffer da transação ou no cache, válido até a
próxima chamada ao `PageFile`; `SlottedPage::read_in`/`generation_in` leem o
registro direto de uma página que não possuem; `find`, `find_at` e
`TableHeap::read` passam a usar a visão. Os caminhos que escrevem a página
continuam copiando.

### Predição (escrita antes de rodar)

`buffer_pool_hit` na fase `read` cai de ~24.500 para ~0 bytes copiados por
operação. Tempo de motor da leitura **20–30% menor** (as cópias eram ~27% dele);
vazão de parede de `read` +6–10% (o harness é ~70% da fase). Nas outras fases
e em `mixed_oltp`, dentro de ±5%. *Refutação:* motor de `read` melhorando menos
de 10% — o custo estaria no acesso ao cache, não na cópia.

## P1 revisado — os ganhos sem power throttling

Mesmo desenho do A/B da T9, agora com o opt-out de throttling nas três
variantes e o binário de **antes do ciclo** (`8aee7f1`, worktree separado, só
com o opt-out aplicado) como base. 5 repetições alternadas
([dados](profiling/2026-09/p1-rev-summary.md)). Houve dois outliers isolados
(uma repetição da variante final de `mixed_oltp` a 8.235 contra ~18k nas
outras; uma de `hold` com intervalo 1 a 270), então a tabela usa **medianas**:

| caso · fase | antes do ciclo | T7.3+T7.1 (intervalo 1) | final (T7.3+T7.1+T9) | total | só T9 |
|---|---|---|---|---|---|
| `mixed_oltp.10k` · `mixed_oltp` | 6.755 | 7.812 | 17.935 | **2,65×** | 2,30× |
| `snapshot_hold.10k` · `hold` | 722 | 1.010 | 2.532 | **3,51×** | 2,51× |

Em `crud_full.100k` (médias, CV ≤ 9% na variante final): `create` 1,16×,
`read` 1,11×, `update_inplace` 1,06×, `update_grow` 1,10×, `update_shrink`
1,34×, `delete` 1,25×.

**Correção:** o "~2,97×" em `mixed_oltp` publicado no resumo da T10 (produto
dos A/B feitos sob throttling) estava **inflado em ~12%**; o número é
**~2,65×**. O de `snapshot_hold` ficou maior (3,5× contra ~2,8–3,0×). A
direção e a ordem de grandeza de todas as conclusões se mantêm; os números da
T10 e do ADR-022 foram atualizados.

### Resultado da T15.5

A/B alternado, 5 repetições, com opt-out de throttling nas duas variantes
([dados](profiling/2026-09/t15-5-summary.md); estágios em
[t15-5-sp](profiling/2026-09/t15-5-sp-summary.md)):

| caso · fase | parede antes → depois | motor antes → depois |
|---|---|---|
| `crud_full.100k` · `read` | 112.824 → 125.620 (**1,11×**) | 390.746 → 590.104 (**1,51×**) |
| `crud_full.100k` · `update_inplace` | 75.491 → 93.162 (1,23×) | 1,25× |
| `crud_full.100k` · `update_grow` | 68.614 → 79.808 (1,16×) | 1,18× |
| `crud_full.100k` · `update_shrink` | 62.372 → 70.668 (1,13×) | 1,14× |
| `crud_full.100k` · `delete` | 177.479 → 209.394 (1,18×) | 1,18× |
| `crud_full.100k` · `create` | 59.658 → 65.804 (1,10×) | 1,15× |
| `read_hotspot.100k` | 42.214 → 43.770 (1,04×, CV 11%) | 1,15× |
| `mixed_oltp.10k` | 17.074 → 18.498 (1,08×) | 1,08× |

**Confirmada, e maior que o previsto.** O motor de `read` ficou 51% mais rápido
(previsto 20–30%); a parede, +11% (previsto 6–10%). `buffer_pool_hit` caiu de
24.466 para **8.192 bytes copiados por leitura** — sobra uma cópia, a de uma
página que o caminho de leitura ainda lê por valor (candidata: a resolução do
diretório do `IdentityMap`). **Errado na direção boa:** a predição dizia que as
outras fases ficariam dentro de ±5%, e elas ganharam 10–25% — update e delete
também leem o objeto (e o `IdentityMap`) antes de mudá-lo.

## T26 — Abrir o banco sem ler o WAL inteiro, e um defeito de integridade

### O defeito encontrado ao desenhar a T26 (anterior a este ciclo, agravado pela T9)

`Wal::open_durable` anexa no **fim do arquivo**. Se a última queda deixou no
fim do WAL um registro rasgado ou as imagens de um commit interrompido, os
commits da sessão seguinte ficavam **atrás** desse lixo — e a recuperação para
no primeiro registro inválido. Antes da T9 isso ficava escondido, porque todo
commit também era checkpointado no arquivo de dados; com o checkpoint
preguiçoso, **uma segunda queda perderia até 63 commits confirmados**.

Vizinho: o `tx_id` recomeça em 1 a cada sessão, e a recuperação decide
"commitado" por `tx_id` no WAL inteiro — uma transação morta que sobrasse no
log poderia ser tomada por commitada se a sessão nova reutilizasse o id.

**Correção:** na abertura, depois da recuperação, o WAL é **cortado no fim do
último registro de commit**, com sync (tudo depois dele é de transação morta);
e `next_tx_id` começa acima do maior `tx_id` visto. Testes em `recovery_test`:

- **W1** (queda com rabo rasgado → reabre → commit B → segunda queda): B
  sobrevive. **Falha sem o corte** (verificado por mutação).
- **W2** (transação morta no fim → reabre → 4 commits → segunda queda): passa,
  mas **passa também sem as duas proteções** — neste arranjo a sessão nova
  regrava as mesmas páginas que a morta tocou e as imagens posteriores
  sobrescrevem as dela. Fica como teste de regressão do cenário, não como
  prova da proteção de `tx_id`.

### A T26 propriamente

A abertura lia o WAL inteiro para a memória, **duas vezes** (em `tx::recover`
e de novo em `Database::open`, para achar o maior LSN) — 134 ms a 10k objetos
(T9.4), crescendo com a idade do banco, porque o WAL nunca é truncado.
Mudança, sem mudar a versão do formato:

- a DBRT ganha `checkpoint_wal_offset` (campo novo no fim; zero em arquivos
  antigos = "desconhecido"), gravado em todo checkpoint com o offset do WAL
  logo depois do último commit coberto, e zerado quando o rollback apaga o WAL;
- `tx::recover` lê só a partir desse offset quando ele é confiável (dentro do
  arquivo, com o primeiro registro ali legível e de LSN posterior ao
  checkpoint); senão, lê tudo, como antes;
- `Database::open` usa o maior LSN que `recover` já calculou, em vez de reler.

### Predição (escrita antes de rodar)

A fase `restart_recovery` de `restart_recovery.10k` cai **pelo menos à
metade** (141 → ≤ 70 ms): a leitura passa a cobrir no máximo ~64 commits em vez
do WAL inteiro, uma vez só. As outras fases e `mixed_oltp` ficam dentro de ±5%
(o checkpoint grava 8 bytes a mais na DBRT, que já é regravada).

### Resultado da T26

A/B alternado, 5 repetições ([dados](profiling/2026-09/t26-summary.md)):

| caso | reinício antes | reinício depois | fator |
|---|---|---|---|
| `restart_recovery.10k` | 125,8 ms | **18,1 ms** | **7,0×** |
| `restart_recovery.100k` | 1.137,2 ms | **60,5 ms** | **18,8×** |

**Confirmada, bem além do previsto** (≥ 2×). O tempo de abrir o banco deixa de
crescer com a idade do WAL: a 100k, o antigo lia ~1 s de WAL; o novo lê só o
trecho depois do último checkpoint.

`mixed_oltp.10k` na mesma rodada mostrou duas repetições da variante nova a
~4.700 ops/s (as outras três a ~19.000). Repetido com 8 repetições alternadas
([dados](profiling/2026-09/t26-mixed-summary.md)): antes 16.121–20.007, depois
17.825–19.773 — **iguais, sem outliers**. Os dois valores baixos foram
intermitência do ambiente, do mesmo tipo dos outliers isolados de "P1
revisado" (uma repetição a 8.235 e outra a 270). A causa desses outliers não foi
isolada; eles aparecem em qualquer binário e somem ao repetir.

## T16 — WAL com imagens de página inteiras

**16.1 — custo por byte vs por commit.** Um commit de `mixed_oltp` grava ~29 KB
de WAL (3,5 imagens de página de 8 KiB, T3.4), e esse número **não depende do
tamanho do registro**: o payload muda os bytes do objeto, não o número de
páginas sujas. Microbenchmark no mesmo disco (append de N bytes + `fsync`,
tamanhos alternados, 5 × 60 medidas):

| bytes antes do `fsync` | 1 KiB | 8 KiB | 29 KiB | 64 KiB |
|---|---|---|---|---|
| mediana por `fsync` | 320 µs | 323 µs | 399 µs | 403 µs |

O `fsync` tem um **piso de ~320 µs**; os 29 KB de um commit acrescentam ~80 µs
(+25%). Somando o que resta de `wal_append` depois da T7.1 (~78 µs por commit
em `mixed_oltp`), um WAL lógico ou delta (~1 KB por commit) economizaria algo
como **~130 µs de um commit de ~400 µs: teto de ~1,3–1,4×** nos workloads que
commitam a cada operação, e bem menos com `batch=1000`.

**16.2/16.3 — decisão: não agora.** O teto é real mas moderado, e o custo é
grande: muda o formato do WAL, a recuperação (que hoje só copia páginas
inteiras e é idempotente por construção) e as réplicas (ADR-016/020, que
aplicam as mesmas imagens). Sai deste ciclo com o teto medido; se entrar,
entra com ADR próprio. O piso de ~320 µs por `fsync` é a durabilidade em si:
nenhuma mudança de formato o remove — só agrupar commits (T8).

## T12 — Workloads ainda não perfilados depois das otimizações

`stage-profile`, 3 repetições, 10k objetos: `range_scan_sweep`,
`cascade_delete`, `blob_lifecycle`, `oversubscribed_churn`, `restart_recovery`
e o alvo `loopback` (`create_only` e `crud_full`). `remote_colocated` precisa de
servidor em outra máquina (`run-remote-load.ps1`) e fica fora.

### Predição (escrita antes de rodar)

Nenhuma fase tem um estágio isolado acima de 50% do tempo de motor, **exceto**
as que commitam a cada operação (onde `wal_sync` domina, como em
`mixed_oltp`), e `oversubscribed_churn`, onde `buffer_pool_miss` deve ser o
maior estágio (cache de 10% do necessário). No `loopback`, a vazão de
`create_only` fica abaixo da metade da do embedded, porque cada objeto cruza o
protocolo.

### Resultado da T12

([dados](profiling/2026-09/t12-summary.md); cascade/blob depois da correção
em [t12b](profiling/2026-09/t12b-summary.md))

| workload · fase | vazão | maiores estágios (% do tempo de motor) |
|---|---|---|
| `range_scan_sweep` · varredura por índice 0,01% / 1% / 100% | 16.042 / 323.756 / 401.060 linhas/s | — (a varredura não cronometra por operação) |
| `oversubscribed_churn` · `delete` (cache em 10%) | 188.620 ops/s | `tx_commit` 30%, `heap_record_read` 23%, `buffer_pool_miss` 23% |
| `oversubscribed_churn` · `create` | 67.208 ops/s | `tx_commit` 23%, `object_encode` 12%, `wal_append` 10% |
| `cascade_delete` · `create_hierarchy` / `cascade_delete` | 185.960 / 517.397 objetos/s | — |
| `blob_lifecycle` · create / read / grow / shrink / delete | 18 / 73 / 12 / 17 / 31 blobs/s | — |
| `create_only` · `loopback` | 117.950 ops/s (embedded: 61.144) | — |

**Predição: parcialmente refutada.**

- Nenhum estágio isolado passa de 50%, como previsto. Mas em
  `oversubscribed_churn` o maior custo do `delete` ainda é o commit (30%), não
  `buffer_pool_miss` (23%): com o `view` da T15.5 e o read-ahead, 10% de cache
  basta para o miss não dominar.
- **`loopback` é mais rápido que o embedded, não mais lento.** A predição
  supunha um objeto por mensagem; o alvo usa `CreateBatch`, um lote por
  `--batch`, e a validação do harness fica fora do laço cronometrado do lado
  cliente. Os dois números não medem a mesma coisa e não devem ser comparados.
- A varredura mais seletiva (0,01%) processa 25× menos linhas por segundo que a
  completa: há um custo fixo por varredura (~60 µs) que domina quando ela
  devolve poucas linhas.

**Três defeitos do harness encontrados:**

1. `cascade_delete` e `blob_lifecycle` preenchiam `operations` e `duration_ns`
   mas **nunca `ops_per_second`**, que ficava 0 — na série histórica esses
   workloads nunca tiveram vazão. Corrigido (7 fases).
2. As mesmas fases não registram bytes de WAL (`wal_bytes` = 0). Não corrigido;
   anotado.
3. O `measure_load.py` aceitava um caso que falhou com `case_error` (alvo
   `loopback` sem `crud_full`), porque o processo sai com 0 e a campanha fecha
   `completed`. Agora falha alto.

## T14 — Resto do caminho de update

Dados: `stage-profile` de `crud_full.100k` depois da T15.5 e da correção do
harness, em que o tempo de motor já inclui o commit
([t15-5-sp](profiling/2026-09/t15-5-sp-summary.md)).

**14.1 — sem objeto** (T3.2: `heap_candidate_scan` foi a 0,7%).

**14.2 — bytes por update.** Cada update grava **uma página inteira** no
buffer da transação — `heap_page_write` 8.354–8.875 bytes por operação — para
um registro de 167 B (`shrink`), 359 B (`inplace`) ou 615 B (`grow`): 14× a 50×
de amplificação dentro do motor (é `memcpy`, não disco). No WAL, amortizado por
lote de 1.000, são 954 / 1.721 / 2.656 B por operação: `update_shrink` suja
~325 páginas distintas por commit, porque o registro encolhido muda de página.

**14.3 — cobertura e o que sobra.** As folhas somam **59%** do tempo de motor
em `update_inplace` e `update_grow` e **72%** em `update_shrink`; o resto é o
tempo exclusivo dos envelopes (`identity_lookup`, `heap_record_read`,
`tx_commit`) e o não atribuído.

| fase | motor ns/op | maiores estágios |
|---|---|---|
| `update_inplace` | 11.837 | `tx_commit` 24%, `identity_lookup` 9%, `object_encode` 9%, `wal_append` 9% |
| `update_grow` | 12.706 | `tx_commit` 28%, `wal_append` 12%, `identity_lookup` 9%, `object_encode` 8% |
| `update_shrink` | 14.526 | `tx_commit` 32%, **`heap_candidate_try` 16%**, **`buffer_pool_miss` 15%**, `wal_append` 13% |

**Conclusão:** nenhum estágio domina sozinho o update; o commit é o maior
(24–32%) e já foi atacado. O alvo novo é o `update_shrink`: quando o registro
encolhido não fica no lugar, `heap_candidate_try` custa ~13,5 µs por chamada e
lê páginas candidatas do disco (`buffer_pool_miss`). Fica anotado; não é
corrigido neste ciclo.

## T15 — Dívidas de CPU no caminho de leitura (Fase 10C)

Tetos a partir da atribuição da fase `read` depois da T15.5 (tempo de motor
~1.700 ns/op em `crud_full.100k`; [t15-5-sp](profiling/2026-09/t15-5-sp-summary.md)):
`object_decode` 406 ns (~24%), `materialize` 289 ns (~17%).

| item | teto medido | decisão |
|---|---|---|
| 15.1 `std::function` por campo no Binding | uma fração de `materialize` (17%): 7 campos a poucos ns por chamada indireta — **≤ ~2%** | não vale: não implementado |
| 15.2 zero-copy em `to_field_values` / strings na leitura | o tipo do usuário é dono das strings, então a cópia final é inevitável; dá para tirar a intermediária (`AttributeValue`), **≤ ~6%** | não agora |
| 15.3 `Handle::get<Member>()` materializa o objeto inteiro | só existe no padrão "ler um campo"; nenhum workload o exercita. Por objeto de 7 campos, decode + materialize ≈ 700 ns contra ~100 ns de um campo | adiado até um workload usar o padrão |
| 15.4 append de coleções O(n) | ver abaixo | **feito** |
| 15.5 cópia de página inteira na leitura | 27% do motor | **feito** (ver T15.5) |

### 15.4 — `PersistentVector::push_back` incremental

`push_back` lia o blob inteiro e o regravava inteiro: O(n) por inserção, O(n²)
para montar um vetor. O próprio código registrava a dívida ("fica para a Fase
10, com medição"). O `BlobStore` ganha `append` (completa a última página e
encadeia páginas novas), `overwrite_prefix` e `read_prefix`; `push_back` lê só
o `count`, acrescenta o elemento e regrava o `count`. O formato em disco
(`count u32 | elemento...`) não muda. Teste novo em `blob_store_test`: appends
de tamanhos variados cruzando fronteiras de página resultam no mesmo blob que
gravar tudo de uma vez; `overwrite_prefix` muda só o começo.

**Sem predição pré-registrada** — ao contrário das outras tarefas, a medição
do "depois" rodou antes deste texto. A expectativa (não registrada a tempo) era
≥ 10× a 20.000 elementos.

Microbenchmark (tempo médio de um `push_back` na altura `n`, `int64`, uma
transação), duas rodadas alternadas:

| n | antes | depois |
|---|---|---|
| 100 | 2,5–5,4 µs | 2,7–2,8 µs |
| 1.000 | 30,7–51,9 µs | 2,9–4,1 µs |
| 5.000 | 34,1–36,2 µs | 4,9–5,0 µs |
| 10.000 | 67,5–75,3 µs | 8,6–10,4 µs |
| 20.000 | 145,3–201,9 µs | **12,9–13,4 µs** |

**Resultado: 12–15× a 20.000.** O que ainda cresce é a caminhada pela cadeia
até a última página (~22 páginas a 20.000 elementos), O(páginas) e não
O(elementos); guardar o id da última página eliminaria até isso.

---

# Ciclo P3

## T18 — Excluir o ponto contaminado das análises

A série é append-only (§13.2 do plano de carga), então o ponto medido de
propósito sob M5 não é apagado. Novo: `load-history/excluded_runs.json`, lido
por `compute_trend` (e, por ele, pelo gate): cada entrada tem `run_id`, um
`case_id` opcional e o motivo, e o ponto correspondente vira `comparable=false`.
O `case_id` importa: aquela execução rodou dois casos, e só o segundo estava
contaminado — o primeiro (100k, primeiro caso do processo) continua comparável.
Teste em `load_history_test`: o ponto excluído continua na série, não reprova o
gate, e um `excluded_runs.json` ilegível é erro, não "nada excluído".

## T19 — `device_class`

Fato da máquina, então declarado por ambiente em `loadtests/environments.json`
(`desktop-windows`: `nvme` — WD PC SN740, `BusType NVMe`), e o rollup passa a
emiti-lo em vez de `null`. Não entra no `series_key`: a chave não muda.

## T28 — `--case` sem ambiente não indexava

Com exatamente **um** ambiente `local` não deprecado no catálogo, ele vira o
padrão dos casos sem `--environment`; com zero ou vários, nada é escolhido
(adivinhar misturaria séries). Teste em `load_campaign_test` para os dois
casos.

## T27 — Registros maiores que uma página

**Decisão:** sem registro de overflow por enquanto; registros acima do que
cabe numa página continuam recusados (`record_too_large`), e o limite fica
documentado no plano de carga. `crud_full` com `payload=fat` saiu do
`load-heavy` (falhava sempre), com um teste que impede a volta.

## T30 — A última cópia de página por leitura

Era a resolução do diretório do `IdentityMap` (`resolve_idmp`), que lia a página
IDMD por valor para tirar um ponteiro de 8 bytes; passa a usar
`PageFile::view`. Bytes copiados por leitura: **8.192 → 0**;
`identity_lookup` 547 → 249 ns. **Sem predição pré-registrada.** A/B
alternado, 5 repetições, `crud_full.100k` ([dados](profiling/2026-09/t30ab-summary.md)):

| fase | parede | motor |
|---|---|---|
| `read` | 1,03× | **1,23×** |
| `update_inplace` | 1,10× | 1,11× |
| `update_grow` | 1,09× | 1,10× |
| `update_shrink` | 1,07× | 1,08× |
| `delete` | 1,15× | 1,15× |
| `create` | 1,00× | 1,01× |

## T31 — `wal_bytes` em `cascade_delete`/`blob_lifecycle`

As 7 fases passam a registrar o tamanho do WAL no fim da fase, como as demais.

## T32 — Outliers intermitentes

Investigado sem conclusão. O Defender está fora: a proteção em tempo real está
**desligada** e o processo dele nem roda. Em 15 repetições de
`mixed_oltp.10k`, com os três processos que mais usaram CPU durante cada uma
registrados, **nenhum outlier** (17.492–19.695 ops/s); o que concorre é o
esperado numa máquina de trabalho (o app do Claude, CLion e seu `fsnotifier`,
Teams). Nas rodadas anteriores eles apareceram em ~2 de ~60 execuções. É mais
um motivo para gates só em ambiente dedicado.

## T20 — Regressão de `delete` do índice de capacidade

O `delete` de `crud_full.100k` hoje: **34% `buffer_pool_miss`**, 24% commit,
36% `heap_record_read` (envelope que contém as páginas lidas). A manutenção do
`std::set` de capacidades não tem estágio próprio e está dentro dos 30% não
atribuídos — um teto, não um custo medido. **Decisão: não trocar o índice
agora**; o custo dominante do `delete` são páginas lidas do disco (o `delete`
percorre os objetos numa ordem que o cache não acompanha), e esse é o alvo se
o `delete` voltar a importar.

## T21 — Custo real da retenção MVCC

`snapshot_hold` já lia todo o working set pela snapshot duas vezes — antes do
churn (nada retido) e depois (1/3 dos objetos lidos da versão `previous`, 1/3
removidos) — mas nenhuma das duas era cronometrada. Agora são as fases
`snapshot_read_fresh` e `snapshot_read_retained`; `hold` não muda.

**Predição (escrita antes de rodar):** a leitura com retenção fica no máximo
20% mais lenta; acima de 50%, a retenção tem custo real.

3 repetições ([dados](profiling/2026-09/t21-summary.md)), motor ops/s:

| escala | sem retenção | com retenção | diferença |
|---|---|---|---|
| 10k | 886.637 | 802.957 | −9% |
| 100k | 804.918 | 837.045 | +4% |

**Confirmada.** Ler sob retenção custa o mesmo que ler sem ela, dentro do
ruído — a versão `previous` é resolvida pela mesma entrada do `IdentityMap`.
Junto com a T5/H5 (a fase `hold` é commit, não retenção), **a retenção MVCC não
tem custo mensurável nem na escrita nem na leitura** nestes workloads.

## T23, T24, T25 — Opcionais: não necessárias agora

Cada uma tinha, no plano, a condição em que valeria. Nenhuma se cumpriu:

- **T23 (atribuição por função, gprof/perf):** "só se sobrar resíduo de motor
  sem explicação". A leitura fecha em 88% do tempo de motor (T6) e, no update,
  o que falta está no tempo exclusivo dos envelopes (T14), que já diz onde
  procurar. Um profiler amostral não mudaria nenhuma decisão deste ciclo.
- **T24 (histograma log₂ por estágio):** "só se as caudas virarem a pergunta".
  Nenhuma decisão deste ciclo dependeu de p99/p999 por estágio; `max_ns`
  bastou.
- **T25 (núcleos físicos em Linux):** "quando houver ambiente Linux calibrado".
  Não há.

## T22 — Calibração por classe de build

`default_calibration_path()` passa a preferir
`loadtests/calibration/<plataforma>-<CMAKE_BUILD_TYPE>.json` e cai no arquivo sem
sufixo — o legado, agora declarado `"build_type": "Debug"` — quando não há um
específico. `scripts/calibrate_load.py` gerou
`windows-x86_64-RelWithDebInfo.json` com **todas as escalas medidas** (uma
execução por ponto, processo e work dir próprios, opt-out de throttling);
nenhuma extrapolada. Duração do caso, em segundos:

| workload | 1k | 10k | 100k | 250k | 500k | 1M |
|---|---|---|---|---|---|---|
| `create_only` | 0,02 | 0,17 | 1,54 | 4,05 | 7,64 | 15,91 |
| `create_delete_forward` | 0,02 | 0,17 | 1,78 | 4,61 | 9,08 | 18,68 |
| `create_delete_reverse` | 0,02 | 0,17 | 1,76 | 4,20 | 8,71 | 18,19 |
| `create_delete_interleaved` | 0,02 | 0,17 | 2,00 | 4,76 | 10,25 | 20,71 |
| `crud_full` | 0,06 | 0,55 | 5,58 | 14,57 | 28,88 | 59,29 |

O crescimento é linear de 10k a 1M em todos os workloads. A calibração antiga
(Debug, julho) tinha `crud_full` a 250k em ~44 minutos e precisava extrapolar
500k/1M; hoje o caso inteiro a 1M leva menos de um minuto. Conferido:
`list-cases` de um binário `relwithdebinfo` estima 5,6 s para
`crud_full.100k`; um binário Debug continua no arquivo legado (242 s).

## T17 — Limpar `load-results/`

Confirmado com o usuário antes de apagar: removidos os 164 arquivos
`.modb`/`.modb.wal` que sobraram de execuções de julho (**7,6 GB → 332 KB**).
Mantidos os 10 resultados brutos de campanha (`.jsonl`/`.partial`, 0,3 MB), que
o §13.2 do plano de carga trata como imutáveis e a retenção pode consultar.

---

## T29 — `update_shrink`: candidatas lidas do disco

**29.1 — por quê.** O índice de capacidade escolhe a candidata por
*best-fit*: a página com a menor capacidade que ainda comporta o registro, em
qualquer ponto do arquivo. No `update_shrink` o espaço liberado fica espalhado
por páginas antigas, e o cache padrão tem 1.024 páginas (8 MB) contra mais de
5.000 páginas de heap em `crud_full.100k` — a candidata tende a ser uma página
fria. Pela T3, `heap_candidate_try` roda 0,167 vez por operação e
`buffer_pool_miss` 0,099: ~60% das tentativas leem do disco. Além disso,
`try_insert` carrega a candidata com `load_trusted`, que copia a página duas
vezes (scratch e `SlottedPage`).

**Mudança:** (1) entre as até 8 primeiras candidatas em ordem de capacidade,
preferir uma **já residente** (buffer da transação ou cache —
`BufferPool::contains`, sem mexer no LRU); se nenhuma estiver, a primeira, como
antes; (2) `load_trusted` passa a partir de `PageFile::view`, uma cópia a menos
em toda inserção e todo update.

### Predição (escrita antes de rodar)

Em `update_shrink`, `buffer_pool_miss` cai de ~0,10 para menos de 0,04 chamada
por operação, e a fase fica **10–20% mais rápida**; as outras fases de escrita
ganham 0–5% (a cópia a menos). O arquivo cresce no máximo 5%, porque a escolha
deixa de ser best-fit puro. *Refutação:* `update_shrink` melhorando menos de
5%, ou o arquivo crescendo mais de 10%.

### Resultado — as duas hipóteses refutadas, e a causa real

A/B alternado, 5 repetições ([dados](profiling/2026-09/t29-summary.md);
estágios em [t29-sp](profiling/2026-09/t29-sp-summary.md)): `update_shrink`
**×0,998**, e `buffer_pool_miss` **0,099 → 0,099** chamada por operação. A
sondagem das candidatas aconteceu (iterações da varredura 0,98 → 1,52 por
operação), mas os misses não mudaram — e o `delete`, que não tenta candidata
nenhuma, tem 0,081 miss por operação. **Os misses não vêm das candidatas:
vêm da leitura do objeto que vai ser mudado.** A preferência por candidata
residente foi revertida (heurística sem efeito não fica no código).

Segunda hipótese, testada à parte: falta de cache. Um build temporário com
`page_cache_capacity` = 8.192 páginas (8× o padrão) contra o normal
([dados](profiling/2026-09/t29-cache-summary.md)): `update_shrink` ×0,98,
`delete` ×1,03, e `update_inplace`/`update_grow` **9–12% mais lentos**, com 38 MB
a mais de RSS. **Refutada também.**

**A causa:** o arquivo de dados cresce **3,8× durante as três fases de update**,
sem nenhuma snapshot aberta *(corrigido: a primeira versão dizia "com
`retained_versions` = 0", mas esse campo só é calculado no `snapshot_hold` —
ver T33.1)*:

| fase | arquivo | páginas | bytes/objeto |
|---|---|---|---|
| `create` | 41,9 MB | 5.357 | 438 |
| `update_inplace` | 79,1 MB | 10.119 | 828 |
| `update_grow` | 144,2 MB | 18.453 | 1.511 |
| `update_shrink` | 159,6 MB | 20.435 | 1.674 |

Até o update "no lugar" praticamente dobra o arquivo: cada update grava uma
versão nova e mantém a anterior, e nada recupera as anteriores enquanto
`collect_garbage()` não for chamado — mesmo sem snapshot que precise delas. Os
registros ficam espalhados por 20 mil páginas, e nenhum tamanho razoável de
cache acompanha. Isso vira a **T33**.

O que ficou da T29: `load_trusted` com uma cópia de página a menos (de `view`
direto para o `SlottedPage`), neutro a levemente positivo no A/B (`create`
+6%, `update_inplace` +3%, `delete` +4%, `update_grow` −4% com CV 11%).

## T33.1 — `collect_garbage()` entre as fases de update

O GC (`ObjectStore::collect_garbage`) percorre todos os registros do heap e
recolhe (a) a `previous` referenciada quando nenhuma snapshot aberta pode
enxergá-la e (b) "cópias órfãs" — a `previous` antiga que perdeu a referência
quando o objeto foi atualizado de novo. Sem chamada explícita, nada disso é
recolhido. Experimento com um build **temporário** (patch só no diretório de
build do experimento; o fonte não mudou): `crud_full.100k` chamando
`collect_garbage()` depois de cada fase de update, fora do tempo
cronometrado.

### Predição (escrita antes de rodar)

Cada GC recolhe da ordem de 100 mil registros (uma versão morta por objeto). O
espaço liberado é reaproveitado, então o arquivo para de crescer tanto: ~100 MB
depois de `update_grow` em vez de 144 MB, quase sem crescimento em
`update_shrink`. `update_shrink` e `delete` ficam 10–30% mais rápidos. Cada GC
custa 0,3–1 s (varredura completa). *Refutação:* o arquivo crescer igual — o
espaço liberado não estaria sendo reaproveitado.

### Resultado da T33.1 — "ainda não": dois defeitos na frente do GC

`crud_full.100k` com `collect_garbage()` depois de cada fase de update:

| depois de | recolhidos | tempo do GC | arquivo (com GC) | arquivo (sem GC) |
|---|---|---|---|---|
| `update_inplace` | 100.000 | **29,7 s** | 79,1 MB | 79,1 MB |
| `update_grow` | 100.000 | **32,4 s** | 144,2 MB | 144,2 MB |
| `update_shrink` | 100.000 | **64,1 s** | 159,6 MB | 159,6 MB |

`hash_match` verdadeiro (o GC não corrompeu nada) e vazão das fases de update
igual à sem GC. Contra a predição: recolheu o previsto (✅), mas **o arquivo
cresceu exatamente igual** (❌) e **cada GC levou 30–64 s** em vez de 0,3–1 s
(❌).

**Defeito 1 — espaço liberado não é reaproveitado.** `TableHeap::erase`
atualiza o índice de capacidade, mas uma página que fica vazia é **retirada da
cadeia do heap**, e o motor não tem lista de páginas livres (limitação
declarada do MVP em GARANTIAS_TRANSACIONAIS). Depois do `update_inplace`, as
versões originais ocupavam páginas inteiras: o GC as esvazia, elas saem da
cadeia e nunca mais são usadas; as inserções seguintes alocam páginas novas.

**Defeito 2 — transação grande é O(n²) no buffer pool.** O mesmo GC a 10k
leva 56–76 ms; a 100k, 30–64 s: 10× mais registros, **~500–850× mais tempo**.
`BufferPool::evict_until` procura vítima a partir da cauda da LRU **pulando os
frames sujos**, que não podem sair antes do commit. Quando uma transação suja
mais páginas do que o cache comporta (1.024), cada inserção no cache percorre a
lista inteira sem achar vítima — O(páginas sujas) por operação. O `delete`
normal remove os mesmos 100k registros em ~0,4 s porque commita a cada 1.000; o
GC faz tudo numa transação só. **Afeta qualquer transação grande**, não só o GC.

**Conclusão:** um GC automático agora não ajudaria — o espaço não volta e a
coleta é quadrática. Os dois defeitos vêm antes (T33.2 e T33.3 no plano).

**Correção de um texto anterior:** a T29 dizia que o arquivo crescia "com
`retained_versions` = 0". Esse campo só é calculado no `snapshot_hold`; no
`crud_full` ele é sempre 0 e não prova nada. O crescimento em si foi medido.

## T33.3 — Transação grande sem custo quadrático no buffer pool

`BufferPool` passa a manter duas listas: `entries_`, só com frames
evictáveis (limpos e não pinados) em ordem LRU, e `held_`, com os sujos ou
pinados. Toda mudança de `dirty`/`pin` recoloca o frame na lista certa por
`splice` (O(1), sem invalidar o iterador do índice), e despejar é tirar da
cauda de `entries_`. Comportamento observável igual — teste novo em
`buffer_pool_test` com 40 páginas sujas num pool de 4.

Medida pelo mesmo experimento da T33.1 (build temporário com `collect_garbage()`
entre as fases de `crud_full.100k`), antes 29,7 / 32,4 / 64,1 s.

### Predição (escrita antes de rodar)

Cada GC a 100k cai para **menos de 3 s** — perto do linear a partir dos 56–76 ms
medidos a 10k. As fases normais (sem GC) ficam dentro de ±5%. *Refutação:*
acima de 10 s.

### Resultado da T33.3 — confirmada

GC entre as fases de `crud_full.100k`, duas execuções:

| depois de | antes | depois |
|---|---|---|
| `update_inplace` | 29,7 s | **1,12–1,16 s** |
| `update_grow` | 32,4 s | **1,18–1,19 s** |
| `update_shrink` | 64,1 s | **1,33–1,37 s** |

**26–47× mais rápido**, dentro do previsto (< 3 s); o caso inteiro, com os três
GCs, foi de 133 s para 10–11 s. Fases normais, A/B alternado, 5 repetições
([dados](profiling/2026-09/t333-summary.md)): de −1,5% a +6,7%, nenhuma pior
(`mixed_oltp` +6,7%, `update_inplace` +5,1%). O arquivo continua crescendo
igual com o GC — é o defeito 1 (T33.2).

## T33.2 — Reaproveitar as páginas esvaziadas ([ADR-023](../docs/decisions/ADR-023-lista-de-paginas-livres-do-heap.md))

Cada `TableHeap` passa a ter uma lista de páginas livres: a raiz `THRP` ganha
`free_head` (campo no fim, zero em raízes antigas = vazia); a página que o
`erase` esvazia vira uma `SlottedPage` vazia cujo `next_page` aponta para a
próxima livre; e o heap desempilha dali antes de pedir página nova ao arquivo.
Sem tipo de página novo — o `database_check` a vê como página de heap vazia.
Testes em `table_heap_test`: a página esvaziada é a próxima usada, o arquivo não
cresce, a lista sobrevive a reabrir e ao reparo; com o reaproveitamento
desligado (mutação), 4 checagens falham.

### Predição (escrita antes de rodar)

Com `collect_garbage()` entre as fases (experimento da T33.1), o arquivo para
em ~100 MB em vez de 159,6 MB, e `update_shrink` não cresce. Sem GC, nada muda.
*Refutação:* arquivo acima de 130 MB.

### Resultado — confirmada

| depois de | sem lista livre | com lista livre |
|---|---|---|
| `update_inplace` (+GC) | 79,1 MB | 79,1 MB |
| `update_grow` (+GC) | 144,2 MB | **107,0 MB** |
| `update_shrink` (+GC) | 159,6 MB | **107,0 MB** |

Duas execuções, `hash_match` verdadeiro nas duas. Sem GC, A/B alternado contra
o commit anterior, 5 repetições ([dados](profiling/2026-09/t332-summary.md)):
todas as fases de `crud_full.100k` e `create_delete_interleaved.100k` entre
−2,2% e +2,5% — ruído.

O arquivo ainda não **encolhe** (páginas livres ocupam espaço até serem
reusadas), e a lista é por heap: páginas livres do heap de dados não servem ao
`IdentityMap`, aos índices nem ao `BlobStore`.
