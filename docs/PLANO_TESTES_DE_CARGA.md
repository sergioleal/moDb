# Plano de testes de carga do Ring0

- Estado: em implementação incremental (Subfases A e B concluídas: matriz,
  seletores, perfis, budget sem calibração, `modb_load run`/`list-cases`/
  `list-profiles`, workload `create_only` real contra o motor embedded)
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

Regra de fases: cada fase é cronometrada, validada e resumida separadamente. Um
`crud_full` em 1M produz seis `phase_summary`, não um número único.

#### 4.2.1 Workloads adicionais — isolam comportamentos específicos do motor

A escada acima mede o caminho de armazenamento (heap, páginas, fragmentação).
Ela não diz nada sobre cache, MVCC, blobs, integridade referencial, recuperação
ou réplica — cada um desses tem um modo de falha próprio que só aparece sob
volume real e duração real, não em uma amostra de benchmark de alguns segundos.
Os workloads abaixo isolam um comportamento por vez, do mesmo jeito que a
escada isola um padrão de mutação por vez. Nenhum deles substitui a escada;
"todos os workloads" nas descrições de perfil (§6.2) refere-se só à escada
básica — estes exigem seleção explícita por `--workload` ou o perfil
`load-behavior`.

| id | fases | o que estressa | invariante final |
|---|---|---|---|
| `read_hotspot` | create → read (Zipf sobre working set fixo) | pressão de buffer pool/page cache sob leitura enviesada | valores lidos == esperados; hit rate do cache registrado |
| `range_scan_sweep` | create → scan (seletividade 0,01%–100%) | custo de índice vs. scan completo variando seletividade e volume | contagem retornada == esperada por seletividade; plano (índice/scan) registrado |
| `mixed_oltp` | fase única, sessões concorrentes emitindo create/read/update/delete numa proporção configurada (padrão 5/80/10/5) | contenção real e cauda de latência sob mistura — não sob uma operação isolada e repetida | contagem final reconcilia (criados − removidos); checksum de amostra determinística confere |
| `snapshot_hold` | create → abrir snapshot(s) → churn (create/update/delete) → fechar snapshot(s) | retenção de versões MVCC sob volume e duração reais, GC ao fechar | leitura pela snapshot aberta permanece idêntica ao estado da abertura durante todo o churn; versões retidas e bytes registrados |
| `blob_lifecycle` | create (com blob) → read/stream → grow → shrink → delete | `BlobStore` sob volume e tamanhos variados (1, 16, 256 MiB) | hash byte a byte do blob lido == escrito; espaço recuperado após delete |
| `cascade_delete` | create_hierarchy (profundidade × largura) → cascade_delete (raiz) | integridade referencial e custo de remoção em cascata escalando com nº de descendentes | zero refs órfãs; total removido == total criado |
| `oversubscribed_churn` | create → churn interleaved, cache explicitamente menor que o working set | degradação graciosa vs. catastrófica quando o volume ultrapassa o cache — versão em volume real do cenário `storage.buffer_pool.oversubscribed` (Fase 10) | mesmas invariantes de `create_delete_interleaved`, mais razão de eviction/releitura registrada |
| `restart_recovery` | churn → kill em ponto definido (mid-transação, pós-commit, mid-checkpoint) → restart → verify | custo e corretude do replay de WAL escalando com volume | hash lógico pós-recuperação == hash do último commit durável; tempo de recuperação registrado |

`range_scan_sweep` e `cascade_delete` formalizam, respectivamente, os antigos
`crud_query` e `crud_relationships` — deixam de ser placeholder e passam a ter
fases e invariantes definidos.

**Implementado na Subfase L**: `read_hotspot` e `range_scan_sweep`, só
`embedded`. `read_hotspot` usa `database.page_file().buffer_pool().metrics()`
para o hit rate real (`PhaseMetrics.cache_hit_rate`, novo campo -- -1.0 nas
fases que não medem isso) e um amostrador Zipf com CDF pré-computada sobre o
working set; valida os valores lidos comparando o hash na mesma ordem em que
foram lidos (não a ordem de criação). `range_scan_sweep` cria um índice em
`User.id` e roda uma fase por seletividade (0,01%/0,1%/1%/10%/100%), cada
fase nomeada com o `AccessMethod` real do `query::QueryPlan` (`scan_1pct_index_scan`,
por exemplo) -- o "plano registrado" do critério de pronto vira parte do
próprio nome da fase, não um campo novo no schema.

Dois workloads adicionais dependem de infraestrutura que o harness genérico
(`target.hpp`, §14) ainda não cobre; ficam **fora de todos os perfis** até essa
infraestrutura existir — mesmo tratamento hoje dado a `primary_storage=wal_only`
(§4.5 secundárias, risco 2 em §17):

| id | fases | o que estressa | depende de |
|---|---|---|---|
| `schema_evolution` | create_v1 → leitura/escrita concorrente sob bindings v1 e v2 → verify | custo de migração/compatibilidade sem parar o mundo | harness de duas versões de `Binding` simultâneas — não existe no `target.hpp` genérico |
| `replica_catchup` | primary_churn → medir lag → rajada → catchup | lag de replicação crescendo com volume; tempo de recuperação após rajada | réplica de leitura orquestrada pelo próprio `modb_load` — hoje `primary_storage` é só parâmetro do primary, não uma topologia com follower |

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

**Implementado na Subfase G (versão mínima).** `loopback` funciona só para
`create_only` (`loadtests/target_client.cpp` + `loadtests/loadtest_facade.cpp`):
um `net::Server` real sobe em `127.0.0.1` (porta OS-assigned), um
`app::ServerConnection` conecta e invoca `CreateBatch` (uma `ops::Operation`
de facade, um lote por `--batch`, mesma semântica de commit-por-lote do
`embedded`) e a validação de hash relê TUDO via `collect()` (query remota),
ordenando pelo campo lógico `id` antes de comparar -- a ordem de um scan
remoto não é garantida ser a ordem de criação, ao contrário do
`embedded`, que relê pelos próprios ids na ordem em que foram criados.
`create_delete_*`/`crud_full` continuam recusando `loopback` no próprio
wrapper (`workloads/*.cpp`), não implementados nesta subfase. Métricas de
rede propriamente ditas (bytes/frames/syscalls/TTFR, coluna "mede" da
tabela acima) **não** são coletadas ainda -- cliente e servidor rodam no
MESMO processo (um `std::thread` aceita a conexão), então `peak_rss_bytes`
reflete os dois combinados, não um custo de rede isolado; `latency_ns` tem
granularidade por LOTE (uma viagem de rede por `invoke`), não por objeto
como no `embedded` -- os dois números não são comparáveis ponto a ponto
entre alvos. Fechar essas lacunas (processos separados, métricas de rede
reais) fica para uma iteração futura desta subfase.

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
| `load-behavior` | `100k` × `read_hotspot`,`range_scan_sweep`,`mixed_oltp`,`snapshot_hold`,`blob_lifecycle`,`cascade_delete`,`oversubscribed_churn` × `embedded` | 1–2 h |
| `load-diagnostic` | vazio; exige seletores explícitos | sem meta |

`load-heavy` é pairwise nas dimensões secundárias, não cartesiano: cada valor
não padrão aparece ao menos uma vez, sem multiplicar a matriz. "Todos os
workloads" em `load-smoke`/`load-local`/`load-standard`/`load-heavy` refere-se
só à escada básica (§4.2); os workloads de §4.2.1 só entram via
`load-behavior` ou seleção explícita. `restart_recovery` fica fora de todo
perfil automático — mata o processo de propósito, então roda só sob
`--workload restart_recovery` explícito, com o operador ciente. `schema_evolution`
e `replica_catchup` ficam fora de todo perfil até a infraestrutura de que
dependem existir (tabela em §4.2.1).

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

**Implementado na Subfase F.** `resume <arquivo.partial>` reconstrói cada caso
pendente a partir do seu próprio `case_start` já gravado (nomes de campo, não o
texto do `case_id` — que não decodifica dimensões secundárias fora do padrão),
considera "concluído" todo `case_id` com `case_summary` **ou** `case_error`, e
recusa retomar (erro explícito, não reexecução silenciosa) um caso que foi
interrompido antes de emitir seu próprio `case_start`. `--work-dir` é opcional
(não é persistido no schema §12; por padrão usa o diretório do próprio
`.partial`, igual ao `run` sem `--work-dir`).

### 6.5 Configuração via YAML (`scripts/run-load.ps1` / `run-load.sh`)

**Implementado.** Os dois scripts são um wrapper de execução local sobre os
seletores de §6.1 — não um formato do próprio `modb_load` (que ainda não
existe; Subfases A/B). Eles leem `loadtests/config/*.yaml`, um subconjunto
restrito de YAML documentado no cabeçalho de
`loadtests/config/load-local.yaml` (chave: valor escalar; chave: seguida de
`  - item` para lista; sem aspas, sem lista em uma linha, sem aninhamento além
de um nível — parseado à mão, erro de formato é falha de parse, não
best-effort).

```bash
./scripts/run-load.sh                                  # usa load-local.yaml
./scripts/run-load.sh --config loadtests/config/x.yaml --dry-run
./scripts/run-load.sh --environment linux-remoto        # sobrescreve o yaml
```

```powershell
.\scripts\run-load.ps1
.\scripts\run-load.ps1 -ConfigPath loadtests\config\x.yaml -DryRun
.\scripts\run-load.ps1 -Environment linux-remoto
```

Cada chave do YAML mapeia para uma flag de §6.1 (`scale`/`workload`/`target`/
`environment`/`concurrency`/`payload`/`case` são listas, viram
`--flag valor1,valor2`); `accept_unknown_budget` e `dry_run` são booleanos —
o segundo passa `--dry-run` para o `modb_load` em si (ele só imprime o plano,
§6.3), distinto do `-DryRun`/`--dry-run` do próprio script (esse nem exige o
binário existir, só mostra o comando resolvido).

Os scripts validam os ids de `environment:` contra `loadtests/environments.json`
(§4.4) **antes** de montar o comando — erro de digitação falha ali, não depois
do `modb_load` ter subido — e avisam (não bloqueiam) quando o ambiente é
`kind=ssh`, porque estes dois scripts executam localmente; despacho remoto
continua sendo `scripts/run-remote-benchmark.ps1` (ou o futuro
`run-remote-load`). A validação em `run-load.sh` é best-effort: sem `jq`
instalado, ela avisa e segue, porque `jq` não é dependência obrigatória do
projeto.

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

`blob_lifecycle` (§4.2.1) usa `dataset_user_blob`, uma variante separada — não
uma extensão de `user_v1` — que soma um campo de blob de tamanho configurável
(1, 16, 256 MiB). Os demais workloads continuam sobre `user_v1` sem blob; risco
15 (§17) explica por quê.

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

**Implementado na Subfase F.** Na prática o corte é "pelo menos uma janela de
`window_interval` fechou" (fase mais curta que o intervalo não emite nenhuma
janela, nem uma de cauda) — mais simples que medir os 30 s de antemão e com o
mesmo efeito: fases curtas nunca produzem `progress_window`. `case_summary`
carrega `windows{first_ops_per_second,last_ops_per_second,
slope_ops_per_second_per_min,first_p99_ns,last_p99_ns}` (da primeira fase do
caso que fechou alguma janela) ou `null` quando nenhuma fechou; o rollup
(§13.3) repassa esse campo tal como gravado.

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

| workload | payload | bytes/objeto | objetos/s (100k) | disco de pico 1M | duração 1M |
|---|---|---|---|---|---|
| `create_only` | `normal` | 458 | 6288 | 458.000.000 | 159 s (extrapolado) |
| `create_delete_forward` | `normal` | 458 | 5555 | 458.000.000 | 180 s (extrapolado) |
| `create_delete_reverse` | `normal` | 458 | 5584 | 458.000.000 | 179 s (extrapolado) |
| `create_delete_interleaved` | `normal` | 458 | 5323 | 458.000.000 | 188 s (extrapolado) |
| `crud_full` | `normal` | 1674 | 413 | 1.674.000.000 | 2420 s (extrapolado) |

Extrapolação de 10k para 1M é linear por padrão e marcada como estimativa; após
a primeira execução real de `1M`, o valor medido substitui a extrapolação.

**Implementado na Subfase H (calibração reduzida).** `loadtests/calibration/
windows-x86_64.json` traz medição real em `10k` e `100k` para os 5 workloads
implementados (`payload=normal`); `1k`/`250k`/`500k`/`1M` são extrapolação
linear simples a partir do ponto de `100k`, marcada por entrada
(`extrapolation_caveat`). A vazão caiu entre 2,4x (`create_only`) e 5,1x
(`crud_full`) só de `10k` para `100k` — um comportamento não-linear real do
motor em escala crescente, não um artefato de medição -- então os valores de
`250k`/`500k`/`1M` acima são conhecidos por serem **otimistas** (a duração
real tende a ser maior). Substituir por medição real nessas escalas maiores
segue como trabalho futuro. `loadtests/calibration/linux-x86_64.json` não
existe ainda (sem ambiente Linux disponível nesta rodada de calibração) --
`estimate_case` simplesmente devolve `known=false` para essa plataforma, o
mesmo comportamento de antes da Subfase H.

`budget.cpp`/`campaign.cpp` usam essa tabela de verdade: `estimate_case`
consulta `loadtests/calibration/<plataforma>-<arch>.json` (resolvido em
tempo de compilação); um caso com estimativa conhecida que excede
`--max-duration`/`--max-disk-gb`/`--max-rss-mb` gera `skipped_budget` e é
pulado (a campanha termina `partial`); antes de começar, `run` soma o disco
de pico de todos os casos com estimativa conhecida e aborta com mensagem
clara se o espaço livre em `--work-dir` for insuficiente.

## 11. Execução remota

Fluxo, evoluindo o `scripts/run-remote-benchmark.ps1` atual:

1. host, usuário e caminho remoto vêm do ambiente registrado (§4.4,
   `--environment ID`), resolvidos por `loadtests/environments.json` — não são
   mais constantes no script. **Já implementado**: o script atual aceita
   `-Environment <id>` e recusa um ambiente cujo `kind` não seja `ssh`;
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

**Implementado na Subfase I (parcial).** `scripts/run-remote-load.ps1` cobre
1, 2, 4, 5, 7 e 8 (item 6 vem de graça do `resume` da Subfase F, que já
funciona sobre qualquer `.partial`, remoto ou não; a checagem de espaço livre
do item 3 é a de `run_campaign` já implementada na Subfase H, mas mede
espaço livre em `--work-dir` LOCAL do host que executa -- quando o alvo é
remoto, isso já roda no próprio host remoto, então a checagem vale, só não
foi testada de verdade contra um host de verdade). O alvo `remote_colocated`
reaproveita o dispatch de `loopback` sem nenhuma linha de código nova
(§4.3: a diferença entre os dois é só ONDE o binário roda). `remote_client_local`
segue **sem dispatch** -- item 3 acima (RTT/banda) não tem onde ser medido
sem ele.

Honestidade sobre o que não foi verificado: o único ambiente `kind=ssh`
cadastrado (`linux-remoto`) tinha a chave SSH diferente da registrada em
`known_hosts` no momento desta subfase (aviso de segurança do OpenSSH, não
contornado de propósito -- pode ser um host reprovisionado ou algo pior, e
não é uma decisão que este agente deveria tomar sozinho). O script foi
escrito seguindo de perto o padrão já em produção de
`run-remote-benchmark.ps1` e validado localmente até onde dá sem rede
(resolução de ambiente, seleção `kind=ssh`, checagem de binário ELF) --
**o round-trip completo por SSH nunca rodou de verdade**. Antes do primeiro
uso real: resolver o aviso de host key (deliberadamente, não com
`-o StrictHostKeyChecking=no`) e compilar `modb_load` para Linux.

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

**Implementado na Subfase A/B**: `run_start`, `environment`, `case_plan`,
`case_start`, `phase_start`, `phase_summary`, `case_error`, `case_summary`,
`run_end` — o suficiente para `create_only` produzir um arquivo válido e
completo. **Implementado na Subfase F**: `progress_window` (a cada `window_interval`,
padrão 10 s, só em fases que de fato fecham uma janela) e `resume`.
**Implementado na Subfase H**: `skipped_budget`, emitido quando um caso com
estimativa calibrada excede `--max-duration`/`--max-disk-gb`/`--max-rss-mb`.
`run_note` conforme os casos que o exigem (interferência de ambiente, §17
risco 11) forem aparecendo.

O campo `case_id` é gravado **também** como `scenario_id`, para que
`modb_bench compare` funcione sobre arquivos de carga sem alteração no
comparador. **Ainda não implementado**: `modb_bench compare` hoje só lê linhas
com `"record":"scenario_summary"` (`benchmarks/runner/campaign.cpp:145`), e o
`modb_load` desta subfase emite `case_summary` (record diferente, por design —
ver tabela acima). Fazer as duas pontes conversarem é trabalho da Subfase J
(junto com `gate`), não da A/B; até lá, comparar campanhas de carga exige ler
o JSONL diretamente.

O arquivo de campanha é a verdade primária de **uma execução**. A série ao longo
do tempo é derivada dele e vive em outro lugar (§13); nenhuma análise histórica
depende de manter todos os brutos disponíveis.

A palavra "ambiente" nomeia três coisas distintas neste plano — deliberadamente
relacionadas, nunca o mesmo campo:

| termo | é | onde vive |
|---|---|---|
| ambiente registrado (D4, §4.4) | identidade cadastrada (`desktop-windows`, `linux-remoto`) — onde o comando executa | `loadtests/environments.json`; campo `environment` no rollup |
| record `environment` (esta seção) | hardware/SO/rede efetivos de **uma execução** | uma linha por campanha bruta |
| `env` (rollup, §13.3) | resumo do hardware/build, derivado do record acima | dentro de cada rollup |

O ambiente registrado **resolve** `host_class` (§13.4) e informa o record
`environment` da campanha; ele não o substitui — o record continua carregando o
que foi observado de fato (versão de kernel, RTT medido etc.), não só o que
estava cadastrado.

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
- **ambiente registrado** — `environment`: o `id` de §4.4 (ex.: `desktop-windows`,
  `linux-remoto`) — onde o caso rodou, não confundir com o `env` abaixo;
- **ambiente resumido** (`env`) — `host_id` anonimizado, `host_class`, SO e
  versão, arquitetura, modelo de CPU, núcleos físicos/lógicos, RAM, filesystem,
  classe do dispositivo, tipo de build, compilador e versão, sanitizers, page
  size, versões de formato e protocolo — resolvido a partir do ambiente
  registrado e do que foi observado na execução;
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

Um rollup sem `commit`, `series_key`, `environment`, `host_class`, tipo de
build, `seed` ou `status` é **rejeitado** pelo indexador com erro. Ponto
histórico sem procedência é ruído que envenena a série anos depois; recusar na
entrada é mais barato que limpar depois.

Nomes de campo canônicos — o dashboard (§13.11) lê exatamente estes:

```json
{"schema":"modb.loadtest.rollup","schema_version":1,
 "series_key":"a1b2c3d4e5f60718","series_key_version":1,
 "case_id":"load.create_only.embedded.100k",
 "workload":"create_only","target":"embedded","scale":"100k","objects":100000,"variant":"",
 "environment":"linux-remoto",
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
   propósito, e a descontinuidade fica registrada em `run_note`. Desde §4.4,
   `host_class` é resolvido a partir do ambiente registrado, não digitado à
   mão; **o `environment` (id) em si não entra no hash** — dois ambientes
   cadastrados com o mesmo `host_class` (hardware equivalente) permanecem na
   mesma série de propósito, e é isso que se quer.

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

**Implementado na Subfase J.** `modb_load gate --case ID --metric NOME
[--phase NOME] [--history-file PATH]` reaproveita `compute_trend`
(Subfase C) sem duplicar a mediana móvel: o gate por execução É o veredito
do último ponto (`compute_trend` já compara candidato × mediana das até 5
anteriores da mesma série); `status != "completed"` no último ponto vira
`fail` direto, nunca uma comparação de limiar (§9: divergência de correção
é falha imediata). A deriva lenta é um cálculo novo
(`loadtests/history/gate.cpp`) sobre a mesma janela de valores comparáveis:
mediana das últimas 5 (incluindo o candidato) × mediana das até 5 que
terminam 20 execuções atrás, limiar único de 15%, sem tier de alerta.
Exit code: 0 quando `passed` (inclui `insufficient`/`insufficient` -- não
bloqueia CI por falta de histórico), 1 quando o gate pontual OU a deriva
reprovam. Verificado com dados sintéticos: queda pontual de 12%
(`ops_per_second`, limiar 10%) reprova o gate por execução; 23 execuções
caindo 1 unidade cada (~20% acumulado) reprovam a deriva mesmo com todo
gate pontual isolado passando (cada passo fica bem abaixo do limiar de
alerta de 5%).

### 13.8 Retenção

Brutos: manter os N últimos por série (padrão 10), todos os marcados como
baseline, todos com `status=failed` e todos com `run_note` de interferência.
Acima de 30 dias, comprimir. Remoção só por `--prune --confirm`, e o rollup do
bruto removido permanece — com o hash, para que a ausência seja detectável.

Rollups: nunca apagados, nunca reescritos. São a memória do projeto.

**Implementado na Subfase J (parcial).** `modb_load prune [--keep N]
[--confirm] [--history-file PATH] [--baselines-file PATH] [--raw-dir DIR]`
agrupa os rollups de `series.jsonl` por `series_key`, mantém os `--keep`
(padrão 10) mais recentes por `started_at`, e entre os mais antigos só
remove o `raw_file` (nunca a linha de rollup) de quem não é `status=failed`
nem tem `run_id` marcado em `baselines.json`. Sem `--confirm`, só lista o
que seria removido. **Não implementado**: compressão acima de 30 dias, e a
proteção por `run_note` de interferência -- `run_note` em si nunca foi
emitido por nenhuma campanha real ainda (nenhuma subfase implementou esse
record type), então a regra fica sem efeito prático até que exista.

### 13.9 Baselines marcadas

`load-history/baselines.json` mapeia `series_key` → `run_id` escolhido
explicitamente, com data e motivo em texto. Uma baseline é uma decisão humana
registrada, não "a execução mais antiga" nem "a melhor". Imutável: substituir uma
baseline é acrescentar entrada nova com o motivo da troca.

**Implementado na Subfase J.** `modb_load baseline --case ID --run-id ID
--reason TEXTO [--history-file PATH] [--baselines-file PATH]` resolve o
`series_key` procurando o par (`case_id`,`run_id`) em `series.jsonl` e
acrescenta uma entrada (nunca reescreve as anteriores) em
`load-history/baselines.json`.

### 13.10 Anonimização

`host_id` é hash do hostname com salt local configurado (`MODB_LOAD_HOST_SALT`),
nunca o hostname bruto. Nenhum usuário, token, endereço IP de cliente ou caminho
real entra no rollup; caminhos são normalizados. O campo `environment` grava só
o `id` cadastrado (`linux-remoto`), nunca `connection.host`/`connection.default_user`
de `loadtests/environments.json` — essa distinção existe justamente para que o
rollup não precise carregar detalhe de conexão nenhum. A série histórica é
versionada no Git — o que entra nela é público para todo mundo que tem o
repositório.

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
| gráfico de escala com referência de custo por objeto constante | §4.1: 10k → 1M é linear ou superlinear? — restrito ao mesmo workload, alvo e ambiente do caso selecionado, senão mistura hardware |
| composição por fase | §4.2: cada fase medida separadamente |
| filtro "Ambiente" e coluna correspondente na tabela | §4.4: separa execuções por ambiente registrado sem misturar hardware distinto em nenhum gráfico |
| tabela com Δ anterior, Δ mediana e exportação CSV | relevo da paleta e §13.6: nenhum valor existe só no gráfico |

Regras de leitura embutidas, iguais às da CLI: menos de 3 pontos não recebe
veredito, pontos `comparable=false` aparecem mas ficam fora do cálculo, falhas
aparecem em vermelho em vez de sumir, e "hoje" é o ponto mais recente da série —
não o relógio da máquina, para que histórico antigo continue legível.

## 14. Artefatos a implementar

```text
loadtests/
  environments.json               catálogo de ambientes registrados (implementado, §4.4)
  json_value.hpp/.cpp             implementado -- parser JSON mínimo, só leitura (não
                                 existia nada além de serialização em benchmarks/runner)
  modb_load.cpp                  implementado -- CLI: run, list-cases, list-profiles,
                                 index, trend, report (forma mínima); resume/gate/
                                 compare reportam "ainda não implementado" em vez de
                                 fingir que fazem algo; `run` indexa por padrão (--no-index desliga)
  matrix.hpp/.cpp                implementado -- dimensões, expansão, seletores, ids
  environments.hpp/.cpp          implementado -- carrega/valida environments.json, resolve --environment
  profiles.hpp/.cpp              implementado -- os 8 perfis de §6.2 (nem todo workload
                                 neles tem dispatch ainda -- ver is_workload_implemented)
  budget.hpp/.cpp                implementado -- gate --accept-unknown-budget; sem
                                 tabela de calibração ainda (Subfase H)
  campaign.hpp/.cpp              implementado -- resolve_cases, render_case_plan,
                                 run_campaign (os records de §12 usados até a Subfase B)
  dataset_user.hpp/.cpp          implementado -- gerador user_v1, função pura de (seed,index)
  target.hpp                     implementado -- structs comuns (PhaseMetrics, WorkloadParams, CaseRunResult)
  target_embedded.hpp/.cpp       implementado -- contra modb::object::Database de verdade
  target_client.cpp              implementação via net::Client (loopback/remoto)
  history/
    rollup.hpp/.cpp              implementado -- extração campanha -> rollup
    series_key.hpp/.cpp          implementado -- hash de comparabilidade (environment
                                 não entra; host_class entra)
    index.hpp/.cpp               implementado -- append idempotente, recusa rollup
                                 sem procedência
    trend.hpp/.cpp               implementado -- 11 métricas (mesmo registro do
                                 dashboard), mediana móvel, quebra de série, veredito
    gate.cpp                     regressão pontual e deriva lenta (Subfase J)
  dashboard/
    index.html                   painel da série histórica (implementado, §13.11)
  workloads/
    create_only.hpp/.cpp            implementado
    create_delete_forward.hpp/.cpp  implementado
    create_delete_reverse.hpp/.cpp  implementado
    create_delete_interleaved.hpp/.cpp implementado -- stride=7, mesmo padrão de
                                       benchmarks/scenarios/object_store_lifecycle.cpp
    crud_full.hpp/.cpp              implementado -- 6 fases, amostra determinística
                                   por update (§9 item 4), não o conjunto inteiro
    read_hotspot.cpp                comportamento (§4.2.1)
    range_scan_sweep.cpp            comportamento; formaliza o antigo crud_query
    mixed_oltp.cpp                  comportamento; concorrência como workload, não só dimensão
    snapshot_hold.cpp               comportamento
    blob_lifecycle.cpp              comportamento
    cascade_delete.cpp              comportamento; formaliza o antigo crud_relationships
    oversubscribed_churn.cpp        comportamento
    restart_recovery.cpp            comportamento; precisa do harness de kill/restart (§17)
  dataset_user_blob.hpp/.cpp        variante do dataset com blob, só para blob_lifecycle
  calibration/
    windows-x86_64.json
    linux-x86_64.json
  config/
    load-local.yaml                config de exemplo para run-load.* (implementado, §6.5)
    load-smoke.yaml                 só create_only + accept_unknown_budget:true --
                                   roda de ponta a ponta hoje (implementado)
scripts/
  run-remote-benchmark.ps1        já consome loadtests/environments.json (implementado)
  run-load.ps1 / run-load.sh      lêem YAML e chamam `modb_load run` localmente (implementado, §6.5)
  run-remote-load.ps1             assume o mesmo -Environment ID do script acima
tests/
  load_matrix_test.cpp           implementado (`ctest -R modb.load_matrix`) -- expansão,
                                 seletores, ids, conjunto vazio
  load_workload_test.cpp         implementado (`ctest -R modb.load_workload`) -- cada
                                 workload em escala minúscula, invariantes
  load_history_test.cpp          implementado (`ctest -R modb.load_history`) --
                                 series_key estável, idempotência do index, rejeição
                                 de rollup sem procedência, mediana móvel/veredito,
                                 quebra de série reseta a janela
load-history/                    versionado no Git
  series.jsonl                   append-only, um rollup por (caso, execução)
  baselines.json                 series_key -> run_id escolhido, com motivo
load-results/                    ignorado pelo Git
```

Reuso direto, sem cópia: `benchmarks/runner/jsonl_writer`, `environment`,
`sha256` e `json_util`. **Implementado como opção (a)**: `modb_load_core`
(CMakeLists.txt) linka `modb_bench_core` `PUBLIC` em vez de duplicar os quatro
arquivos — herda o include dir `benchmarks/` de graça, então
`#include "runner/jsonl_writer.hpp"` funciona em `loadtests/*.cpp` sem
configuração adicional. A extração para um `bench_runner_core` próprio
continua possível depois, mas não foi necessária para A/B.

A chave do desenho é `target.hpp`: uma interface de CRUD sobre `User` com duas
implementações. Workload e matriz não sabem se estão embedded ou em rede — é o
que permite que o mesmo caso rode nos quatro alvos.

## 15. Ordem de implementação

Uma subfase por branch, conforme a convenção do projeto.

> **Estado atual e sequência revisada**:
> [docs-process/PLANO_IMPLEMENTACAO_CARGA.md](../docs-process/PLANO_IMPLEMENTACAO_CARGA.md)
> levanta o que está de fato implementado (lendo o código, não esta doc),
> agrupa o resto em ondas dirigidas a dependência e rastreia o progresso
> subfase por subfase. Ele registra duas divergências desta tabela, ambas
> justificadas ali: duas dívidas do já implementado (`case_id` que mente sobre
> concorrência; métricas que o dashboard pressupõe e o coletor não produz)
> entram antes de qualquer subfase nova, e a Subfase H (escalas altas) é
> adiantada em relação a G/I (rede), porque a pergunta "10k a 1M" é
> respondível só com `embedded`.

| subfase | entrega | critério de pronto |
|---|---|---|
| A | **Implementado.** matriz, ids, seletores, todos os 8 perfis de §6.2, `list-cases`, `--dry-run`, `budget` sem calibração (gate `--accept-unknown-budget`), `environments.hpp/.cpp` (valida `--environment` contra `loadtests/environments.json`), `json_value.hpp/.cpp` (parser JSON mínimo, não existia nada além de serialização) | `load_matrix_test` verde (13 casos); `list-cases`/`run --dry-run` imprimem sem executar; `--environment` inválido falha com a lista de ids cadastrados; `run-load.ps1`/`run-load.sh` param de imprimir "não encontrado" e chamam o binário de verdade |
| B | **Implementado.** `campaign.cpp` (writer + os 8 records de §12 usados até aqui), `dataset_user` (gerador puro por (seed,index), splitmix64), `target_embedded.cpp` (contra `modb::object::Database` de verdade — create em lotes, bind de `User`, commit), workload `create_only`, escalas `1k`/`10k`/`100k`/`1M` (todas as escalas do catálogo, não só 1k/10k) | `modb_load run --profile load-smoke --workload create_only --accept-unknown-budget` gera JSONL válido com `hash_match:true` (create + releitura completa comparados por SHA-256); perfis com workload não implementado reportam `case_error` claro por caso e `status:"partial"`, nunca travam a campanha |
| C | `series_key`, rollup, `index` idempotente, `trend`, `report` | duas execuções de `load-smoke` produzem dois pontos na mesma série; reindexar não duplica; o dashboard (§13.11) abre o `series.jsonl` gerado sem conversão |
| D | `create_delete_forward`, `create_delete_reverse`, `create_delete_interleaved` | invariantes de contagem zero e espaço recuperado registrados |
| E | `crud_full` com as seis fases separadas | leitura campo a campo confere; `phase_summary` por fase |
| F | `progress_window`, inclinação no rollup, `case_summary`, `resume` | interrupção em 100k retomável sem reexecutar caso concluído |
| G | `target_client`, alvo `loopback`, métricas de rede | `load-local` cobre embedded e loopback com o mesmo caso |
| H | escalas `250k`/`500k`/`1M`, calibração medida, guarda-corpos ativos | tabela §10 preenchida; caso acima do orçamento pulado com registro |
| I | `remote_colocated`, `remote_client_local`, indexação dos brutos trazidos (`run-remote-benchmark.ps1` já resolve `-Environment` pelo catálogo, §4.4) | `load-remote` traz exatamente um arquivo do host, com hash, e ele entra na série |
| J | `gate`, deriva lenta, retenção e `--prune`, baselines marcadas | regressão sintética de 12% e deriva sintética de 15% são detectadas |
| K | dimensões secundárias, `load-heavy` pairwise, `load-soak` | cada valor não padrão exercitado ao menos uma vez |
| L | `read_hotspot`, `range_scan_sweep` (índice sobre `dataset_user`) | hit rate e plano (índice/scan) registrados; contagem por seletividade confere |
| M | `mixed_oltp` sobre sessões concorrentes (reusa `--concurrency`, §4.5) | contagem final reconcilia sob concorrência real; checksum de amostra confere |
| N | `snapshot_hold` | leitura pela snapshot aberta permanece idêntica durante todo o churn; versões/bytes retidos registrados |
| O | `dataset_user_blob`, `blob_lifecycle` | hash byte a byte do blob confere; espaço recuperado após delete |
| P | `cascade_delete` sobre hierarquias `Ref`/`OwnedRef` | zero refs órfãs em profundidade/largura configuráveis |
| Q | `oversubscribed_churn`, perfil `load-behavior` | mesmas invariantes de `create_delete_interleaved` mais razão de eviction registrada; perfil executável de ponta a ponta |
| R | harness de kill/restart (Windows e Linux), `restart_recovery` | hash pós-recuperação == hash do último commit durável; tempo de recuperação registrado nos dois SOs |

`schema_evolution` e `replica_catchup` (§4.2.1) não têm subfase própria: entram
na ordem só depois que a infraestrutura de que dependem (versionamento de
`Binding` simultâneo; réplica orquestrada pelo harness) existir por outro
motivo — não vale construir essa infraestrutura só para o teste de carga.

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
- cada combinação de D1 × D2 × D3 × D4 declarada nos perfis for executável
  isoladamente por seletores, sem editar código;
- registrar um novo ambiente (`kind=local` ou `kind=ssh`) em
  `loadtests/environments.json` e rodar/filtrar por ele exigir só editar esse
  arquivo — nunca o código de `modb_load` ou dos scripts;
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

Especificamente para os workloads adicionais (§4.2.1):

- `read_hotspot` registrar hit rate do cache e os valores lidos conferirem
  campo a campo com o esperado;
- `range_scan_sweep` retornar a contagem esperada em cada seletividade da
  varredura e registrar se o plano usado foi índice ou scan completo;
- `mixed_oltp` reconciliar a contagem final (criados − removidos) sob
  concorrência real, com o checksum de uma amostra determinística conferindo —
  nenhuma escrita perdida sob contenção;
- `snapshot_hold` produzir leituras idênticas pela snapshot aberta do início ao
  fim do churn, e registrar versões retidas, bytes retidos e a pausa de GC ao
  fechar;
- `blob_lifecycle` conferir hash byte a byte do blob lido contra o escrito em
  pelo menos um tamanho de 256 MiB, com espaço recuperado após o delete;
- `cascade_delete` não deixar nenhuma ref órfã após remover a raiz de uma
  hierarquia com profundidade e largura configuráveis, com total removido ==
  total criado;
- `oversubscribed_churn` degradar de forma mensurável (não travar nem corromper)
  quando o volume ultrapassa o cache configurado, com razão de eviction
  registrada;
- `restart_recovery` produzir hash pós-recuperação idêntico ao do último commit
  durável em pelo menos três pontos de kill (mid-transação, pós-commit,
  mid-checkpoint), em Windows e em Linux;
- `schema_evolution` e `replica_catchup` permanecerem documentados como
  dependentes de infraestrutura ainda não construída, sem entrar em nenhum
  perfil até que essa infraestrutura exista — critério de aceite é a
  dependência ficar explícita, não a implementação em si.

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
   trabalho com carga concorrente são ruidosos. `host_class` separa as séries,
   mas antes de §4.4 era fácil esquecer de configurá-lo à mão e contaminar a
   série oficial. **Mitigado pelo ambiente registrado (§4.4)**: `host_class`
   agora é resolvido do catálogo, não digitado por execução; o risco residual é
   escolher `--environment` errado (ex.: `linux-remoto` numa sessão que na
   verdade rodou no desktop) — sem detecção automática disso, é erro do
   operador, não do sistema.
10. **Rollup versionado gera conflito de merge** — arquivo append-only tocado por
    vários branches conflita. Mitigação: uma linha por ponto, ordenação por
    `started_at` na leitura e não no arquivo, e resolução de conflito por união
    das linhas — o `index` detecta duplicata por `run_id` de qualquer forma.
11. **Deriva de ambiente confundida com regressão** — atualização de SO,
    firmware ou driver de disco no mesmo ambiente registrado muda a linha de
    base sem mudar `series_key` (o `host_class` cadastrado não muda sozinho).
    Mitigação: `run_note` obrigatório quando o record `environment` (§11, o
    hardware/SO efetivo da execução — não confundir com o campo `environment`
    do rollup) diverge do ponto anterior da série em campo relevante, e o
    relatório marca o ponto.
12. **Registro versus realidade** — nada impede de rodar fisicamente numa
    máquina diferente da apontada por `--environment` (ex.: SSH para um host
    que não é o cadastrado). O catálogo declara intenção, não verifica
    identidade de hardware; a verificação de fato vem do record `environment`
    observado (§11) divergir do esperado para aquele `host_class` — que cai no
    risco 11.
13. **Kill/restart é específico por plataforma** — `restart_recovery` (§4.2.1)
    precisa matar o processo em pontos definidos (mid-transação, pós-commit,
    mid-checkpoint) de um jeito que não mascare o que está sendo medido: em
    Linux, `SIGKILL`; em Windows, não há equivalente direto (`TerminateProcess`
    não é a mesma semântica de falha abrupta). O harness da Subfase R precisa
    de um mecanismo por SO, documentado, e os dois têm que produzir o mesmo
    critério de aceite — hash pós-recuperação idêntico.
14. **Invariante sob concorrência é mais caro de verificar** — `mixed_oltp`
    mistura create/read/update/delete concorrentes; "nenhuma escrita perdida"
    não dá para verificar objeto a objeto sem serializar (o que anula o próprio
    propósito do workload). Mitigação: checksum de uma amostra determinística
    de ids, não do conjunto inteiro — mais barato, mas é verificação por
    amostragem, não exaustiva; documentar isso explicitamente no resultado.
15. **`blob_lifecycle` exige dataset próprio** — os workloads da escada básica
    usam `dataset_user` (§7) sem blob. `dataset_user_blob` (Subfase O) é uma
    variante nova, não uma extensão do existente, porque blobs de 256 MiB no
    dataset padrão inflariam todos os outros workloads que não precisam deles.
16. **`snapshot_hold` pode mascarar vazamento de memória como retenção
    esperada** — versões retidas enquanto uma snapshot está aberta são
    corretas por design (MVCC); a Subfase N precisa distinguir isso de um
    vazamento real comparando o crescimento **depois** do `close_snapshot`
    contra o esperado, não só durante o churn.
