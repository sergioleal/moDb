# Plano de testes de carga do Ring0

- Estado: especificação, não implementada
- Versão: 1
- Data: 2026-07-25
- Escopo: carga de volume e duração sobre o modelo de objetos, com execução
  local (embedded e loopback) e remota, selecionável por subset, com série
  histórica versionada

## 1. Objetivo

Medir o comportamento do Ring0 quando o **volume de usuários** cresce de 10 mil
a 1 milhão de objetos, sob padrões de mutação de complexidade crescente, tanto
embedded quanto através do servidor (local e remoto).

Um teste de carga responde perguntas diferentes das do
[PLANO_BENCHMARKS.md](PLANO_BENCHMARKS.md):

| | Benchmark (Fase 10) | Teste de carga (este plano) |
|---|---|---|
| Pergunta | qual é a latência/throughput de uma operação | o sistema se mantém correto, estável e previsível em escala |
| Duração | amostras curtas repetidas (≥ 250 ms) | uma passada longa por caso (minutos a horas) |
| Unidade | operação | campanha de volume (N usuários do início ao fim) |
| Falha típica | regressão de 10% | crescimento de arquivo, fragmentação, degradação por janela, esgotamento de disco/memória, corrupção |
| Horizonte | uma comparação contra baseline | série histórica contínua, com deriva lenta detectável (§13) |

Os dois planos compartilham deliberadamente a infraestrutura: writer JSONL,
coleta de ambiente, seeds, `sha256` e o comparador. O que muda é o eixo de
variação e o critério de aceite.

## 2. Terminologia e premissa explícita

- **usuário**: um objeto persistente do tipo `User` (registro), não uma sessão
  humana. As escalas de 10k a 1M deste plano são **quantidade de objetos**.
- **sessão**: uma conexão de cliente concorrente. Concorrência existe como
  dimensão secundária (§4.5) com valores modestos, porque o motor tem **um
  único escritor de transação** (Fase 5) e o servidor assume poucas conexões
  por instância ([ADR-011](decisions/ADR-011-concorrencia-do-servidor.md)).
  Escalar sessões não escala escrita; mede contenção e fairness.

Se a intenção original for carga de *sessões simultâneas* em vez de volume de
registros, a dimensão D1 e a dimensão de concorrência trocam de papel — o resto
do plano (workloads, alvos, seleção de subset, formato de resultado) permanece
válido sem alteração.

## 3. Princípios

1. Nenhuma campanha completa a matriz cartesiana; a matriz existe para ser
   filtrada (§6).
2. Todo caso declara antecipadamente disco, tempo e memória estimados; exceder o
   orçamento produz `skipped_budget` registrado, nunca truncamento silencioso.
3. Carga não substitui correção: cada fase valida invariantes fora da região
   medida e a campanha falha se a validação falhar, independentemente da taxa.
4. Resultado por janela, não só agregado: degradação temporal só aparece em
   séries.
5. Dados determinísticos, seed registrada, dataset versionado.
6. Um caso interrompido deixa arquivo `.partial` legível e retomável.
7. Escrita e leitura, criação e remoção, medidas em fases separadas e nomeadas.
8. O mesmo binário roda embedded, loopback e remoto; o alvo é parâmetro, não
   fork de código.
9. Toda execução deposita um ponto na série histórica (§13). Uma medição que não
   pode ser comparada com a de três meses atrás foi trabalho perdido.
10. A série histórica é append-only e versionada; resultado bruto é descartável,
    ponto histórico não é.

## 4. Dimensões

### 4.1 D1 — Escala (quantidade de usuários)

| id | objetos | uso |
|---|---|---|
| `1k` | 1 000 | smoke de desenvolvimento; fora da faixa oficial de carga |
| `10k` | 10 000 | piso da faixa oficial; roda em qualquer máquina |
| `100k` | 100 000 | caso de referência para comparação histórica |
| `250k` | 250 000 | ponto intermediário para checar linearidade |
| `500k` | 500 000 | pressão de cache/buffer pool |
| `1M` | 1 000 000 | teto da faixa; exige orçamento de disco e tempo declarado |

A progressão 10k → 100k → 1M é multiplicativa de propósito: permite verificar se
tempo, bytes por objeto e páginas por objeto crescem de forma linear ou
superlinear. `250k` e `500k` existem para localizar o joelho quando 1M degrada.

### 4.2 D2 — Workload (complexidade crescente)

Escada fixa. Cada degrau contém as operações do anterior e acrescenta uma fonte
de estresse. Ids estáveis, nunca reaproveitados com semântica diferente.

| id | fases | o que estressa | invariante final |
|---|---|---|---|
| `create_only` | create | ingestão pura, crescimento de arquivo/WAL, custo de commit por lote | contagem == N, hash lógico do conjunto confere |
| `create_delete_forward` | create → delete (ordem de criação, FIFO) | remoção com localidade perfeita, devolução de espaço em ordem | contagem == 0, nenhum id resolvível |
| `create_delete_reverse` | create → delete (ordem inversa, LIFO) | compactação da última página, caminho de encolhimento | contagem == 0, tamanho final ≤ tamanho do `forward` |
| `create_delete_interleaved` | create → delete por stride/Zipf com seed | fragmentação real, free-list, reuso parcial | contagem == 0, fragmentação registrada |
| `crud_full` | create → read → update in-place → update maior → update menor → delete | ciclo completo, reescrita de registro, movimento entre páginas | valores lidos == esperados por objeto; contagem == 0 |

Extensões previstas, **fora do escopo desta versão** e habilitadas só nos perfis
pesados quando implementadas: `crud_query` (CRUD + consulta indexada e streaming
por seletividade) e `crud_relationships` (usuários com `Ref`/coleções e remoção
em cascata).

Regra de fases: cada fase é cronometrada, validada e resumida separadamente. Um
`crud_full` em 1M produz seis `phase_summary`, não um número único.

### 4.3 D3 — Alvo de execução

| id | topologia | mede | não mede |
|---|---|---|---|
| `embedded` | in-process, sem rede | motor puro: storage, WAL, modelo de objetos | protocolo, serialização de rede |
| `loopback` | servidor + cliente TCP em `127.0.0.1` | protocolo, frames, backpressure, custo de sessão | latência e banda reais |
| `remote_colocated` | binário de carga roda no host remoto, cliente e servidor no mesmo host | motor e protocolo no hardware/FS do servidor | rede WAN |
| `remote_client_local` | servidor remoto, cliente na máquina local | rede real: RTT, banda, jitter, TTFR | isolamento do motor |

`remote_colocated` é o alvo padrão para números de escala (rede não polui a
medição). `remote_client_local` é o alvo para questões de rede e é limitado às
escalas `10k`/`100k`, porque 1M objetos atravessando WAN mede o enlace, não o
banco.

### 4.4 D4 — Ambiente registrado

D3 responde "qual topologia" (o que roda onde, em relação a quem); D4 responde
"em qual máquina cadastrada" — as duas são ortogonais. Topologias locais
(`embedded`, `loopback`) rodam sobre um único ambiente registrado (o que
executa o comando); topologias remotas (`remote_colocated`,
`remote_client_local`) nomeiam papéis (cliente/servidor) que cada um resolve
para um ambiente registrado, possivelmente dois diferentes.

Sem um ambiente identificado, "máquina de bench" é uma string digitada de
memória a cada execução — exatamente o modo como o risco 9 (§17) acontece. Um
registro nomeado existe para ser escolhido de uma lista, não retranscrito.

Catálogo: `loadtests/environments.json`, versionado no Git — sem segredo, só
identidade e forma de alcançar a máquina (mesma prática já usada em
`scripts/run-remote-benchmark.ps1`, que hoje tem o IP embutido no script em vez
de cadastrado; isso sai como parte desta dimensão, não só na Subfase H).

```json
{
  "schema": "modb.loadtest.environments",
  "schema_version": 1,
  "environments": [
    {
      "id": "desktop-windows",
      "label": "Meu desktop (Windows, dev)",
      "kind": "local",
      "host_class": "dev-windows",
      "os_hint": "windows",
      "notes": "Máquina de desenvolvimento; ruidosa, não usar para gate."
    },
    {
      "id": "linux-remoto",
      "label": "Servidor Linux remoto (bench)",
      "kind": "ssh",
      "host_class": "bench-linux-01",
      "os_hint": "linux",
      "connection": {
        "host": "161.35.9.43",
        "default_user": "root",
        "remote_work_dir": "/tmp/modb_load",
        "binary_name": "modb_load"
      }
    }
  ]
}
```

Campos:

- `id`: slug estável usado por `--environment`; nunca renomeado — renomear
  significa cadastrar um novo id e marcar o antigo `"deprecated": true`, para
  não invalidar séries antigas que o referenciam;
- `kind`: `local` (processo no host onde o comando roda) ou `ssh` (host remoto
  por OpenSSH — credenciais nunca no arquivo, solicitadas interativamente,
  igual ao script atual);
- `host_class`: o rótulo de comparabilidade de §13.4, resolvido a partir do
  cadastro em vez de digitado a cada execução — fecha o risco 9;
- `connection`: só para `kind=ssh`; host, usuário padrão, diretório e nome do
  binário remoto; nenhuma senha ou token;
- `notes`: texto livre, entra em `run_note` quando relevante (ex.: "não usar
  para gate").

CLI: `--environment ID[,ID...]` seleciona onde o comando de carga executa de
fato. Implementado hoje, fora da ordem de implementação da Subfase A, em
`scripts/run-remote-benchmark.ps1 -Environment <id>`: o script resolve host,
usuário e caminho remoto pelo catálogo e recusa `kind` diferente de `ssh`. O
`modb_load` da Subfase A adota a mesma interface.

`environment` não entra no `case_id` (mantém o princípio 3 de ids estáveis),
mas é gravado em `case_start`, no rollup (`environment`, §13.3) e é filtro de
primeira classe na CLI e no dashboard (§13.11).

### 4.5 Dimensões secundárias

Fixas em um valor padrão; variadas apenas por caso dirigido a risco.

| dimensão | valores | padrão |
|---|---|---|
| payload do usuário | `slim` (~64 B), `normal` (~256 B), `fat` (~4 KiB) | `normal` |
| objetos por commit | 1, 100, 1 000, 10 000 | 1 000 |
| sessões concorrentes | 1, 4, 16 | 1 |
| leitores concorrentes durante escrita | 0, 2, 8 | 0 |
| durabilidade | `sync_real`, `disabled_diagnostic` | `sync_real` |
| cache | `warm`, `database_reopen`, `oversubscribed` | `warm` |
| `primary_storage` | `full`, `wal_only` | `full` |

`durability=disabled_diagnostic` nunca é publicado como número de carga durável;
serve para isolar CPU/codec do custo de `fsync`.

`primary_storage=wal_only`
([ADR-017](decisions/ADR-017-primary-wal-only-sem-arquivos-de-dados.md)) muda a
natureza do teste: no primary só existe WAL, então "crescimento de arquivo"
passa a ser crescimento de log e a questão relevante é retenção/checkpoint.
Casos `wal_only` só entram nos perfis pesados e sempre com o follower medido em
conjunto.

## 5. Identidade dos casos

```text
load.<workload>.<target>.<scale>[.<variante>]
```

Exemplos:

```text
load.create_only.embedded.100k
load.create_delete_reverse.loopback.1M
load.crud_full.remote_colocated.100k.c16
load.create_only.embedded.1M.payload_fat
```

O sufixo de variante aparece **somente** quando alguma dimensão secundária sai
do padrão, e usa o nome curto da dimensão (`c16`, `payload_fat`, `batch1`,
`nosync`, `reopen`, `walonly`). Um caso com todos os padrões nunca carrega
sufixo — isso mantém os ids históricos estáveis quando novas dimensões
secundárias são adicionadas.

O registro de resultado sempre carrega **todos** os parâmetros efetivos, mesmo
os que não aparecem no id.

## 6. Seleção de subset

Requisito central: qualquer recorte da matriz deve ser executável isoladamente,
sem editar código.

### 6.1 Interface de linha de comando

```text
modb_load list-cases [seletores]
modb_load run [seletores] [orçamento] [--output-dir DIR] [--work-dir DIR] [--seed N]
modb_load resume <arquivo.partial>
modb_load list-profiles
```

Seletores, todos combináveis, todos aceitando lista separada por vírgula e
repetição da flag:

| flag | efeito |
|---|---|
| `--profile NOME` | ponto de partida: conjunto pré-definido de casos (§6.2) |
| `--scale 10k,1M` | restringe D1 |
| `--workload create_only,crud_full` | restringe D2 |
| `--target embedded,loopback` | restringe D3 |
| `--environment ID` | restringe D4 — resolve host/kind pelo catálogo `loadtests/environments.json` |
| `--case ID` | caso exato; ignora perfil e demais seletores |
| `--filter SUBSTR` | casamento por substring no `case_id` (mesma semântica do `modb_bench --filter`) |
| `--exclude SUBSTR` | remove do conjunto após todos os filtros |
| `--concurrency 1,16` | variante de sessões |
| `--payload normal,fat` | variante de payload |
| `--repeat N` | repetições do caso inteiro (padrão 1; ≥ 3 para decisão de A/B) |

Semântica de composição, sem ambiguidade:

1. o perfil define o conjunto inicial;
2. cada seletor de dimensão faz **interseção** com esse conjunto;
3. `--case` substitui tudo por uma lista explícita;
4. `--exclude` subtrai por último;
5. conjunto vazio é erro com código de saída 2 e mensagem listando o que sobrou
   em cada etapa — nunca "executou zero casos com sucesso".

### 6.2 Perfis

| perfil | conjunto | duração alvo |
|---|---|---|
| `load-smoke` | `1k` × todos os workloads × `embedded` | 1–3 min |
| `load-local` | `10k`,`100k` × todos os workloads × `embedded`,`loopback` | 20–40 min |
| `load-standard` | `100k` × todos os workloads × `embedded`,`loopback`,`remote_colocated` + `1M` × `create_only`,`crud_full` × `embedded` | 2–4 h |
| `load-heavy` | `250k`,`500k`,`1M` × todos os workloads × `embedded`,`remote_colocated` + variantes secundárias pairwise | 12–24 h |
| `load-remote` | `10k`,`100k` × `create_only`,`crud_full` × `remote_colocated`,`remote_client_local` | 30–60 min |
| `load-soak` | `500k` × `create_delete_interleaved` em laço por duração fixa | 1–24 h |
| `load-diagnostic` | vazio; exige seletores explícitos | sem meta |

`load-heavy` é pairwise nas dimensões secundárias, não cartesiano: cada valor
não padrão aparece ao menos uma vez, sem multiplicar a matriz.

### 6.3 Planejamento antes de executar

`list-cases` e `run --dry-run` imprimem, em stderr, a lista final com estimativa
por caso e total:

```text
load.create_only.embedded.100k        objetos=100000  disco~=?  tempo~=?
load.crud_full.embedded.1M            objetos=1000000 disco~=?  tempo~=?
--
14 casos  disco de pico estimado ~= ?  tempo total estimado ~= ?
```

As estimativas vêm da tabela de calibração (§10), gravada no repositório e
atualizada por medição, não por chute. Enquanto a calibração não existir, o
comando imprime `?` e `run` exige `--accept-unknown-budget`.

### 6.4 Retomada

Cada caso concluído é uma linha `case_summary` no JSONL. `resume` lê o
`.partial`, reconstrói o conjunto de casos já concluídos e executa apenas o
restante, gravando no mesmo arquivo. Isso torna a matriz `load-heavy` viável em
janelas de manutenção descontínuas.

## 7. Dataset

`user_v1`, sintético e determinístico:

| campo | tipo | conteúdo |
|---|---|---|
| `id` | u64 | sequencial a partir de 1 |
| `login` | string ≤ 16 | derivado do id, único |
| `email` | string ≤ 32 | derivado do id, único |
| `display_name` | string ≤ 24 | corpus sintético versionado |
| `created_at` | i64 | base fixa + id × passo (nunca relógio real) |
| `status` | i32 | enum com distribuição declarada |
| `filler` | bytes | dimensiona o payload para `slim`/`normal`/`fat` |

Regras: geração fora da região medida; `dataset_id`, `dataset_version`, `seed`,
`generator_commit` e `logical_hash` registrados; nenhum dado real, nenhum
relógio de parede dentro do conteúdo (quebraria reprodutibilidade do hash).

Ordem de remoção do `create_delete_interleaved` vem de um gerador com seed
registrada, não de `rand()` global.

## 8. Métricas

Por fase (`create`, `read`, `update_inplace`, `update_grow`, `update_shrink`,
`delete`) e por janela de progresso:

**Tempo e taxa** — duração da fase, objetos/s, commits/s, latência por operação
(p50, p95, p99, p99.9, máximo) medida no lado que emite a operação, histograma
completo preservado, TTFR nas fases de leitura.

**Recursos** — CPU de usuário/sistema, RSS atual e de pico, alocações e pico de
alocação, leituras/escritas físicas e lógicas, `fsync` e sua latência, páginas
lidas/escritas/alocadas/reutilizadas/evictadas, hit rate do cache.

**Espaço e amplificação** — tamanho do banco e do WAL antes e depois de cada
fase, bytes persistidos por objeto lógico, write/space amplification, ocupação
média de página, fragmentação interna e externa, espaço recuperado após
`delete`.

**Rede** (alvos `loopback` e `remote_*`) — bytes e frames enviados/recebidos,
bytes por objeto no fio, syscalls, RTT base medido antes da carga, razão de
compressão quando habilitada.

**Qualidade** — erros, retries, timeouts, cancelamentos, transações abortadas, e
o hash lógico que comprova equivalência entre variantes.

**Séries por janela** — toda fase com duração acima de 30 s emite
`progress_window` a cada janela fixa (padrão 10 s) com taxa, latência, RSS e
tamanho de arquivo da janela. Sem isso, degradação temporal é invisível.

## 9. Validação

Fora da região medida, ao final de cada fase e de cada caso:

1. contagem de objetos confere com o esperado da fase;
2. hash lógico do conjunto confere com o esperado do dataset;
3. após fases de `delete`, nenhum id removido resolve e a contagem é a esperada;
4. após `update`, uma amostra determinística de objetos é lida e comparada campo
   a campo;
5. reabertura do banco ao final do caso, com repetição de (1) e (2), quando
   durabilidade faz parte do caso;
6. `database_check` ao final de todo caso de mutação em escala ≥ `100k`;
7. em alvos remotos com réplica, `applied_lsn` do follower alcança o
   `primary_commit_lsn` e o hash lógico do follower confere.

Qualquer divergência marca o caso como `failed` e a campanha inteira como
`failed`. Nenhuma taxa é publicada para uma fase logicamente incorreta.

## 10. Orçamento de recursos

Cada caso declara antes de executar: objetos, bytes de disco de pico, memória de
pico e duração estimados. `run` verifica espaço livre e aborta com mensagem
clara antes de começar, em vez de encher o disco no meio de 1M.

Flags: `--max-duration`, `--max-disk-gb`, `--max-rss-mb`. Caso que excede o
orçamento gera registro `skipped_budget` com o motivo e o valor estimado.
Campanha com casos pulados termina `partial`, nunca `completed`.

Tabela de calibração, a ser preenchida por medição na Subfase A e versionada no
repositório (um arquivo por plataforma):

| workload | payload | bytes/objeto | objetos/s | disco de pico 1M | duração 1M |
|---|---|---|---|---|---|
| `create_only` | `normal` | a medir | a medir | a medir | a medir |
| `create_delete_forward` | `normal` | a medir | a medir | a medir | a medir |
| `create_delete_reverse` | `normal` | a medir | a medir | a medir | a medir |
| `create_delete_interleaved` | `normal` | a medir | a medir | a medir | a medir |
| `crud_full` | `normal` | a medir | a medir | a medir | a medir |

Extrapolação de 10k para 1M é linear por padrão e marcada como estimativa; após
a primeira execução real de `1M`, o valor medido substitui a extrapolação.

## 11. Execução remota

Fluxo, evoluindo o `scripts/run-remote-benchmark.ps1` atual:

1. host e porta são parâmetros, não constantes no script (o script atual tem o
   IP embutido — isso sai);
2. validação de que o binário é ELF antes do envio, como hoje;
3. validação de espaço livre no host remoto antes de iniciar;
4. execução de `modb_load run` com os mesmos seletores usados localmente;
5. cópia de volta de **exatamente um** arquivo por campanha;
6. `.partial` preservado quando a execução falha, e retomável por `resume`;
7. impressão de caminho local, tamanho, `run_id`, status e SHA-256;
8. nenhuma senha, token ou usuário registrado no resultado; nunca sobrescreve
   arquivo existente.

Para `remote_client_local`, o RTT base e a banda observada são medidos antes da
carga e gravados em `environment`; sem isso os números de rede não são
comparáveis entre execuções.

## 12. Formato do resultado

JSON Lines UTF-8, um objeto por linha, mesmo cabeçalho do plano de benchmarks com
schema próprio:

```json
{"schema":"modb.loadtest","schema_version":1,"record":"...","run_id":"...","sequence":1}
```

| `record` | conteúdo |
|---|---|
| `run_start` | instante, comando, perfil, seletores efetivos, seed |
| `environment` | igual ao plano de benchmarks, mais alvo, host, RTT base e banda |
| `case_plan` | conjunto final de casos e orçamento estimado (uma linha por campanha) |
| `case_start` | `case_id` e todos os parâmetros efetivos |
| `phase_start` | fase, objetos previstos, política de cache |
| `progress_window` | janela temporal com taxa, latência, RSS e tamanho de arquivo |
| `phase_summary` | estatísticas da fase, histograma e métricas de espaço |
| `case_summary` | agregado do caso, validações executadas e veredito |
| `case_error` | erro de preparação, execução ou validação |
| `skipped_budget` | caso não executado por orçamento, com o motivo |
| `run_note` | interferência ou observação |
| `run_end` | duração, contagens, status e hash do conteúdo anterior |

Nome do arquivo, política `.partial` → `.jsonl`, tratamento de inteiros grandes,
unidades no nome da métrica e proibição de segredos seguem §4 do
[PLANO_BENCHMARKS.md](PLANO_BENCHMARKS.md), sem divergência.

O campo `case_id` é gravado **também** como `scenario_id`, para que
`modb_bench compare` funcione sobre arquivos de carga sem alteração no
comparador.

O arquivo de campanha é a verdade primária de **uma execução**. A série ao longo
do tempo é derivada dele e vive em outro lugar (§13); nenhuma análise histórica
depende de manter todos os brutos disponíveis.

## 13. Série histórica

### 13.1 Problema a resolver

Um arquivo por campanha não é uma série. Hoje `benchmark-results/` é ignorado
pelo Git e a única campanha preservada (`benchmark-results-10f/`) foi commitada à
mão — isso não escala, não é consultável e não sobrevive à troca de máquina. Sem
uma camada histórica explícita, deriva lenta (1% por semana) é invisível: cada
comparação par a par passa nos limiares e o produto degrada em silêncio.

### 13.2 Duas camadas, papéis distintos

| camada | onde | conteúdo | ciclo de vida |
|---|---|---|---|
| bruta | `load-results/` (ignorado pelo Git; local ou no host remoto) | campanha completa: janelas, histogramas, amostras | imutável; retenção por política (§13.8) |
| histórica | `load-history/` (versionada no Git) | um *rollup* por (caso, execução), sem janelas nem histogramas | append-only; nunca apagada |

A camada histórica é pequena por construção — dezenas de bytes por caso por
execução — e por isso pode viver no repositório, ser diffável, revisável em PR e
acompanhar o commit que a produziu. A camada bruta é grande (uma campanha `soak`
com janelas de 10 s produz megabytes) e é descartável desde que o rollup
sobreviva.

Camada opcional: espelho dos brutos em storage externo endereçado por SHA-256,
para quando o detalhe de uma execução antiga precisar ser recuperado. O rollup
guarda nome e hash do bruto justamente para tornar isso possível sem manter tudo
localmente.

### 13.3 Registro de rollup

Um objeto JSON por linha em `load-history/series.jsonl`, schema próprio:

```json
{"schema":"modb.loadtest.rollup","schema_version":1,"series_key":"...","case_id":"...","run_id":"...","started_at":"..."}
```

Campos obrigatórios, agrupados por função:

- **identidade temporal** — `run_id`, `case_id`, `started_at` em UTC ISO-8601 com
  milissegundos, `repeat_index` quando `--repeat > 1`;
- **procedência de código** — `commit` completo, `branch`, `tree_dirty`,
  `diff_hash` quando suja, `workload_version`, `dataset_id`,
  `dataset_version`, `seed`;
- **comparabilidade** — `series_key` e `series_key_version` (§13.4);
- **ambiente resumido** — `host_id` anonimizado, `host_class`, SO e versão,
  arquitetura, modelo de CPU, núcleos físicos/lógicos, RAM, filesystem, classe do
  dispositivo, tipo de build, compilador e versão, sanitizers, page size,
  versões de formato e protocolo;
- **métricas por fase** — para cada fase: operações, duração, ops/s, p50, p95,
  p99, p99.9, bytes por objeto, tamanho de banco e WAL ao final, RSS de pico,
  páginas lidas/escritas/reutilizadas, erros;
- **agregados do caso** — duração total, disco de pico, RSS de pico, espaço
  recuperado após `delete`, write/space amplification;
- **inclinação temporal** (casos com janelas) — taxa e latência da primeira e da
  última janela, e a inclinação da regressão simples entre elas: é o que torna
  degradação intra-execução visível na série histórica;
- **veredito** — `status`, lista de validações executadas, `comparable`;
- **rastreabilidade** — nome do arquivo bruto e seu SHA-256.

Um rollup sem `commit`, `series_key`, `host_class`, tipo de build, `seed` ou
`status` é **rejeitado** pelo indexador com erro. Ponto histórico sem procedência
é ruído que envenena a série anos depois; recusar na entrada é mais barato que
limpar depois.

Nomes de campo canônicos — o dashboard (§13.11) lê exatamente estes:

```json
{"schema":"modb.loadtest.rollup","schema_version":1,
 "series_key":"a1b2c3d4e5f60718","series_key_version":1,
 "case_id":"load.create_only.embedded.100k",
 "workload":"create_only","target":"embedded","scale":"100k","objects":100000,"variant":"",
 "run_id":"019f2c...","started_at":"2026-08-07T03:12:44.120Z","repeat_index":0,
 "commit":"a1b2c3d4...","commit_short":"a1b2c3d","branch":"master","tree_dirty":false,"diff_hash":null,
 "workload_version":1,"dataset_id":"user_v1","dataset_version":1,"seed":"123456",
 "env":{"host_id":"h7c1a2","host_class":"bench-linux-01","os":"Linux 6.8.0","arch":"x86_64",
        "cpu_model":"AMD EPYC 7443P","cores_physical":8,"cores_logical":16,"ram_gb":32,
        "fs":"ext4","device_class":"nvme","build_type":"Release","compiler":"gcc 14.2",
        "sanitizers":"none","page_size":4096,"format_version":1,"protocol_version":1},
 "params":{"payload":"normal","batch":1000,"concurrency":1,"readers":0,
           "durability":"sync_real","cache":"warm","primary_storage":"full"},
 "phases":[{"phase":"create","operations":100000,"duration_ns":763000000,
            "ops_per_second":131062,"latency_ns":{"p50":5490,"p95":21400,"p99":33600,"p999":77200},
            "bytes_per_object":271,"db_bytes":36180000,"wal_bytes":29800000,
            "peak_rss_bytes":132120576,"pages_read":31000,"pages_written":42000,
            "pages_reused":0,"errors":0}],
 "totals":{"duration_ns":763000000,"peak_disk_bytes":69680000,"peak_rss_bytes":132120576,
           "reclaimed_bytes":0,"write_amplification":2.44,"space_amplification":1.35},
 "windows":{"first_ops_per_second":135000,"last_ops_per_second":128400,
            "slope_ops_per_second_per_min":-390,"first_p99_ns":30100,"last_p99_ns":34200},
 "status":"completed","comparable":true,
 "validations":["count","logical_hash","reopen","database_check"],
 "raw_file":"modb-load-20260807T031244Z-a1b2c3d-bench01.jsonl","raw_sha256":"9f81c2…"}
```

Campos ausentes são `null`, nunca zero inventado; unidades fazem parte do nome
(`_ns`, `_bytes`, `ops_per_second`), como no plano de benchmarks.

### 13.4 `series_key` — o que pode ser comparado com o que

`series_key` é um hash estável sobre o conjunto de atributos que precisam ser
idênticos para dois pontos pertencerem à mesma série:

```text
case_id, workload_version, dataset_id, dataset_version,
parâmetros semânticos efetivos (escala, payload, lote, concorrência,
  durabilidade, cache, primary_storage),
classe de build, arquitetura, page size, versão de formato, versão de protocolo,
host_class, alvo de execução
```

Regras:

1. mudança semântica em workload ou dataset incrementa sua versão, o que produz
   **nova série** — nunca se mistura com a antiga;
2. trocar de máquina, compilador ou classe de build também produz nova série; o
   relatório mostra a descontinuidade explicitamente em vez de emendar duas
   séries incomparáveis;
3. `series_key_version` acompanha a fórmula do hash, para que uma mudança na
   própria definição possa ser recomputada sobre os rollups existentes sem perder
   histórico;
4. `host_class` é um rótulo configurado (`dev-windows`, `bench-linux-01`), não o
   hostname — permite trocar de hardware equivalente sem quebrar a série de
   propósito, e a descontinuidade fica registrada em `run_note`.

### 13.5 Indexação

```text
modb_load index [--scan DIR] [--history load-history/series.jsonl] [--dry-run]
```

Lê campanhas brutas (`.jsonl` finais e `.partial`), extrai um rollup por caso e
faz append no arquivo histórico. Propriedades exigidas:

- **idempotente** — dedup por (`run_id`, `case_id`, `repeat_index`); reexecutar o
  índice sobre o mesmo diretório não duplica pontos;
- **append-only** — nunca reescreve linha existente; correção se faz por nova
  linha com `supersedes` apontando para o `run_id` corrigido;
- **ordenação por `started_at`**, jamais por data de modificação de arquivo;
- **casos falhos entram na série** com `status=failed`; apagar fracasso da
  história é a forma mais eficiente de repetir o mesmo erro;
- `--dry-run` imprime o que seria acrescentado, com os motivos de rejeição.

O indexador roda ao final de `run` por padrão (`--no-index` desliga) e também
manualmente sobre brutos trazidos de execução remota.

### 13.6 Consulta

```text
modb_load trend  --case ID [--metric ops_per_second] [--phase create] [--last N] [--since DATA]
modb_load trend  --series-key HASH [...]
modb_load report --format md|csv [seletores]   # exporta a série para análise externa
```

`trend` imprime uma linha por ponto: data UTC, commit curto, valor, delta em
relação ao ponto anterior, delta em relação à mediana das últimas K (padrão 5) e
marca de outlier. Regras de leitura embutidas na ferramenta:

- menos de 3 pontos na série → imprime os dados e recusa emitir veredito;
- a referência é a **mediana móvel**, não o ponto anterior isolado, que é ruído;
- pontos com `comparable=false` aparecem na listagem e ficam fora do cálculo;
- quebra de `series_key` é impressa como linha separadora explícita.

`report` existe porque nenhuma ferramenta interna vai cobrir toda análise futura:
CSV e Markdown permitem levar a série para planilha, notebook ou gráfico sem
depender do binário.

### 13.7 Regressão pontual e deriva lenta

Dois mecanismos distintos, porque detectam coisas distintas:

| mecanismo | compara | pega |
|---|---|---|
| gate por execução | candidato × mediana das últimas 5 da mesma série | regressão abrupta introduzida por um commit |
| deriva lenta | mediana das últimas 5 × mediana de 20 pontos atrás | degradação de 1–2% por execução que nenhum gate pontual acusa |

Limiares herdados de §11 do [PLANO_BENCHMARKS.md](PLANO_BENCHMARKS.md), sem
divergência: alerta em 5% de mediana/throughput, falha em 10%, p99 em 15%,
espaço/WAL/bytes de rede em 10%, e qualquer divergência de correção é falha
imediata. Para deriva, o limiar é acumulado: 15% entre as duas medianas.

`modb_load gate --case ID` retorna código de saída utilizável em CI. Nenhum gate
decide com série de menos de 3 pontos: retorna `insufficient_history` e sucesso,
para não bloquear por falta de dados.

### 13.8 Retenção

Brutos: manter os N últimos por série (padrão 10), todos os marcados como
baseline, todos com `status=failed` e todos com `run_note` de interferência.
Acima de 30 dias, comprimir. Remoção só por `--prune --confirm`, e o rollup do
bruto removido permanece — com o hash, para que a ausência seja detectável.

Rollups: nunca apagados, nunca reescritos. São a memória do projeto.

### 13.9 Baselines marcadas

`load-history/baselines.json` mapeia `series_key` → `run_id` escolhido
explicitamente, com data e motivo em texto. Uma baseline é uma decisão humana
registrada, não "a execução mais antiga" nem "a melhor". Imutável: substituir uma
baseline é acrescentar entrada nova com o motivo da troca.

### 13.10 Anonimização

`host_id` é hash do hostname com salt local configurado (`MODB_LOAD_HOST_SALT`),
nunca o hostname bruto. Nenhum usuário, token, endereço IP de cliente ou caminho
real entra no rollup; caminhos são normalizados. A série histórica é versionada no
Git — o que entra nela é público para todo mundo que tem o repositório.

### 13.11 Dashboard

`loadtests/dashboard/index.html` — arquivo único, sem dependências, que abre por
`file://` e lê o `series.jsonl` no próprio navegador (nada é enviado para
serviço externo). Implementado; consome o schema de §13.3 sem conversão.

Entrada: seletor de arquivo, arrastar-e-soltar, ou `?src=…` quando a pasta é
servida por HTTP. Um botão carrega uma série sintética rotulada, para inspecionar
as leituras do painel antes de existir medição real.

O painel é a leitura visual das regras deste capítulo, não um enfeite:

| elemento | regra que ele torna visível |
|---|---|
| tendência com mediana móvel e limiares de alerta/falha desenhados | §13.7: o ponto é comparado com a mediana das 5 anteriores, não com o anterior |
| separador vertical de `series_key` | §13.4: séries incomparáveis não são emendadas; a mediana reinicia na quebra |
| marca de veredito por ponto + pino de deriva | §13.7: gate pontual e deriva lenta são mecanismos distintos |
| gráfico de escala com referência de custo por objeto constante | §4.1: 10k → 1M é linear ou superlinear? |
| composição por fase | §4.2: cada fase medida separadamente |
| tabela com Δ anterior, Δ mediana e exportação CSV | relevo da paleta e §13.6: nenhum valor existe só no gráfico |

Regras de leitura embutidas, iguais às da CLI: menos de 3 pontos não recebe
veredito, pontos `comparable=false` aparecem mas ficam fora do cálculo, falhas
aparecem em vermelho em vez de sumir, e "hoje" é o ponto mais recente da série —
não o relógio da máquina, para que histórico antigo continue legível.

## 14. Artefatos a implementar

```text
loadtests/
  modb_load.cpp                  CLI: run, list-cases, resume, index, trend, report,
                                 gate, list-profiles
  matrix.hpp/.cpp                dimensões, expansão, seletores, ids
  profiles.hpp/.cpp              load-smoke ... load-soak
  budget.hpp/.cpp                estimativas, calibração, guarda-corpos
  dataset_user.hpp/.cpp          gerador user_v1
  target.hpp                     interface comum das operações CRUD
  target_embedded.cpp            implementação in-process
  target_client.cpp              implementação via net::Client (loopback/remoto)
  history/
    rollup.hpp/.cpp              extração campanha -> rollup
    series_key.hpp/.cpp          hash de comparabilidade e versionamento
    index.cpp                    append idempotente em series.jsonl
    trend.cpp                    série, mediana móvel, outliers, quebras
    gate.cpp                     regressão pontual e deriva lenta
  dashboard/
    index.html                   painel da série histórica (implementado, §13.11)
  workloads/
    create_only.cpp
    create_delete_forward.cpp
    create_delete_reverse.cpp
    create_delete_interleaved.cpp
    crud_full.cpp
  calibration/
    windows-x86_64.json
    linux-x86_64.json
scripts/
  run-load.ps1 / run-load.sh
  run-remote-load.ps1
tests/
  load_matrix_test.cpp           expansão, seletores, ids, conjunto vazio
  load_workload_test.cpp         cada workload em escala minúscula, invariantes
  load_history_test.cpp          rollup, series_key estável, idempotência do
                                 index, mediana móvel, deriva, quebra de série
load-history/                    versionado no Git
  series.jsonl                   append-only, um rollup por (caso, execução)
  baselines.json                 series_key -> run_id escolhido, com motivo
load-results/                    ignorado pelo Git
```

Reuso direto, sem cópia: `benchmarks/runner/jsonl_writer`, `environment`,
`sha256` e `json_util`. Se necessário, esses quatro sobem para um alvo comum
(`bench_runner_core`) consumido pelos dois executáveis.

A chave do desenho é `target.hpp`: uma interface de CRUD sobre `User` com duas
implementações. Workload e matriz não sabem se estão embedded ou em rede — é o
que permite que o mesmo caso rode nos quatro alvos.

## 15. Ordem de implementação

Uma subfase por branch, conforme a convenção do projeto.

| subfase | entrega | critério de pronto |
|---|---|---|
| A | matriz, ids, seletores, perfis, `list-cases`, `--dry-run`, `budget` sem calibração | `load_matrix_test` verde; subset selecionável e imprimível sem executar carga |
| B | writer, records, `dataset_user`, `target_embedded`, workload `create_only`, escalas `1k`/`10k` | `load-smoke` gera JSONL válido com validação de invariante |
| C | `series_key`, rollup, `index` idempotente, `trend`, `report` | duas execuções de `load-smoke` produzem dois pontos na mesma série; reindexar não duplica; o dashboard (§13.11) abre o `series.jsonl` gerado sem conversão |
| D | `create_delete_forward`, `create_delete_reverse`, `create_delete_interleaved` | invariantes de contagem zero e espaço recuperado registrados |
| E | `crud_full` com as seis fases separadas | leitura campo a campo confere; `phase_summary` por fase |
| F | `progress_window`, inclinação no rollup, `case_summary`, `resume` | interrupção em 100k retomável sem reexecutar caso concluído |
| G | `target_client`, alvo `loopback`, métricas de rede | `load-local` cobre embedded e loopback com o mesmo caso |
| H | escalas `250k`/`500k`/`1M`, calibração medida, guarda-corpos ativos | tabela §10 preenchida; caso acima do orçamento pulado com registro |
| I | scripts remotos, `remote_colocated`, `remote_client_local`, indexação dos brutos trazidos | `load-remote` traz exatamente um arquivo do host, com hash, e ele entra na série |
| J | `gate`, deriva lenta, retenção e `--prune`, baselines marcadas | regressão sintética de 12% e deriva sintética de 15% são detectadas |
| K | dimensões secundárias, `load-heavy` pairwise, `load-soak` | cada valor não padrão exercitado ao menos uma vez |

A e B são o mínimo útil: já entregam "executar apenas um subset" com um workload
real. **C vem antes dos demais workloads de propósito**: a partir dela toda
execução já deposita um ponto histórico, então nenhuma medição feita entre C e K
é perdida. Se a história fosse a última subfase, todo o trabalho de D a I
produziria números que ninguém consegue comparar depois.

## 16. Critérios de aceite

O plano estará implementado quando:

- `modb_load list-cases --profile load-standard` imprimir a matriz com
  orçamento estimado sem executar nada;
- `modb_load run --workload create_delete_reverse --scale 100k --target embedded`
  executar exatamente um caso e produzir um único JSONL final;
- cada combinação de D1 × D2 × D3 declarada nos perfis for executável
  isoladamente por seletores, sem editar código;
- interromper uma campanha deixar `.partial` legível e `resume` completar o
  restante sem repetir casos concluídos;
- todo workload validar suas invariantes e uma corrupção injetada resultar em
  `failed`, nunca em número publicado;
- as escalas `10k` a `1M` estarem cobertas por medição real ao menos uma vez em
  Windows e em Linux, com a tabela de calibração preenchida;
- um caso acima do orçamento de disco ou tempo ser pulado com registro explícito
  e a campanha terminar `partial`;
- uma campanha remota trazer exatamente um arquivo, com `run_id`, status e
  SHA-256 impressos, e nenhum segredo no conteúdo;
- três execuções do mesmo caso no mesmo ambiente ficarem dentro da variação
  declarada para os casos-gate.

Especificamente para a série histórica:

- toda execução, local ou remota, depositar um rollup em
  `load-history/series.jsonl` com procedência completa, e um rollup incompleto ser
  rejeitado com erro;
- `modb_load index` ser idempotente: reindexar o mesmo diretório duas vezes não
  alterar o arquivo histórico;
- `modb_load trend --case load.create_only.embedded.100k` mostrar a série
  completa com data, commit, valor e delta contra a mediana móvel;
- uma regressão sintética de 12% ser reprovada pelo `gate`, e uma deriva
  sintética de 15% distribuída em 20 execuções ser detectada pelo mecanismo de
  deriva mesmo quando nenhum passo individual dispara o gate;
- troca de host, compilador ou classe de build produzir nova série e aparecer
  como descontinuidade explícita no relatório, nunca como queda de desempenho;
- remover os brutos por retenção não impedir nenhuma análise histórica: a série
  continua completa e a ausência do bruto é detectável pelo hash;
- o arquivo histórico permanecer pequeno o suficiente para ser versionado e
  revisado em PR (ordem de KB por mês de uso regular);
- o dashboard (§13.11) abrir o `series.jsonl` real sem conversão, e mostrar a
  mesma leitura que `modb_load trend`/`gate` para o mesmo caso e métrica — se
  divergirem, um dos dois está errado.

## 17. Riscos e questões abertas

1. **Sentido de "usuários"** — este plano assume volume de registros (§2). Se o
   alvo for sessões simultâneas, D1 e concorrência trocam de papel; decidir
   antes da Subfase A, porque muda os perfis, não a arquitetura.
2. **1M em `wal_only`** — com o primary só WAL
   ([ADR-017](decisions/ADR-017-primary-wal-only-sem-arquivos-de-dados.md)), 1M
   objetos viram crescimento de log; sem política de retenção/checkpoint medida,
   o caso mede o disco enchendo. Manter `wal_only` fora dos perfis padrão até a
   Subfase K.
3. **Escrita não escala com sessões** — motor single-writer (Fase 5). Casos `c4`
   e `c16` devem ser lidos como contenção e fairness; documentar isso no
   resultado para evitar interpretação errada de "não escala".
4. **Disco em `load-heavy`** — a matriz pesada em 1M pode exigir dezenas de GB;
   o guarda-corpo da Subfase H é pré-requisito para rodá-la sem supervisão.
5. **`remote_client_local` em escala alta** — mede o enlace, não o banco;
   limitado a `10k`/`100k` de propósito.
6. **Duplicação com benchmarks** — se `runner/` não for extraído para um alvo
   comum na Subfase B, as duas árvores divergem. Extrair na primeira necessidade,
   não depois.
7. **Toolchain** — `cmake`/`mingw` do projeto vivem dentro do CLion; a Subfase A
   deve confirmar que o novo alvo compila pelo preset usado hoje antes de crescer.
8. **Série curta demais para decidir** — carga em `1M` não roda a cada commit, e
   uma série com 3 pontos por trimestre não sustenta gate. Consequência aceita: as
   escalas altas produzem série de tendência (análise humana), e o gate automático
   fica restrito a `10k`/`100k`, que rodam com frequência. Decidir a cadência de
   cada perfil junto com a Subfase J, não depois.
9. **Máquina de desenvolvimento na série** — pontos coletados em máquina de
   trabalho com carga concorrente são ruidosos. `host_class` separa as séries, mas
   é fácil esquecer de configurá-lo e contaminar a série oficial. Mitigação: sem
   `host_class` explícito, o padrão é `dev-<plataforma>`, jamais o rótulo do host
   de bench.
10. **Rollup versionado gera conflito de merge** — arquivo append-only tocado por
    vários branches conflita. Mitigação: uma linha por ponto, ordenação por
    `started_at` na leitura e não no arquivo, e resolução de conflito por união
    das linhas — o `index` detecta duplicata por `run_id` de qualquer forma.
11. **Deriva de ambiente confundida com regressão** — atualização de SO,
    firmware ou driver de disco muda a linha de base sem mudar `series_key`.
    Mitigação: `run_note` obrigatório quando o `environment` diverge do ponto
    anterior da série em campo relevante, e o relatório marca o ponto.
