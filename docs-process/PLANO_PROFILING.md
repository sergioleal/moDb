# Plano de profiling de desempenho — onde estão os gargalhos

- Estado: **Etapa 0 concluída; H1 confirmada; ação A2 implementada e medida**
  (ver §8, Andamento)
- Resultados medidos e ações sugeridas: [RESULTADOS_PROFILING.md](RESULTADOS_PROFILING.md)
- Desenho da ferramenta (lacunas de cobertura da Etapa 1, caminho de leitura,
  atribuição por classe de operação, dimensão leitura/escrita):
  [PLANO_PROFILER.md](PLANO_PROFILER.md)
- Versão: 1
- Data de abertura: 2026-07-26
- Branch: `subphase/H-calibration-250k`
- Escopo: identificar, com evidência, os gargalhos reais do motor; **não** é
  um plano de otimização — otimização entra depois, com comparação
  antes/depois registrada (§12 de [PLANO_BENCHMARKS.md](../docs/PLANO_BENCHMARKS.md))

## 1. Objetivo e o que este plano não é

Os dois planos existentes respondem perguntas diferentes:
[PLANO_BENCHMARKS.md](../docs/PLANO_BENCHMARKS.md) mede *quanto custa uma
operação*; [PLANO_TESTES_DE_CARGA.md](../docs/PLANO_TESTES_DE_CARGA.md) mede
*se o sistema continua correto e previsível em escala*. Nenhum dos dois
responde **onde**, dentro de uma operação, o tempo é gasto.

Este plano cobre essa terceira pergunta: atribuição de custo. Ele termina
com uma lista ordenada de gargalhos, cada um com evidência numérica, teto
estimado de ganho e custo de correção — insumo para decidir o que otimizar,
não a otimização em si.

Regra herdada e mantida: **nenhuma otimização entra só com perfil.** Precisa
de comparação de benchmark antes/depois registrada.

## 2. Base factual de partida

Extraído de [load-history/series.jsonl](../load-history/series.jsonl) (30
pontos, todos **build Debug/`-O0`**, GCC 15.2.0 MinGW, `desktop-windows`):

| Caso / fase | 10k | 100k | sinal |
|---|---|---|---|
| `create_only` create | 15.767 ops/s | 4.736 ops/s | −3,3× com bytes/objeto constante (460→458) |
| `crud_full` update_inplace | 7.524 ops/s | 2.085 ops/s | p99 226 µs → 876 µs |
| `crud_full` update_grow | 5.161 ops/s | 1.287 ops/s | p99 321 µs → 1.382 µs |
| `crud_full` update_shrink | 3.939 ops/s | 896 ops/s | p99 449 µs → 2.449 µs; WAL 163→326 MB na fase |
| `mixed_oltp` | 1.180 ops/s | 1.121 ops/s | p99 5.487 µs; WAL 1.464 MB / 500k ops ≈ **3 KB/operação** |
| `snapshot_hold` hold | 26,5 s | 264 s | escala 10× com o volume |
| `crud_full` read | 32.296 ops/s | 40.706 ops/s | **não degrada** |
| `range_scan_sweep` 100% | 45.176 linhas/s | 43.848 linhas/s | **plano** |

Leituras encontradas:

1. **O gargalo está na escrita/mutação, não na leitura.** Leitura e varredura
   ficam planas de 10k a 100k; ingestão e update caem de 3× a 4,4×.
2. **`crud_full` a 100k gasta 264 s, dos quais 237 s (90%) são as três fases
   de update.** É o maior bloco de tempo absoluto do conjunto.
3. **Degradação é super-linear e não é amplificação de espaço** — bytes por
   objeto ficam constantes (458). O custo *por operação* cresce com o volume.

## 3. Defeitos de medição corrigidos antes de qualquer perfil

Não são observações sobre os dados: são defeitos no código de medição. Nada
medido antes de corrigi-los é aproveitável.

| # | Defeito | Onde | Consequência |
|---|---|---|---|
| M1 | Toda a série é Debug/`-O0`; só existem `build/debug` e `build/coverage` | — | ranking de hotspot em `-O0` não é o de `-O2` (inlining, `std::function`, wrappers de `Result<T>` pesam de forma desproporcional) |
| M2 | `peak_rss_bytes()` devolve o pico do **processo**, não da fase | [process_metrics.cpp:18](../loadtests/process_metrics.cpp) | monotônico dentro da campanha: 213 MB idêntico em 30 fases seguidas; inútil por fase |
| M3 | `page_size` gravado como **string**, lido como **número** | [campaign.cpp:583](../loadtests/campaign.cpp) ↔ [rollup.cpp:156](../loadtests/history/rollup.cpp) | `page_size: 0` em toda a série **e no `series_key`** — a varredura 4k/8k/16k da Etapa 3 colidiria numa série só |
| M4 | `os`, `cpu_model`, `cores_*`, `ram_gb`, `fs`, `device_class`, `sanitizers` são `null` hardcoded no rollup; `EnvironmentInfo` nem os coleta | [rollup.cpp:290](../loadtests/history/rollup.cpp), [environment.hpp:8](../benchmarks/runner/environment.hpp) | §13.3 exige esses campos para o ponto ser comparável |
| M4b | `os_version` era `std::to_string(GetVersion())` — o DWORD empacotado em decimal (`1717043210`), e a API é deprecada e mente sobre 8.1+ sem manifesto | [environment.cpp](../benchmarks/runner/environment.cpp) | só ficou visível quando M4 fez o campo chegar ao rollup |
| **M5** | **A vazão medida de um caso depende do que rodou antes dele na mesma campanha — efeito de 2×** | metodologia do harness | comparação entre casos dentro de uma campanha é inválida; ver §3.1 |

### 3.1 M5 — contaminação por ordem na campanha

Descoberto ao interpretar o baseline: a fase `create` do `create_only` a 10k
deu 52.639 ops/s, enquanto a **mesma fase** (mesmo `perform_create_phase`,
[target_embedded.cpp:782](../loadtests/target_embedded.cpp) e
[:854](../loadtests/target_embedded.cpp)) nos outros quatro workloads deu
~25.000. Em Debug as duas eram iguais (15.767 vs 15.102), então o efeito
apareceu junto com o build otimizado.

Experimento, cada linha um processo próprio:

| Execução | `create` a 10k |
|---|---|
| `create_only.10k` isolado, primeiro caso | 49.341 ops/s |
| `create_delete_forward.10k` isolado, primeiro caso | 49.998 ops/s |
| 10k logo depois de outro 10k (controle) | 49.668 / 49.974 ops/s |
| **10k logo depois de um caso de 100k** | **24.262 ops/s** |

Um único caso de 100k antes é suficiente para cortar a vazão do caso seguinte
pela metade, e o efeito é **estável** (CV 0,3% nas 3 repetições do baseline),
não um transiente que decai.

Causa ainda não isolada. Candidatas, em ordem de suspeita:

1. **estado de processo** — o heap/allocator cresceu para acomodar 100k objetos
   e, depois do free, ficou com free lists grandes e localidade pior. A
   estabilidade do novo regime favorece esta hipótese: cache de escrita do SO
   drenaria e recuperaria;
2. **cache de escrita do SO** — a campanha deixa ~94 MB (db+WAL) sujos por caso
   de 100k;
3. **metadados NTFS** do diretório de trabalho, que acumula arquivos por caso.

Duas leituras possíveis, e não decidi entre elas com o que tenho:

- **como defeito do harness**: um processo por campanha não é a condição normal
  de uso; números só são comparáveis com um processo por caso;
- **como sinal real do produto**: se um servidor de vida longa degrada 2% depois
  de churn, isso importa de verdade e não é artefato.

Consequência imediata, já adotada: **medição comparável usa um processo por
caso**. O baseline de 30 pontos coletado antes desta descoberta continua válido
para comparar *repetições do mesmo caso* (CV 0,3–6%), mas **não** para comparar
casos entre si.

Consequência declarada de M3: corrigir `page_size` **muda o `series_key`** de
todos os pontos futuros. Isso é intencional — a chave antiga foi calculada
sobre um bug, e a alternativa (comparar corridas de 4k com 16k como se
fossem a mesma série) é pior. `series_key_version` é incrementado para 2 e os
30 pontos existentes permanecem válidos na versão 1, sem reescrita
(a camada histórica é append-only, §13.2).

## 4. Hipóteses

Cada hipótese tem evidência no código e uma predição falsificável.

### H1 — `TableHeap::insert` varre todas as páginas a cada inserção

[table_heap.cpp:353](../src/storage/table_heap.cpp) itera o
`insertion_capacity_by_page_` inteiro (um `std::map`) montando candidatas,
filtrando *dentro* do laço. A 100k objetos o arquivo tem ~11.200 páginas de
4 KiB, contra ~1.120 a 10k.

**Predição:** latência por insert é afim no número de páginas.

Recalibrada com o baseline RelWithDebInfo (cada ponto como primeiro caso do seu
próprio processo, para escapar de M5):

| escala | páginas (4 KiB) | ops/s | µs/op |
|---|---|---|---|
| 10k | ~1.120 | 49.341 | 20,3 |
| 100k | ~11.200 | 15.103 | 66,2 |

Delta: +45,9 µs/op para +10.080 páginas ⇒ **≈ 4,55 ns por entrada do mapa**,
compatível com um pointer chase de `std::map` com ~11k nós fora do L2.

**Pré-registro da varredura de page size** (escrito antes de rodar). Se o custo
é proporcional à contagem de páginas, o termo de 45,9 µs cai com o page size
enquanto o piso de 20,3 µs não muda:

| page size | páginas a 100k | µs/op previsto | ops/s previsto |
|---|---|---|---|
| 4 KiB | ~11.200 | 66,2 (medido) | 15.103 (medido) |
| 8 KiB | ~5.600 | 20,3 + 45,9/2 = 43,3 | ~23.100 |
| 16 KiB | ~2.800 | 20,3 + 45,9/4 = 31,8 | ~31.400 |

**Refutação:** se a vazão a 100k ficar ~15k independentemente do page size, a
degradação não é dirigida por contagem de páginas e H1 está errada.

### H2 — `persist_root()` por registro

Chamado em [table_heap.cpp:314](../src/storage/table_heap.cpp),
[:343](../src/storage/table_heap.cpp), [:426](../src/storage/table_heap.cpp),
[:715](../src/storage/table_heap.cpp), [:783](../src/storage/table_heap.cpp)
— uma escrita extra da página raiz por operação.

**Predição:** escritas de página por objeto ≥ 2 na fase create.

### H3 — WAL grava imagens de página inteiras, com sync por commit

[wal.cpp:324](../src/tx/wal.cpp) (`append_page_image` recebe a página
completa). `mixed_oltp` commita por operação sob mutex: 3 KB de WAL por
operação com objetos de 458 B é exatamente o que a imagem de página inteira
prediz.

**Predição:** varrer `batch` 1/100/1k/10k move o custo por operação em
proporção inversa; `durability=disabled_diagnostic` isola quanto é `fsync`.

### H4 — caminho de update move registros entre páginas

Fase mais lenta de todas (896 ops/s) e a única que dobra o WAL dentro de si
mesma (163 → 326 MB).

**Predição:** bytes movidos e escritas de página por operação muito acima do
tamanho do registro.

### H5 — retenção MVCC no `snapshot_hold`

264 s a 100k; `retained_versions=7666` já registrado a 10k.

### H6 — dívidas de CPU já documentadas

[OTIMIZACOES_10C.md](OTIMIZACOES_10C.md): `std::function` por campo no
Binding, `to_field_values` sem zero-copy, `Handle::get<Member>()`
materializando o objeto inteiro. **Só aparecem em Release** — em `-O0` ficam
escondidas no ruído dos wrappers.

## 5. Ferramental disponível nesta máquina

Verificado, não presumido:

- **Disponível:** `gprof`, `objdump`, `addr2line`, `nm` (MinGW GCC 15.2.0);
  `wpr.exe`; WSL2 com Ubuntu-24.04; presets `profile-8k`/`profile-16k`.
- **Ausente:** MSVC, clang, VTune, AMD uProf, Windows Performance Toolkit
  (`xperf`/WPA), `gdb`.

Consequência: **não há profiler amostral com símbolos para o binário
Windows** — MinGW emite DWARF em PE; VTune e WPA querem PDB. Daí as três
trilhas de atribuição por função:

| Trilha | O que dá | Ressalva |
|---|---|---|
| **A — instrumentação in-process** (espinha dorsal) | atribuição por estágio nomeado, incluindo **espera de I/O e `fsync`**, que profiler amostral de CPU não mostra; integra no JSONL existente | precisa de código |
| **B — WSL + `perf record -g` + flamegraph** | melhor atribuição por função disponível aqui | mede o I/O do Linux (`async_file_linux.cpp`), não o do Windows; hotspots de CPU transferem, I/O não |
| **C — `gprof` (`-pg`)** | ranqueamento barato no próprio Windows | precisão medíocre com inlining de `-O2`; serve para ordenar, não para medir |

## 6. Etapas

### Etapa 0 — Tornar a medição confiável *(pré-requisito)*

1. Preset `relwithdebinfo` (`-O2 -g3 -fno-omit-frame-pointer`) + rebuild de
   `modb_load`/`modb_bench` — corrige M1.
2. RSS por fase — corrige M2.
3. `page_size` como número + `series_key_version` = 2 — corrige M3.
4. Coleta e emissão de CPU/cores/RAM/OS/sanitizers — corrige M4.
5. Baseline de referência: 3 repetições, seed fixa, máquina ociosa.

Nota de comparabilidade: `environments.json` marca `desktop-windows` como
*"noisy, don't use for gating"*. Vale como diagnóstico comparativo consigo
mesmo, nunca como gate.

### Etapa 1 — Atribuição grossa: CPU vs I/O vs fsync

Timers de escopo com taxonomia fixa, emitidos como registro `stage_profile`
no JSONL (aditivo ao schema, sem quebrar o dashboard):

`encode` · `heap_candidate_scan` · `heap_page_write` · `persist_root` ·
`wal_append` · `wal_sync` · `buffer_pool_miss` · `buffer_pool_writeback` ·
`index_update` · `identity_map` · `validate`

Contadores no mesmo registro: iterações do laço de candidatas, escritas de
página por operação, bytes de WAL por operação, contagem e tempo de `fsync`,
hit/miss/eviction do buffer pool.

**Critério de aceite:** a soma dos estágios cobre ≥ 90% da duração da fase;
o resíduo não atribuído é gravado explicitamente, nunca absorvido.

### Etapa 2 — Atribuição por função

Trilha A vem da Etapa 1. Adicionalmente `gprof` no Windows e, se aprovado,
`perf` em WSL. Artefatos ligados a `run_id`/`scenario_id` com hash, conforme
§12 de [PLANO_BENCHMARKS.md](../docs/PLANO_BENCHMARKS.md).

Cenários em ordem de prioridade:

1. `load.create_only.embedded.100k` → H1, H2, H3
2. `load.crud_full.embedded.100k`, fases `update_grow`/`update_shrink` → H4
   *(maior bloco de tempo absoluto: 189 s)*
3. `load.mixed_oltp.embedded.100k` → H3 + contenção do mutex
4. `load.snapshot_hold.embedded.100k` → H5
5. `object_store.read_hotpath` (via `modb_bench`) → H6, só visível em Release

### Etapa 3 — Experimentos dirigidos (leis de escala)

| Varredura | Isola | Refuta/confirma |
|---|---|---|
| scale 1k/10k/25k/50k/100k/250k em `create_only` | expoente de crescimento | H1 |
| `--batch` 1/100/1k/10k | custo de commit | H3 |
| `durability` `sync_real` vs `disabled_diagnostic` | custo puro de `fsync` | H3 |
| page size 4k/8k/16k (presets existentes) | custos por contagem de páginas | H1, H2 |
| `cache` `warm` vs `oversubscribed` | buffer pool | — |
| `payload` slim/normal/fat | custo por byte vs por operação | H4, H6 |

A varredura de page size é a mais barata e a mais decisiva: zero linhas de
código, e depende de M3 estar corrigido para não colidir a série.

### Etapa 4 — Relatório e gates

`docs-process/PROFILING_<tag>.md`: por gargalo, hipótese, evidência, perfil,
**teto estimado se corrigido**, custo da correção, decisão. Mais casos de
`modb_load gate` para as métricas escolhidas, para regressão futura falhar
sozinha.

## 7. Riscos

1. `gprof` com inlining de `-O2` atribui mal funções pequenas — serve para
   ranquear, não para medir.
2. `perf` no WSL2 costuma exigir compilar o `perf` do kernel; contadores PMU
   de hardware não estão disponíveis por padrão (eventos de software bastam
   para amostragem por tempo).
3. Sem WPA, ETW só responde no nível do processo ("é CPU ou é disco?"), sem
   símbolos.
4. Máquina de desenvolvimento declaradamente ruidosa: comparações precisam de
   ≥ 3 repetições e ordem A/B alternada.
5. A calibração já avisa que estimativas acima de 100k são otimistas
   (throughput caiu 2,4×–5,1× só de 10k para 100k) — a varredura até 250k
   pode demorar mais que o previsto.

## 8. Andamento

Legenda: ⬜ não começado · 🔄 em andamento · ✅ concluído · ⛔ bloqueado

| Item | Estado | Nota |
|---|---|---|
| **Etapa 0** | ✅ | 138/138 testes passam no preset novo |
| 0.0 Documento de plano e andamento | ✅ | este arquivo |
| 0.1 Preset `relwithdebinfo` (M1) | ✅ | + preset `gprof`; presets `profile-8k/16k` passaram a herdar dele |
| 0.1b Descoberta de binário nos scripts (M1) | ✅ | `run-load`/`run-benchmarks` preferiam `build\debug`; agora otimizado primeiro, com aviso ao cair em Debug |
| 0.2 RSS por fase (M2) | ✅ | `RssTracker` amostrado; 15 sítios convertidos |
| 0.3 `page_size` numérico + `series_key_version`=2 (M3) | ✅ | guarda de regressão em `tests/load_history_test.cpp` |
| 0.4 Coleta CPU/cores/RAM/OS/fs/instrumentação (M4) | ✅ | `device_class` segue não coletado (ver abaixo) |
| 0.5 Baseline RelWithDebInfo, 3 repetições | ✅ | 30 pontos, CV 0,3–6%; válido entre repetições, não entre casos (M5) |
| **Etapa 1** — `stage_profile` | ✅ | 18 estágios (13 folhas + 5 envelopes), caminho de leitura e escrita; `mixed_oltp` fecha em 94,8%. `create` (48%) e `read` (17%) não fecham, e a instrumentação mostra que o resíduo está no harness, não no motor ([PLANO_PROFILER.md §9.3](PLANO_PROFILER.md)) |
| **Etapa 2** — atribuição por função | ⬜ | **provavelmente desnecessária agora** — ver abaixo |
| **Etapa 3** — varreduras | 🔄 | page size ✅ (§9.1); faltam batch, durability, cache, payload, scale até 250k |
| **Etapa 4** — relatório e gates | ⬜ | |

Resultados medidos: [RESULTADOS_PROFILING.md §4.1](RESULTADOS_PROFILING.md).

**Ação A2 implementada e medida** ([RESULTADOS_PROFILING.md §4.2](RESULTADOS_PROFILING.md)):
o laço de candidatas virou um índice ordenado por capacidade
(`std::set<std::pair<capacidade, PageId>>`, `lower_bound` em O(log n), para na
primeira candidata). `crud_full.embedded.100k`: 105,6 s → ~30 s (3,5×). Dois
falsos começos medidos e descartados no caminho (uma poda por "capacidade > 0"
que não podava quase nada e regrediu 11×; um índice ordenado correto mas que
coletava candidatas demais e regredia `update_shrink`). Contrapartida aceita:
`delete` ~22% mais lento (custo de manter o índice). `hash_match=True` em
todas as verificações; 138/138 testes em `relwithdebinfo`, `stage-profile` e
`sanitizers`.

**Correção retroativa dos números de cobertura (2026-08-02).** Ao executar
[PLANO_PROFILER.md](PLANO_PROFILER.md) descobriu-se que `heap_candidate_try`
continha `heap_page_write` e `persist_root` (o lambda `try_insert` faz as três
coisas), então **os percentuais de cobertura citados acima e nos resultados
anteriores contavam tempo duas vezes**. Os 93% de update eram, de verdade,
66–86%. A taxonomia agora separa folhas de envelopes e `attributed_ns` só soma
folhas; um teste falha se a fração passar de 1,0. Nenhum número de *vazão* muda
— só a atribuição.

**Hipótese nova, vinda da Etapa 1 e não prevista em H1–H6.** O commit reabre o
WAL a cada transação (`Wal::open_durable`, 148 µs/op em `mixed_oltp`) e chama
`fsync` do arquivo de dados 3–4 vezes por commit (`page_file_sync`, 87 µs/op).
Juntos são ~68% do custo de `tx_commit`, que por sua vez é 97% da fase. Ver
[PLANO_PROFILER.md §9.2](PLANO_PROFILER.md).

**Reordenação depois da Etapa 1.** O gargalo está localizado numa linha
([table_heap.cpp:353](../src/storage/table_heap.cpp)) com 80–88% das fases de
update. A Etapa 2 (gprof/`perf`) existia para achar hotspots por função — e o
hotspot já está achado, com um contador provando que o laço não produz nada. Ela
passa a ser opcional: vale se, depois de corrigir a varredura, o resíduo não
atribuído de `create` (39%) continuar sem explicação pelos estágios que faltam.

Próximo passo recomendado: revisitar H5 (retenção MVCC do `snapshot_hold`) ou
H6 (dívidas de CPU do Binding, só visíveis em Release) -- ou seguir para a
Etapa 3 (varreduras de `--batch`/`durability`/`payload`), agora que o gargalo
dominante de update/create está corrigido e essas varreduras vão medir efeitos
reais, não o ruído do laço de candidatas.

### O que ficou fora, deliberadamente

- **`device_class` (nvme/ssd/hdd)** continua `null`. Determinar isso no Windows
  exige `IOCTL_STORAGE_QUERY_PROPERTY` por volume (seek penalty). Como é um fato
  da máquina, e não da corrida, o lugar natural é um campo novo em
  `loadtests/environments.json` (§4.4 do plano de carga já existe para isso) —
  não foi feito nesta rodada. `null` explícito é melhor que um palpite.
- **Núcleos físicos em Linux** ficam 0 (não coletado): exigiria deduplicar
  `(physical id, core id)` do `/proc/cpuinfo`, e não há ambiente Linux calibrado
  para validar. Repetir o valor lógico mentiria sobre SMT.
- **RSS por operação em `mixed_oltp`, `cascade_delete` e `create_hierarchy`**: o
  pico é limitado por início/fim da fase, porque não há ponto de tick por
  operação (threads concorrentes num caso, recursão atômica do motor nos
  outros). Está comentado no código em cada sítio.

### Mudança de comportamento a registrar

`series_key_version` passou de 1 para 2. Pontos antigos não são reescritos
(camada append-only), mas **não são comparáveis** com os novos: as chaves da
versão 1 foram calculadas com `page_size=0` e sem `instrumentation`. A série
histórica efetivamente reinicia aqui — o que é o preço de ter medido 30 pontos
sobre um bug, e o motivo de a Etapa 0 vir antes de qualquer perfil.

## 9. Registro de achados

Achados confirmados ou refutados entram aqui com a evidência, em ordem
cronológica. Uma hipótese refutada é resultado, não fracasso — fica
registrada para não ser reinvestigada.

| Data | Hipótese | Veredito | Evidência |
|---|---|---|---|
| 2026-07-26 | M1–M4b (defeitos de medição) | confirmados por leitura de código, corrigidos | §3 |
| 2026-07-26 | M5 (contaminação por ordem) | **confirmado** por experimento | §3.1 |
| 2026-07-26 | H1 | **confirmado, com um contra-termo novo** | §9.1 |
| — | H2 | pendente | — |
| — | H3 | reforçada indiretamente por §9.1 | — |
| — | H4 | pendente | — |
| — | H5 | pendente | — |
| — | H6 (leitura) | pendente | estágio `materialize` existe desde 2026-08-02; falta medir em Release |
| 2026-08-02 | H6 (escrita, `to_field_values`) | **refutada** | o estágio `object_bind` move 0,3 p.p. de `create` — a conversão do Binding é barata ([PLANO_PROFILER.md §9.2](PLANO_PROFILER.md)) |
| 2026-08-02 | **H7 (nova): commit reabre o WAL e faz fsync de dados 3–4× por transação** | **confirmada por instrumentação** | [PLANO_PROFILER.md §9.2](PLANO_PROFILER.md) |

### 9.1 H1 — confirmado; e a varredura revelou um segundo termo

Varredura de page size, `create_only`, um processo por caso, work dir limpo por
execução, 3 repetições, ordem alternada entre repetições:

| page size | escala | páginas | ops/s | µs/op | previsto (§4 H1) |
|---|---|---|---|---|---|
| 4 KiB | 100k | 11.179 | 15.471 | 64,88 | — (ponto de partida) |
| 8 KiB | 100k | 5.351 | 22.652 | 44,17 | **43,3** ✅ |
| 16 KiB | 100k | 2.675 | 23.368 | 42,91 | 31,8 ❌ |
| 4 KiB | 10k | 1.118 | 47.631 | 21,00 | — |
| 8 KiB | 10k | 536 | 51.717 | 19,34 | — |
| 16 KiB | 10k | 268 | 44.748 | 22,35 | — |

**H1 está confirmada.** Reduzir a contagem de páginas pela metade (4 KiB → 8 KiB
a 100k) cortou 20,7 µs/op contra 22,9 µs previstos — uma predição feita antes de
rodar, acertada em 10%. E o custo por página tem agora duas estimativas
independentes que concordam: 4,55 ns/página (delta de escala 10k→100k) e
4,47 ns/página (degrau de page size a 100k).

Ajustando `µs/op = a + b·páginas + c·page_size` aos três pontos de 100k:
`a ≈ 9,6 µs`, `b ≈ 4,47 ns/página`, `c ≈ 1,31 ns/byte de página`. Nesse ajuste,
**o termo proporcional à contagem de páginas responde por ~50 dos 65 µs/op
(≈77%) na configuração atual de 4 KiB a 100k.**

**O contra-termo (novo, não previsto).** O passo 8 KiB → 16 KiB não rendeu quase
nada (44,17 → 42,91 µs), e a 10k as páginas de 16 KiB são *mais lentas* que as de
4 KiB (22,35 vs 21,00). Existe um custo que cresce com o **tamanho** da página e
cancela o ganho. Suspeito principal: H3 — o WAL grava **imagens de página
inteiras** ([wal.cpp:324](../src/tx/wal.cpp)), então dobrar a página dobra os
bytes de WAL por página suja. Somam-se o memcpy do buffer de página por escrita e
a compactação da `SlottedPage`.

Honestidade sobre o ajuste: o modelo de 3 parâmetros calibrado nos pontos de 100k
**erra os pontos de 10k** (prevê 32,2 µs para 16 KiB/10k, mediu 22,35). Ou seja,
`c` não é um custo puro por byte de página independente da escala. Não vou
continuar ajustando curvas: separar esses termos é exatamente o trabalho da
Etapa 1 (timers por estágio), e é para lá que isto vai.

**Achado acionável imediato, sem uma linha de código:** páginas de 8 KiB dão
**+46% de vazão de ingestão a 100k** (15.471 → 22.652 ops/s) com o arquivo
ligeiramente **menor** (43,7 → 41,9 MB). 16 KiB não acrescenta nada (+3%) e piora
escalas pequenas. Isto não entra como decisão ainda: falta rodar o resto da
matriz (update/delete/read) em 8 KiB antes de mexer no default.

> **Revisão de 2026-08-02 — este +46% não existe mais.** A matriz foi rodada, e
> na mesma medição a ingestão a 100k ficou **igual** entre 4 KiB e 8 KiB (55.724
> vs 55.011 ops/s, dentro do CV). A ação A2 absorveu o ganho: as duas mudanças
> atacavam o mesmo termo proporcional à contagem de páginas, e A2 o eliminou na
> fonte (o próprio 4 KiB subiu de 15.471 para 55.724 ops/s, 3,6×). O padrão foi
> mesmo trocado para 8 KiB, mas por outro motivo: `update_shrink` 1,94× e
> `delete` 1,26×, com todas as outras fases planas. Detalhe e consequências de
> formato em [PLANO_PROFILER.md §9.2.2](PLANO_PROFILER.md).
>
> Um achado de perfil vale contra o código em que foi medido. Este ficou 5 dias
> obsoleto.

### 9.2 Nota de procedência da série histórica

As execuções de diagnóstico desta sessão foram indexadas por engano na série
permanente — `--no-index` existe ([modb_load.cpp:183](../loadtests/modb_load.cpp))
e não foi usado. Um ponto ficou medido sob contaminação conhecida de M5 e é
indistinguível de um ponto limpo:

- `run-20260726T211057.125Z-03b9fe9b`, caso
  `load.create_delete_forward.embedded.10k`: 24.262 ops/s, medido de propósito
  logo depois de um caso de 100k. O valor limpo do mesmo caso é ~49.998 ops/s.

Mantido no arquivo, não removido: a camada histórica é append-only por decisão de
projeto (§13.2 do plano de carga), e apagar a linha destruiria também o registro
de que ela existiu. Quem analisar a série deve excluir esse `run_id`. Corretivo
para o futuro: `--no-index` em toda execução exploratória.
