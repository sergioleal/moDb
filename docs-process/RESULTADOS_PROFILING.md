# Resultados de profiling e ações sugeridas

- Data: 2026-07-26
- Branch: `subphase/H-calibration-250k`
- Commit base: `060b517`. As medições desta rodada foram colhidas sobre `4419afd`;
  `060b517` chegou depois, em paralelo, e só alterou a tabela de calibração
  (nenhum caminho de código medido aqui) — ver o conflito registrado em A7.
- Plano e andamento: [PLANO_PROFILING.md](PLANO_PROFILING.md)
- Escopo desta rodada: Etapa 0 (medição confiável), varredura de page size da
  Etapa 3 e Etapa 1 (timers por estágio, parcial). Etapas 2 e 4 não começaram.

Este documento registra **o que foi medido** e **o que sugerimos fazer**. Nada
aqui é uma otimização aplicada: a regra do repositório é que otimização entra
com comparação antes/depois registrada, e nenhuma mudança de caminho quente foi
feita nesta rodada.

## 1. Resumo em cinco linhas

1. **Corrigido**: o laço de candidatas de
   [`TableHeap::insert`](../src/storage/table_heap.cpp) respondia por 80–88%
   das fases de update e ~50% da ingestão, varrendo de 5.000 a 37.622 entradas
   por operação e devolvendo **zero candidatas** na maioria das vezes. Trocado
   por um índice ordenado por capacidade (`lower_bound` em O(log n)) — dois
   falsos começos medidos e descartados antes da versão final (§4.2).
2. Resultado medido, isolado, antes/depois: `crud_full.embedded.100k` de
   **105,6 s para ~30 s (3,5×)**, com `create` 3,2×, `update_inplace` 6,7×,
   `update_grow` 11,1× e `update_shrink` 7,2× mais rápidos. `hash_match=True`
   em todas as verificações; 138/138 testes em três presets.
3. Contrapartida real e aceita: `delete` ficou ~22% mais lento (custo de manter
   o índice ordenado). `read` não regrediu — a comparação inicial vinha de uma
   campanha contaminada por M5, não de uma medição isolada limpa (§4.2).
4. O gargalo era de **escrita e mutação**, não de leitura: leitura e varredura
   nunca degradaram com o volume.
5. Cinco defeitos de medição foram encontrados e corrigidos antes de qualquer
   conclusão. Antes deles, nenhum número da série histórica descrevia o
   produto.

## 2. Confiabilidade da medição (o que mudou)

Toda a série histórica anterior (30 pontos) foi coletada em build **Debug/`-O0`**,
com `page_size=0`, `os`/`cpu`/`cores`/`ram`/`fs` nulos e RSS por fase inutilizável.
Os defeitos e suas correções estão em [PLANO_PROFILING.md §3](PLANO_PROFILING.md).

Ambiente atual, agora registrado de verdade em cada ponto:

```
Windows 10.0.26200.8737 (25H2) · AMD Ryzen 7 PRO 8840HS · 8 físicos / 16 lógicos
28 GB RAM · NTFS · GNU 15.2.0 · RelWithDebInfo · instrumentation=none · page 4096
```

Escala do erro que o build errado introduzia: `crud_full` a 100k caiu de **264 s
(Debug)** para **105,6 s (RelWithDebInfo)**. Rankings colhidos em `-O0` não são os
rankings do produto.

Qualidade do baseline: 30 pontos, 3 repetições, **CV de 0,3% a 6%**.

Ressalva de comparabilidade (defeito M5, §5): o baseline é válido para comparar
**repetições do mesmo caso**, não para comparar **casos entre si**.

## 3. Onde o tempo está

### 3.1 Degradação por escala (baseline, 3 repetições)

| Fase | 10k | 100k | queda |
|---|---|---|---|
| `read` | 55.933 ops/s | 53.070 ops/s | 1,05× — plano |
| `delete` | 86–102k ops/s | 80–91k ops/s | ~1,1× — plano |
| `range_scan` (100%) | 45.176 linhas/s | 43.848 linhas/s | plano *(Debug)* |
| `create` | 49.341 ops/s | 15.103 ops/s | **3,27×** |
| `update_inplace` | 26.470 ops/s | 5.885 ops/s | **4,50×** |
| `update_grow` | 18.883 ops/s | 3.092 ops/s | **6,11×** |
| `update_shrink` | 14.326 ops/s | 2.206 ops/s | **6,49×** |

Os valores de `create` vêm de execuções isoladas (um processo por caso); os
demais, do baseline. Bytes por objeto ficam constantes (458) em todas as escalas:
**não é amplificação de espaço, é custo por operação crescendo com o volume.**

### 3.2 Onde está o tempo absoluto: `crud_full` a 100k

| Fase | segundos | % do caso |
|---|---|---|
| `update_shrink` | 45,51 | **43,1%** |
| `update_grow` | 32,44 | **30,7%** |
| `update_inplace` | 17,03 | **16,1%** |
| `create` | 7,50 | 7,1% |
| `read` | 1,89 | 1,8% |
| `delete` | 1,24 | 1,2% |
| **total** | **105,61** | |

**As três fases de update são 90% do caso.** É o maior bloco de tempo do
conjunto inteiro e ainda não recebeu nenhuma investigação dirigida.

## 4. H1 — confirmada, e um termo novo apareceu

**Hipótese:** [`TableHeap::insert`](../src/storage/table_heap.cpp) itera o
`insertion_capacity_by_page_` inteiro (um `std::map`) a cada inserção, montando
candidatas e filtrando dentro do laço — custo O(nº de páginas) por insert.

**Predição registrada antes de medir:** se o custo é proporcional à contagem de
páginas, dobrar o page size corta o termo pela metade.

| page size | escala | páginas | ops/s | µs/op | previsto |
|---|---|---|---|---|---|
| 4 KiB | 100k | 11.179 | 15.471 | 64,88 | — (partida) |
| 8 KiB | 100k | 5.351 | 22.652 | 44,17 | **43,3** ✅ |
| 16 KiB | 100k | 2.675 | 23.368 | 42,91 | 31,8 ❌ |
| 4 KiB | 10k | 1.118 | 47.631 | 21,00 | — |
| 8 KiB | 10k | 536 | 51.717 | 19,34 | — |
| 16 KiB | 10k | 268 | 44.748 | 22,35 | — |

Metodologia: um processo por caso, work dir limpo por execução, 3 repetições,
ordem alternada entre repetições.

**Confirmada.** Metade das páginas cortou 20,7 µs/op contra 22,9 previstos. Duas
estimativas independentes do custo por página concordam: **4,55 ns/página**
(delta de escala 10k→100k) e **4,47 ns/página** (degrau de page size a 100k).

Ajuste `µs/op = a + b·páginas + c·page_size` aos três pontos de 100k:

| termo | valor | a 100k / 4 KiB |
|---|---|---|
| `a` custo fixo | 9,6 µs | 15% |
| `b` por página | 4,47 ns | **50,0 µs — 77%** |
| `c` por byte de página | 1,31 ns | 5,4 µs — 8% |

**O termo novo.** O passo 8 → 16 KiB não rendeu quase nada, e a 10k as páginas de
16 KiB são *mais lentas* que as de 4 KiB. Há um custo que cresce com o **tamanho**
da página. Suspeito principal: [`wal.cpp:324`](../src/tx/wal.cpp) grava **imagens
de página inteiras**, então dobrar a página dobra os bytes de WAL por página suja.

**Limite do que este ajuste sustenta:** o modelo calibrado nos pontos de 100k
erra os de 10k (prevê 32,2 µs para 16 KiB/10k, mediu 22,35). Os termos não são
separáveis por varredura — é para isso que existe a Etapa 1.

## 4.1 Etapa 1: a varredura de candidatas domina TUDO, não só a ingestão

Timers por estágio (preset `stage-profile`), `create_only` e `crud_full` a 100k,
um processo por caso. Sobrecarga da instrumentação: dentro do ruído entre
execuções (6,9 s e 5,95 s instrumentado contra 6,62 s limpo na mesma fase).

| fase | duração | `heap_candidate_scan` | ns/op | iterações/op | candidatas produzidas |
|---|---|---|---|---|---|
| `create` | 6,0–6,9 s | **48,9–51,2%** | 30–34 µs | 5.000 | **zero** |
| `update_inplace` | 14,9 s | **80,2%** | 119,5 µs | 15.000 | **zero** |
| `update_grow` | 33,4 s | **85,2%** | 284,4 µs | 28.334 | **zero** |
| `update_shrink` | 45,2 s | **87,7%** | 396,3 µs | 37.622 | 0,367/op |
| `delete` | 1,2 s | — (não insere) | — | — | — |

**O laço de [table_heap.cpp:353](../src/storage/table_heap.cpp) é 80–88% das
fases de update**, que por sua vez são 90% do caso. H1 mirava a ingestão; o dano
real está no update. Um update que não cabe no lugar precisa inserir a nova
versão em outro ponto (o MVCC retém a `previous`), então **todo update chama
`insert` e paga a varredura completa** — e o número de iterações cresce ao longo
das fases (15.000 → 28.334 → 37.622) porque o heap cresce com as versões retidas.

**A varredura não produz nada.** O estágio `heap_candidate_try` conta as
candidatas que o laço ofereceu: **zero** em `create`, `update_inplace` e
`update_grow`. Ou seja, em três das quatro fases o laço percorre de 5.000 a
28.000 entradas de um `std::map` por operação e devolve lista vazia — em ingestão
sequencial todas as páginas menos a última estão cheias, e o filtro por
capacidade descarta todas, uma a uma. Só `update_shrink` produz candidatas
(0,367/op), porque encolher registros libera espaço.

**A escrita de página não é o gargalo:** `heap_page_write` fica em 0,2–0,5% em
todas as fases. `persist_root` (H2) é real — 1 chamada por operação, confirmada —
mas custa 0,5–1,8%. `wal_sync` fica em 0,8–2,5% com `batch=1000`.

Teto estimado se a varredura virasse O(1)/O(log n): `crud_full` a 100k cairia de
**105,6 s para ~22 s**, cerca de **4,8×**. É extrapolação a partir da fração
medida por fase, não uma medição — vale como ordem de grandeza para priorizar,
não como promessa.

**Cobertura da atribuição** (critério de aceite da Etapa 1 é ≥ 90%):

| fase | atribuído |
|---|---|
| `update_shrink` | 93,3% ✅ |
| `update_grow` | 89,7% |
| `update_inplace` | 84,8% |
| `create` | 58,6–60,6% ❌ |
| `delete` | 15,7% ❌ |

`create` e `delete` ainda não fecham. O resíduo **não** é I/O (a escrita de
página está medida e é desprezível): sobra o codec do objeto, o Binding, o
`IdentityMap`, o B-tree e o caminho de `erase`. Instrumentar esses é o que falta
da Etapa 1.

## 4.2 A2 — a varredura corrigida, com dois falsos começos honestos

Ação A2 (corrigir o laço de candidatas de `TableHeap::insert`) foi implementada.
O caminho até a versão final teve dois erros medidos e corrigidos, registrados
aqui porque descrevem por que a solução final é do jeito que é.

**Tentativa 1 — "capacidade > 0" como poda.** Manter um subconjunto de páginas
"reutilizáveis" (capacidade > 0) parecia bastar. Não bastou: páginas reais quase
sempre sobram alguns bytes de folga mesmo efetivamente cheias para o tamanho de
registro em uso, então quase TODAS as páginas continuavam qualificando — o
filtro não podava quase nada, e o custo extra de manter um índice paralelo (sem
reduzir o conjunto varrido) somou um `.find()` a mais por página ao trabalho que
já existia. Medido, não hipotético: `update_inplace` a 100k caiu de 5.885 para
**528 ops/s** — **11× mais lento**, com degradação progressiva dentro da própria
fase (a matemática bate: log₂(15.000) ≈ 13,9, muito próximo do fator observado).
`crud_full.embedded.100k` não terminava em 3 minutos (antes levava 105,6 s).

**Tentativa 2 — índice ordenado por capacidade, coletando todas as candidatas.**
Corrigido para um `std::set<std::pair<capacidade, PageId>>` de verdade, com
`lower_bound` pulando direto para a primeira página suficiente em O(log n). As
três fases-alvo melhoraram (create 2,4×, update_inplace 4,6×, update_grow 7,3×),
mas `update_shrink` ficou **pior** que o baseline (1.913 vs 2.206 ops/s) com um
padrão de ACELERAÇÃO dentro da fase (334 → 26.685 ops/s). Causa: o laço
continuava depois do `lower_bound` até `capacity_index_.end()`, coletando TODAS
as páginas com capacidade suficiente antes de tentar qualquer uma — inofensivo
quando poucas qualificam, mas em `update_shrink` (que libera espaço em muitas
páginas ao longo da fase) isso chegou a coletar milhares de candidatas genuínas
por operação quando a primeira já bastava.

**Versão final — para na primeira candidata.** `heap_candidate_scan` só coleta
a primeira página não-`last_` encontrada pelo `lower_bound`; se ela falhar
(cache desatualizado — não deveria acontecer em uso single-thread, já que o
cache é corrigido a cada escrita), cai no fallback de `last_`/página nova em vez
de tentar mais candidatas. Confirmado pelos timers: `heap_candidate_scan` caiu
de 48,9–87,7% para **0,4–0,5%** em create/update_inplace/update_grow, e de
87,7% para 75,7% em update_shrink (ainda o estágio dominante ali, mas com 6.723
iterações/op contra 37.622 antes — 5,6× menos).

### Resultado final, medido isolado (mesma metodologia dos dois lados)

`crud_full.embedded.100k`, um processo por medição, código antes/depois do
commit desta ação, 3 repetições onde o tempo permitiu:

| fase | antes (isolado) | depois (3 rep, CV) | fator |
|---|---|---|---|
| `create` | 10.438 | 33.233 (4,2%) | **3,18×** |
| `read` | 37.206 | 37.332 (2,6%) | 1,00× |
| `update_inplace` | 4.023 | 26.966 (1,5%) | **6,70×** |
| `update_grow` | 2.046 | 22.691 (2,2%) | **11,09×** |
| `update_shrink` | 1.484 | 10.742 (1,0%) | **7,24×** |
| `delete` | 59.022 | 45.924 (1,3%) | 0,78× |

`hash_match=True` em todas as execuções (create_only e crud_full, 10k e 100k, 3
repetições cada) — nenhuma regressão de corretude. Suíte completa (138 testes,
incluindo os 5 testes de invariante do `TableHeap`) passa nos presets
`relwithdebinfo`, `stage-profile` e `sanitizers` (MinGW não tem ASan/UBSan
reais; a rede de segurança é `_GLIBCXX_ASSERTIONS` + `-fstack-protector-strong`,
que também não acusou nada).

**`delete` fica ~22% mais lento — real, não ruído, e aceito.** Cada mudança de
capacidade agora paga duas operações de árvore num `std::set<pair>` (remove a
entrada antiga, insere a nova) em vez de uma única atribuição de mapa —
alocação/liberação de nó a cada toque, mais cara que sobrescrever um valor no
lugar. `delete` é a fase que mais toca capacidade (cada erase atualiza ou remove
uma entrada), então é onde esse custo aparece mais. **`read` não regrediu** —
a comparação inicial contra 53.070 ops/s vinha de uma campanha de 30 casos no
mesmo processo (sujeita a M5, §5), não de uma medição isolada; refeita com a
mesma metodologia dos dois lados, `read` fica praticamente idêntico (37.206 →
37.332).

**Não investigado mais a fundo, por escolha:** eliminar o custo de
alocação/liberação de nó do `std::set<pair>` (um índice intrusivo ou por bucket
seria mais amigável ao allocator) devolveria parte do `delete`, mas é engenharia
adicional para um ganho pequeno frente ao que já foi conquistado. O saldo total
do caso (create-to-delete) já é **3,5× mais rápido** (105,6 s → ~30 s), e a
perda em `delete` (≈0,5 s em 100k operações) é uma fração disso.

## 5. M5 — a vazão depende do que rodou antes

Descoberto ao perseguir uma anomalia do baseline: a mesma fase `create`, no mesmo
código, deu 52.639 ops/s num workload e ~25.000 em outro.

| Execução | `create` a 10k |
|---|---|
| isolado, primeiro caso (`create_only`) | 49.341 ops/s |
| isolado, primeiro caso (`create_delete_forward`) | 49.998 ops/s |
| 10k depois de outro 10k (controle) | 49.668 / 49.974 ops/s |
| **10k depois de um caso de 100k** | **24.262 ops/s** |

Um único caso de 100k antes corta a vazão do seguinte pela metade, e o regime
novo é **estável** (CV 0,3%), não um transiente. Causa não isolada; a suspeita
principal é estado de processo (heap/allocator), porque cache de escrita do SO
drenaria e recuperaria.

Isto tem duas leituras e **não decidimos entre elas**: defeito de metodologia do
harness, ou sinal real de degradação de processo de vida longa — que importaria
de verdade num servidor.

## 6. Ações sugeridas

Ordenadas por valor sobre custo. Nenhuma foi executada.

### A1 — Etapa 1: timers por estágio ✅ *parcialmente entregue*

Entregue: preset `stage-profile`, seis estágios instrumentados
(`heap_candidate_scan`, `heap_candidate_try`, `heap_page_write`, `persist_root`,
`wal_append`, `wal_sync`), registro `stage_profile` por fase no JSONL com
`unattributed_ns` explícito. Custo zero quando desligado; dentro do ruído quando
ligado. Resultados em §4.1.

**Falta** fechar a cobertura em `create` (58,6%) e `delete` (15,7%):
instrumentar codec do objeto, Binding, `IdentityMap`, B-tree e o caminho de
`erase`. **Esforço:** ~meio dia.

### A2 — `TableHeap::insert`: laço de candidatas ✅ *implementada e medida*

Entregue: `capacity_index_`, um `std::set<std::pair<capacidade, PageId>>`
mantido em todo ponto que escreve `insertion_capacity_by_page_`; `insert()`
usa `lower_bound(tamanho do registro)` e para na primeira candidata. Detalhes,
os dois falsos começos e a análise da contrapartida em `delete`: §4.2.

Resultado: `crud_full.embedded.100k` de 105,6 s para ~30 s (3,5×), com ganhos
de 3,2× a 11,1× nas fases de create/update. `delete` ~22% mais lento (aceito).
138/138 testes em `relwithdebinfo`, `stage-profile` e `sanitizers`.

### A3 — Caminho de update ✅ *respondido por A1*

Era o maior bloco de tempo absoluto e não tinha hipótese. Agora tem: 80–88% dele
é a varredura de candidatas de A2, porque todo update que não cabe no lugar
insere a nova versão e paga a varredura inteira. **Não precisa de investigação
própria — é a mesma correção.**

### A4 — WAL: imagens de página inteiras *(implicado por dois caminhos)*

[`wal.cpp:324`](../src/tx/wal.cpp) grava a página completa. Dois indícios
independentes: o contra-termo por tamanho de página (§4) e os ~3 KB de WAL por
operação do `mixed_oltp` com objetos de 458 B *(medido em Debug; refazer em
RelWithDebInfo)*.

- **Antes de mudar qualquer coisa:** varreduras de `--batch` (1/100/1k/10k) e
  `durability` (`sync_real` vs `disabled_diagnostic`), que já existem como flags
  e separam custo de commit do custo de `fsync`.
- **Esforço da medição:** baixo, é só tempo de máquina. **Da mudança:** alto —
  registro lógico/delta muda o formato do WAL e o recovery.

### A5 — Decidir o page size padrão *(sem uma linha de código)*

8 KiB deu **+46% de ingestão a 100k** com arquivo **menor** (43,7 → 41,9 MB).
16 KiB não acrescenta (+3%) e piora escalas pequenas.

- **Falta antes de decidir:** rodar a matriz completa (`update_*`, `delete`,
  `read`, `range_scan`) em 8 KiB. Ingestão é uma fase entre seis.
- **Esforço:** só tempo de máquina; os presets já existem.

### A6 — Resolver a ambiguidade de M5

Experimento sugerido: rodar o **mesmo** caso N vezes num único processo e observar
a série. Decaimento monotônico aponta estado de processo (heap/allocator);
patamar após o primeiro caso grande aponta cache do SO.

- **Se for estado de processo**, é um achado de produto, não de harness, e merece
  hipótese própria.
- **Enquanto não estiver resolvido:** medição comparável usa **um processo por
  caso**. Já adotado.
- **Esforço:** ~1 h.

### A7 — Higiene *(pequeno, mas cobra juros)*

- Usar `--no-index` em toda execução exploratória. Uma execução desta sessão
  depositou um ponto contaminado na série permanente
  (`run-20260726T211057.125Z-03b9fe9b`) — mantido, porque a camada é append-only,
  e documentado para ser excluído em análises.
- `device_class` segue não coletado; o lugar natural é um campo novo em
  `loadtests/environments.json`, já que é fato da máquina, não da corrida.
- `load-results/` acumulou 4,7 GB de arquivos `.modb` de execuções antigas
  (descartáveis pelo §13.2 do plano de carga).
- **Conflito a resolver entre a calibração e a descoberta de binário.** A tabela
  em [`loadtests/calibration/windows-x86_64.json`](../loadtests/calibration/windows-x86_64.json)
  é medida em **Debug** — inclusive as entradas de 250k que `060b517` acabou de
  substituir por medições reais. Esta rodada mudou os scripts para preferirem o
  binário **RelWithDebInfo** (correção de M1), então estimativa e execução passam
  a divergir por ~2,5×: `--dry-run` fica pessimista e, pior, `--max-duration` /
  `--max-disk-gb` podem emitir `skipped_budget` para casos que na verdade
  caberiam. Três saídas possíveis, nenhuma escolhida ainda:
  1. recalibrar em RelWithDebInfo (custa horas de máquina — `crud_full` a 250k
     levou 44 min só em Debug);
  2. versionar a calibração **por classe de build** (`windows-x86_64-Debug.json`,
     `windows-x86_64-RelWithDebInfo.json`), que é o que a `series_key` já faz
     para os pontos históricos;
  3. manter uma calibração só e aceitar a folga, documentando que a estimativa é
     um limite superior.

  A opção 2 é a coerente com o resto do desenho, e é barata: `estimate_case` já
  resolve o arquivo por plataforma/arquitetura, faltaria acrescentar o build type
  ao nome.

## 7. O que este documento não afirma

- Não afirma que 8 KiB é melhor: mediu ingestão, e ingestão é 7% do `crud_full`.
  Com a varredura corrigida (A2), o ganho do page size provavelmente encolhe —
  boa parte dele era só reduzir a contagem de iterações do laço.
- Não afirma que M5 é artefato de medição — só que impede comparação entre casos.
- Não explica 39% de `create` nem 84% de `delete`: a atribuição não fecha nessas
  duas fases, e o que falta está nomeado em A1, não adivinhado.
- O teto de ~4,8× para A2 é **extrapolação** da fração medida por fase, não uma
  medição de antes/depois.
- H5 (retenção MVCC) e H6 (dívidas de CPU do Binding) continuam **não testadas**.
  H2 foi confirmada mas é pequena (0,5–1,8%). H3 e H4 foram absorvidas: com
  `batch=1000` o WAL não domina, e o custo do update é a varredura de A2.
