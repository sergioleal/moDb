# Plano de tarefas — análise e otimização de desempenho

- Data de abertura: 2026-09-26
- Estado do código na abertura: `8260f59` (`feat: make H5 measurable, and refute it`)
- Consolida: [PLANO_PROFILING.md](PLANO_PROFILING.md),
  [PLANO_PROFILER.md](PLANO_PROFILER.md),
  [RESULTADOS_PROFILING.md](RESULTADOS_PROFILING.md),
  [OTIMIZACOES_10C.md](OTIMIZACOES_10C.md)
- Uso: marque `[x]` ao concluir; anote o commit e o documento com a evidência
  ao lado do item. Uma hipótese refutada também é conclusão.

Regras que valem para todas as tarefas (herdadas dos planos acima):

1. Nenhuma otimização entra só com perfil — precisa de antes/depois registrado
   ([PLANO_BENCHMARKS.md §12](../docs/PLANO_BENCHMARKS.md)).
2. Medição comparável: RelWithDebInfo, **um processo por caso**, **work dir
   limpo por repetição**, ≥3 repetições (5 quando o CV passar de 5%), ordem A/B
   alternada.
3. `--no-index` em toda execução exploratória.
4. Predição escrita **antes** de rodar, com limite de refutação.
5. Um achado de perfil vale contra o código em que foi medido: remedir antes de
   agir sobre números anteriores a uma otimização.
6. 138/138 testes em `relwithdebinfo`, `stage-profile` e `sanitizers` antes de
   fechar qualquer mudança de motor.

---

## Parte 1 — O que já foi feito

### 1.1 Medição confiável (Etapa 0)

- [x] M1 — preset `relwithdebinfo` (+ `gprof`, `profile-4k/8k/16k`); scripts
  preferem binário otimizado
- [x] M2 — RSS por fase (`RssTracker`)
- [x] M3 — `page_size` numérico, `series_key_version` = 2
- [x] M4/M4b — CPU, cores, RAM, OS, fs, instrumentação coletados; `os_version` correto
- [x] M5 — contaminação por ordem **identificada** (efeito 2×); mitigada com um
  processo por caso — **causa ainda não isolada** (ver T11)
- [x] Baseline RelWithDebInfo, 30 pontos, CV 0,3–6%
- [x] Defeito do harness: work dir não limpo entre repetições (erro de até 2×) — corrigido na metodologia

### 1.2 Profiler in-process (Etapa 1 / PLANO_PROFILER passos 1–9)

- [x] `stage_profile` no JSONL: 18 estágios (13 folhas + 5 envelopes), leitura e escrita
- [x] Envelopes fora de `attributed_ns`; teste falha se fração > 1,0
- [x] `max_ns` por estágio
- [x] Dimensão `reads_per_write` ponta a ponta; mix alcançado; balde por classe de operação
- [x] `workload_version` por workload (`mixed_oltp` → 2)
- [x] Overhead do harness como número (`harness_overhead_*`, `engine_ops_per_second`)
- [x] Fase `hold` do `snapshot_hold` instrumentada; ops `noop` separadas

### 1.3 Hipóteses resolvidas

- [x] H1 — varredura de candidatas em `TableHeap::insert`: **confirmada**
- [x] H2 — `persist_root` por registro: **confirmada, pequena** (0,5–1,8%)
- [x] H5 — retenção MVCC no `snapshot_hold`: **refutada** (fase é 98% durabilidade de commit)
- [x] H6 (escrita) — `to_field_values`/Binding: **refutada** (0,3 p.p. de `create`)
- [x] H7 (nova) — WAL reaberto por commit + fsync de dados 3–4×: **confirmada**
- [x] Pred. 1 (mix dominado pelo commit): **confirmada a 10k** (2,26× vs ~2,1× previsto)
- [x] Pred. 2 (sem commit, mix quase não importa): **confirmada a 10k** (−2,2% por escrita)

### 1.4 Otimizações aplicadas e medidas

| Ação | Commit | Ganho medido |
|---|---|---|
| A2 — índice de candidatas por capacidade | `c1afe5b` | `crud_full.100k` 105,6 s → ~30 s (3,5×); `delete` −22% (aceito) |
| WAL mantido aberto entre commits | `9157e81` | `mixed_oltp.1k` 2,01× |
| Page size padrão 8 KiB (A5) | `e1e0847` | `update_shrink` 1,94×, `delete` 1,26× |
| Um fsync de dados a menos por commit | `c7fe4d1` | `mixed_oltp.10k` 1,19× |
| Uma leitura por leitura (`peeked_`) | `9133234` | fase `read` 1,14× (motor ~2×) |
| Correção da vazão do load test (overhead do harness) | `2130a20` | — (medição) |

---

## Parte 2 — O que falta

A numeração **é a ordem de execução**: primeiro por prioridade, depois da mais
fácil para a mais difícil dentro de cada prioridade.

- **P0** — está errado hoje
- **P1** — maior teto de ganho conhecido
- **P2** — hipótese aberta com evidência
- **P3** — higiene, ou opcional

Esforço: **máquina** = só tempo de execução, sem código · **pequeno** < 2 h ·
**médio** ~meio dia a um dia · **grande** exige ADR ou mexe em formato/recuperação.

### P0

#### T1 — Atualizar a documentação obsoleta *(pequeno, só texto)* ✅

Os três documentos de origem contradizem o código atual.

- [x] 1.1 `RESULTADOS_PROFILING.md`
  - [x] 1.1.1 §1/§6: "Nenhuma foi executada" é falso — marcar A5 feita (por
    outro motivo), A4 parcial, e registrar as ações de H7 e da leitura dupla
  - [x] 1.1.2 §7: H5 refutada; H6 (escrita) refutada
  - [x] 1.1.3 §4.1: avisar que os números de `heap_candidate_scan` são de antes da A2
- [x] 1.2 `PLANO_PROFILING.md`
  - [x] 1.2.1 §8 Andamento: atualizar H2 (confirmada, pequena), H3 e H4
  - [x] 1.2.2 Trocar o "próximo passo recomendado" (H5, já feito) por um link para este plano
- [x] 1.3 `PLANO_PROFILER.md`
  - [x] 1.3.1 §9 passo 10: medido a 10k (§4.7), 100k pendente (T4.1)
  - [x] 1.3.2 §9.3.1 "Não corrigido aqui": a leitura dupla foi corrigida em §9.2.4
  - [x] 1.3.3 §8 "não toca em H5": H5 foi refutada (PLANO_PROFILING §9.3)
- [x] 1.4 Linkar este plano a partir dos três documentos

#### T2 — Site navegável com toda a documentação em HTML *(médio)* ✅

Hoje são 148 arquivos `.md` versionados (raiz 5, `docs/` 17, `docs/decisions/`
22, `docs/reference/` 9, `docs/training/` 44, `docs-process/` 17,
`docs-process/training/` 16, `examples/` 17, `tests/fixtures/` 1), ligados por
links relativos entre si e para o código-fonte. O site gera HTML estático a
partir deles — os `.md` continuam sendo a fonte; nada é editado à mão no HTML.

- [x] 2.1 Escolher o gerador
  - [x] 2.1.1 Critérios: roda no Windows sem rede depois de instalado; resolve
    links `.md` → `.html`; tabelas, blocos de código com destaque, âncoras de
    seção; busca local; saída abrível direto do disco (`file://`)
  - [x] 2.1.2 Comparar: MkDocs + Material (Python já instalado), pandoc +
    template, ou script próprio em Python
  - [x] 2.1.3 Registrar a escolha e o porquê no próprio script ou no DEVELOPER_GUIDE
- [x] 2.2 Estrutura de navegação
  - [x] 2.2.1 Página inicial com as seções: visão geral (raiz), referência,
    decisões (ADRs), operação, treinamento, processo (planos e resultados),
    exemplos
  - [x] 2.2.2 Menu lateral por seção; ADRs em ordem numérica; lições de
    treinamento em ordem de capítulo
  - [x] 2.2.3 Trilha de navegação (breadcrumb) e "anterior/próximo" dentro do treinamento
  - [x] 2.2.4 Decidir o que fica de fora (ex.: `tests/fixtures/`)
- [x] 2.3 Links
  - [x] 2.3.1 Links entre documentos (`../docs/X.md#secao`) viram links entre páginas
  - [x] 2.3.2 Links para código (`../src/...:353`, `../loadtests/...`): decidir
    o destino — arquivo local, ou texto sem link com o caminho visível
  - [x] 2.3.3 Verificador de links: o build falha com link interno quebrado e
    lista os quebrados
  - [x] 2.3.4 Corrigir os links quebrados que o verificador achar nos `.md`
- [x] 2.4 Script de build
  - [x] 2.4.1 `scripts/build-docs.ps1` (convenção dos scripts do repo), saída
    em diretório próprio (ex.: `build/docs-site/`)
  - [x] 2.4.2 Saída fora do git (`.gitignore`), a menos que se decida publicar
  - [x] 2.4.3 Build incremental ou rápido o bastante para rodar a cada edição
- [x] 2.5 Validação
  - [x] 2.5.1 Abrir no navegador e navegar pelas seções; conferir tabelas
    largas, blocos de código, acentos (UTF-8) e tema claro/escuro
  - [x] 2.5.2 Busca encontra termos de documentos de seções diferentes
- [x] 2.6 Documentar como gerar e abrir (README e DEVELOPER_GUIDE)
- [ ] 2.7 *(opcional)* Publicar: artefato de CI ou página hospedada

### P1

#### T3 — Rodada única de remedição com `stage-profile` *(máquina)*

Uma mesma rodada (`mixed_oltp.10k`, `snapshot_hold.10k`, `crud_full.100k`,
5 repetições, preset `stage-profile`) responde quatro perguntas. As subtarefas
3.2–3.4 pertencem a tarefas P2 (T14, T15, T16), mas saem de graça aqui e
decidem se essas tarefas valem.

- [ ] 3.1 Atribuição atual de `tx_commit` (depois das três ações de commit):
  `page_file_sync`, `wal_sync`, `wal_append`, `buffer_pool_writeback`
- [ ] 3.2 `heap_candidate_scan` em `update_shrink` com páginas de 8 KiB
  (era 75,7% e 6.723 iterações/op, medido a 4 KiB) → decide T14
- [ ] 3.3 Peso de `materialize` e `object_decode` na fase `read` → decide T15
- [ ] 3.4 Bytes de WAL por operação em RelWithDebInfo (o "3 KB/op" é de Debug) → decide T16

#### T4 — Varreduras sem código *(máquina)*

- [ ] 4.1 Pred. 1 e Pred. 2 a **100k** (`mixed_oltp`, `--reads-per-write 4,10`)
  — o passo 10 do PLANO_PROFILER pedia 100k; só 10k foi medido
- [ ] 4.2 Page size 16 KiB depois da A2 — o contra-termo continua ausente?
- [ ] 4.3 Cache `warm` vs `oversubscribed` (buffer pool)
- [ ] 4.4 Payload slim/normal/fat → custo por byte vs por operação
- [ ] 4.5 Escala 1k/10k/25k/50k/100k/250k em `create_only` → expoente de
  crescimento depois da A2 (a mais lenta: o 250k pode levar dezenas de minutos)

#### T5 — Varreduras de `durability` e `--batch` *(pequeno + máquina)*

Não aparecem no `--help` do `modb_load`; existem na matriz, mas talvez sem flag.

- [ ] 5.1 Confirmar como selecionar `durability` e `batch`; expor na CLI se preciso
- [ ] 5.2 `durability` `sync_real` vs `disabled_diagnostic` → teto puro do
  `fsync` (predição escrita antes de rodar) → decide T9
- [ ] 5.3 `--batch` 1/100/1k/10k em `create_only` e `mixed_oltp` → custo de commit amortizado

#### T6 — Pred. 3: a leitura fecha em `materialize`/`object_decode` *(pequeno)*

- [ ] 6.1 Cobertura de estágios medida contra `operation_ns_total` (tempo de
  motor), não contra `duration_ns` (que inclui o harness)
- [ ] 6.2 Registrar o veredito: confirmada, ou refutada se `buffer_pool_miss` dominar

#### T7 — Investigar `wal_append` *(médio)*

~124 µs/op na fase `hold`.

- [ ] 7.1 Localizar o custo: cópia da imagem de página, alocação, codificação?
- [ ] 7.2 Se houver correção barata: predição e medição antes/depois

#### T8 — Desenho de group commit *(médio, só documento)*

- [ ] 8.1 Desenho: vários commits concorrentes → um `fsync` de WAL
- [ ] 8.2 Estimar o teto em `mixed_oltp` com sessões concorrentes
- [ ] 8.3 Registrar a decisão: fazer ou não

#### T9 — Checkpoint preguiçoso: 1 `page_file_sync` por commit *(grande)*

Só entra se T5.2 mostrar teto grande.

- [ ] 9.1 ADR: avançar o checkpoint a cada N commits; impacto na recuperação
  (replay mais longo) e na invariante "páginas duráveis antes do checkpoint"
- [ ] 9.2 Teste de crash/recovery com checkpoint defasado
- [ ] 9.3 Implementação
- [ ] 9.4 Antes/depois em `mixed_oltp`, `snapshot_hold` e `restart_recovery`

#### T10 — Relatório e gates do ciclo (Etapa 4) *(médio)*

Fecha o ciclo P1; repetir ao fim de cada ciclo seguinte.

- [ ] 10.1 `docs-process/PROFILING_<tag>.md`: por gargalo, hipótese,
  evidência, teto, custo e decisão
- [ ] 10.2 Baselines RelWithDebInfo pós-otimizações para os casos principais
- [ ] 10.3 Casos de `modb_load gate` (vazão de motor por fase, `tx_commit` ns/op)
  — `desktop-windows` é ruidoso: gate só em ambiente calibrado
- [ ] 10.4 Tag de versão com o ciclo fechado

### P2

#### T11 — Isolar a causa de M5 *(pequeno + máquina)*

- [ ] 11.1 Rodar o mesmo caso N vezes num único processo e plotar a série
- [ ] 11.2 Variar: caso de 100k antes ou não; work dir limpo ou acumulado;
  mesmo processo ou processo novo
- [ ] 11.3 Classificar: estado de processo (heap/allocator), cache do SO ou metadados NTFS
- [ ] 11.4 Se for estado de processo: abrir hipótese de produto (servidor de
  vida longa) e testar um allocator alternativo

#### T12 — Perfilar os workloads ainda não medidos depois das otimizações *(máquina, muitos casos)*

Para cada um: cobertura de estágios, top-3 estágios, hipótese se houver anomalia.

- [ ] 12.1 `range_scan_sweep` (último número é de Debug)
- [ ] 12.2 `cascade_delete`
- [ ] 12.3 `blob_lifecycle`
- [ ] 12.4 `oversubscribed_churn`
- [ ] 12.5 `restart_recovery` (replay do WAL; afetado por T9)
- [ ] 12.6 `loopback` / `remote_colocated` (caminho de rede)

#### T13 — Decidir o que o load test mede *(pequeno a médio)*

De 37% a 74% de cada número de fase é o medidor.

- [ ] 13.1 Decidir: motor, ponta a ponta, ou os dois explicitamente
  (registrar em PLANO_TESTES_DE_CARGA)
- [ ] 13.2 Se "motor": tirar validação e formatação do laço cronometrado
- [ ] 13.3 Dashboard mostra `engine_ops_per_second` ao lado de `ops_per_second`
- [ ] 13.4 Aviso na série histórica: comparar fases entre si nunca foi válido

#### T14 — Resto do caminho de update (H4) *(médio; depende de T3.2)*

- [ ] 14.1 Se `heap_candidate_scan` ainda dominar `update_shrink`: explicar por
  que `lower_bound` + "para na primeira" ainda itera milhares de vezes
- [ ] 14.2 Bytes movidos e escritas de página por update vs tamanho do registro
- [ ] 14.3 Cobertura de `update_inplace`/`update_grow` (66–67%): o que falta atribuir

#### T15 — Dívidas de CPU no caminho de leitura (H6 / Fase 10C) *(médio; depende de T3.3)*

Medir com `modb_bench object_store.read_hotpath` e o estágio `materialize`.
Cada item: predição, antes/depois, e decisão mesmo que seja "não vale".

- [ ] 15.1 `std::function` por campo no Binding → NTTP / ponteiro cru
- [ ] 15.2 Zero-copy em `to_field_values` / strings (na leitura; a escrita foi refutada)
- [ ] 15.3 `Handle::get<Member>()` materializa o objeto inteiro → projeção de um campo
- [ ] 15.4 Append incremental de coleções (hoje O(n))

#### T16 — WAL grava imagens de página inteiras (A4 / H3) *(grande; depende de T3.4 e T5)*

- [ ] 16.1 Separar custo por byte (append + sync) do custo por commit, com T5.2/T5.3
- [ ] 16.2 Se o teto justificar: ADR de registro lógico/delta (muda o formato
  do WAL, a recuperação e a réplica — ADR-016/020)
- [ ] 16.3 Registrar a decisão (fazer / não fazer) com o teto estimado

### P3

#### T17 — Limpar `load-results/` *(pequeno)*

- [ ] 17.1 Listar o que será apagado (7,6 GB de `.modb` descartáveis, §13.2 do
  plano de carga) e confirmar antes de apagar

#### T18 — Excluir o ponto contaminado das análises *(pequeno)*

- [ ] 18.1 Lista de exclusão para `run-20260726T211057.125Z-03b9fe9b` nas análises da série

#### T19 — `device_class` em `loadtests/environments.json` *(pequeno)*

- [ ] 19.1 Campo por máquina (nvme/ssd/hdd), não por corrida

#### T20 — Regressão de `delete` do índice de capacidade *(médio)*

- [ ] 20.1 Remedir o custo residual com 8 KiB (8 KiB já devolveu 1,26×)
- [ ] 20.2 Se ainda relevante: índice intrusivo ou por bucket no lugar de `std::set<pair>`
- [ ] 20.3 Antes/depois em `delete` e `update_shrink`

#### T21 — Custo real da retenção MVCC *(médio)*

H5 foi refutada para `snapshot_hold`, mas a retenção nunca foi medida.

- [ ] 21.1 Desenhar workload de leitura pesada sob snapshot aberta
- [ ] 21.2 Predição pré-registrada; medir 10k e 100k
- [ ] 21.3 Registrar o veredito

#### T22 — Calibração por classe de build *(pequeno em código, horas de máquina)*

Só afeta execuções com `--max-duration`/`--max-disk-gb`; as medições deste
plano não usam esses limites.

- [ ] 22.1 `estimate_case` escolhe `windows-x86_64-<build>.json`
- [ ] 22.2 Recalibrar em RelWithDebInfo (10k/100k; 250k se o tempo couber)

#### T23 — Atribuição por função (Etapa 2) *(opcional)*

Só se, depois de T6 e T13, sobrar resíduo de motor sem explicação.

- [ ] 23.1 `gprof` (preset `gprof`) sobre `create_only.100k`
- [ ] 23.2 `perf record -g` + flamegraph no WSL2 (CPU transfere; I/O não)

#### T24 — Histograma log₂ por estágio *(opcional)*

- [ ] 24.1 Só se caudas (p99/p999) virarem a pergunta; `max_ns` já aponta o estágio

#### T25 — Núcleos físicos em Linux *(opcional)*

- [ ] 25.1 Deduplicar `(physical id, core id)` do `/proc/cpuinfo` quando houver
  ambiente Linux calibrado

---

## Parte 3 — Registro de execução

| Data | Tarefa | Commit | Resultado / evidência |
|---|---|---|---|
| 2026-09-26 | T1 | — (não commitado) | RESULTADOS_PROFILING, PLANO_PROFILING e PLANO_PROFILER atualizados e linkados a este plano |
| 2026-09-26 | T2 | — (não commitado) | `scripts/build_docs_site.py` (+ `build-docs.ps1`/`.sh`, `docs_site_assets/`): 148 documentos, 8 seções, 115 páginas de código com âncora de linha, busca local; build em ~3 s; 91 links quebrados corrigidos nos `.md` (90 apontavam para arquivos movidos para `docs/`, 1 para arquivo removido). 2.7 (publicar) não feito — opcional |
