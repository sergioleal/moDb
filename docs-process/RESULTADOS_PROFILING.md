# Resultados de profiling e ações sugeridas

- Data: 2026-07-26
- Branch: `subphase/H-calibration-250k`
- Commit base: `060b517`. As medições desta rodada foram colhidas sobre `4419afd`;
  `060b517` chegou depois, em paralelo, e só alterou a tabela de calibração
  (nenhum caminho de código medido aqui) — ver o conflito registrado em A7.
- Plano e andamento: [PLANO_PROFILING.md](PLANO_PROFILING.md)
- Escopo desta rodada: Etapa 0 (medição confiável) + varredura de page size da
  Etapa 3. Etapas 1, 2 e 4 não começaram.

Este documento registra **o que foi medido** e **o que sugerimos fazer**. Nada
aqui é uma otimização aplicada: a regra do repositório é que otimização entra
com comparação antes/depois registrada, e nenhuma mudança de caminho quente foi
feita nesta rodada.

## 1. Resumo em cinco linhas

1. O gargalo está na **escrita e na mutação**. Leitura e varredura não degradam
   com o volume; ingestão degrada 3,3× e update degrada até 6,5× de 10k para 100k.
2. **77% do custo de inserção a 100k é um termo proporcional à contagem de
   páginas do heap** — confirmado por predição registrada antes da medição e
   acertada em 10%.
3. Existe um **segundo termo, proporcional ao tamanho da página**, que cancela o
   ganho de páginas maiores. Suspeito principal: o WAL grava imagens de página
   inteiras.
4. **90% do tempo de um caso `crud_full` a 100k está nas três fases de update** —
   e o caminho de update ainda não foi investigado.
5. Cinco defeitos de medição foram encontrados e corrigidos. Antes deles, nenhum
   número da série histórica descrevia o produto.

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

### A1 — Etapa 1: timers por estágio *(habilitador; recomendado primeiro)*

Sem isto, tudo abaixo continua sendo inferência de curva. Instrumentar
`encode`, `heap_candidate_scan`, `heap_page_write`, `persist_root`, `wal_append`,
`wal_sync`, `buffer_pool_*`, `index_update`, com contadores de iterações do laço
de candidatas, escritas de página por operação, bytes de WAL por operação e
tempo de `fsync`.

- **Responde:** separa `b` (por página) de `c` (por tamanho de página); aponta a
  linha exata em vez de um termo de regressão.
- **Critério de aceite:** soma dos estágios ≥ 90% da duração da fase, resíduo
  registrado explicitamente.
- **Esforço:** ~1 dia. **Risco:** baixo (aditivo ao schema JSONL).

### A2 — `TableHeap::insert`: laço de candidatas *(maior ganho isolado)*

O laço em [table_heap.cpp:353](../src/storage/table_heap.cpp) percorre todas as
páginas e filtra dentro. Em ingestão sequencial todas as páginas menos a última
estão cheias, então ele itera ~11.200 entradas e produz **zero** candidatas —
custo puro, sem trabalho útil. *(A alegação "zero candidatas" é dedução do código,
ainda não medida: é o primeiro contador que A1 deve registrar.)*

- **Direção sugerida:** manter apenas páginas com espaço reutilizável numa
  estrutura própria indexada por capacidade, em vez de varrer o mapa inteiro.
- **Teto:** o modelo sugere até ~4× de ingestão a 100k se `b` for a zero. A
  evidência **medida** direta é mais modesta e mais confiável: **+46% ao dobrar o
  page size**, que corta o termo pela metade.
- **Pré-requisito:** A1, para confirmar que o custo está neste laço e não no
  buffer pool ou no mapa de espaço livre.
- **Esforço:** médio. **Risco:** mexe em caminho quente com invariantes de
  reuso de espaço — exige `table_heap_space_reuse` e `churn` verdes.

### A3 — Caminho de update *(maior bloco de tempo absoluto, não investigado)*

90% do `crud_full` a 100k, e degradação de 6,5× — pior que a ingestão. Nenhuma
hipótese foi testada ainda.

- **Sugestão:** incluir as fases `update_*` no escopo de A1 desde o início, e
  medir bytes movidos e escritas de página por operação.
- **Esforço:** vem quase de graça junto com A1.

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

- Não afirma **qual linha** consome o termo por página. H1 identifica o *termo*;
  atribuí-lo a `heap_candidate_scan` em vez do buffer pool ou do mapa de espaço
  livre é trabalho de A1.
- Não afirma nada sobre o caminho de update além de quanto ele custa.
- Não afirma que 8 KiB é melhor: mediu ingestão, e ingestão é 7% do `crud_full`.
- Não afirma que M5 é artefato de medição — só que impede comparação entre casos.
- H2, H4, H5 e H6 continuam **não testadas**.
