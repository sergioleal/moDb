# Plano completo de benchmarks do moDb

- Estado: especificação em implementação incremental (Fase 10A: runner +
  cenário `object_store.lifecycle` + perfis smoke/standard/diagnostic)
- Versão: 1
- Data: 2026-07-17
- Responsável pela implementação: Fase 10, com instrumentação adicionada nas
  fases anteriores quando o componente correspondente existir

## 1. Objetivo

Este plano define como medir todas as camadas do moDb, registrar os resultados
de forma reproduzível e construir uma série histórica útil para investigar
regressões, avaliar otimizações e comparar plataformas.

Cada campanha produz **um único arquivo autocontido**, com data e hora no nome.
O arquivo guarda metadados do ambiente, configuração, amostras brutas, métricas
agregadas, falhas e resumo final. Não são necessários CSVs, logs ou sidecars
para interpretar uma execução.

Benchmark não substitui teste de correção. Cada cenário valida seu resultado
fora da região medida e marca a amostra como inválida se a verificação falhar.

## 2. Princípios

1. Medir antes de otimizar.
2. Separar tempo medido de preparação, geração de dados e validação.
3. Manter ids de cenários e nomes de métricas estáveis ao longo do tempo.
4. Registrar amostras brutas; percentis isolados não bastam para análise futura.
5. Nunca apagar outliers silenciosamente.
6. Declarar explicitamente cache frio, cache quente e estado misto.
7. Usar dados determinísticos e registrar a seed.
8. Comparar apenas campanhas com ambientes e semânticas compatíveis.
9. Tratar latência, throughput, memória, CPU, I/O, espaço e correção como
   dimensões independentes.
10. Evitar o produto cartesiano completo: executar combinações dirigidas por
    risco, perfis predefinidos e matrizes pairwise; cenários críticos recebem
    varreduras completas.

## 3. Artefatos a implementar

```text
benchmarks/
  runner/                         infraestrutura comum
  primitive_bench.cpp            endian, CRC, codec e compressão
  io_bench.cpp                   NativeFile e PageFile
  storage_bench.cpp              SlottedPage, TableHeap e cache
  object_bench.cpp               modelo OO e materialização
  collection_bench.cpp           blobs, coleções e relacionamentos
  transaction_bench.cpp          WAL, commit, rollback e recovery
  snapshot_bench.cpp             MVCC, retenção e GC
  query_bench.cpp                índices, planner e streaming
  protocol_bench.cpp             frames, compressão e rede
  operation_bench.cpp            runtime de métodos C++
  workload_bench.cpp             cenários de ponta a ponta
  datasets/                      geradores determinísticos
  profiles/                      smoke, standard, exhaustive e soak
scripts/
  run-benchmarks.ps1
  run-benchmarks.sh
  compare-benchmarks.*           leitor; nunca altera arquivos históricos
benchmark-results/               ignorado pelo Git; arquivos coletados
```

Um executável coordenador, `modb_bench`, registra os casos e executa uma
campanha:

```text
modb_bench run --profile standard --output-dir benchmark-results
modb_bench run --profile smoke --filter storage.*
modb_bench run --profile exhaustive --seed 123456
modb_bench compare arquivo-antigo.jsonl arquivo-novo.jsonl
```

Os executáveis especializados podem existir durante a migração, mas o runner é
o único responsável pelo arquivo final e pela coleta uniforme de metadados.

## 4. Arquivo único por campanha

### 4.1 Nome

Usar UTC, precisão de milissegundos e caracteres portáveis:

```text
modb-benchmark-YYYYMMDDTHHMMSS.mmmZ-<commit>-<host>.jsonl
```

Exemplo:

```text
modb-benchmark-20260717T184233.482Z-a1b2c3d-build01.jsonl
```

Se não houver Git, usar `nogit`; se o host não puder ser identificado, usar
`unknown-host`. Nome já existente recebe um sufixo monotônico `-01`, `-02`, sem
sobrescrever coleta anterior.

Durante a execução, gravar com extensão `.partial`. Cada linha deve ser
sincronizada ao menos ao terminar um cenário. No encerramento normal, registrar
`run_end`, sincronizar e renomear atomicamente para `.jsonl`. Um `.partial`
continua legível e deve conter todas as amostras concluídas antes do crash.

### 4.2 Formato

O formato é JSON Lines UTF-8: um objeto JSON completo por linha. Isso permite
acrescentar resultados sem manter o arquivo inteiro em memória e recuperar
campanhas interrompidas. Todos os registros possuem:

```json
{"schema":"modb.benchmark","schema_version":1,"record":"...","run_id":"...","sequence":1}
```

Tipos de registro:

| `record` | Conteúdo |
|---|---|
| `run_start` | identidade, instante, comando, perfil e seed |
| `environment` | hardware, SO, compilador, build, Git e limites |
| `scenario_start` | id, parâmetros, dataset e política de cache |
| `sample` | uma repetição bruta com métricas e validade |
| `scenario_summary` | estatísticas calculadas sem descartar amostras |
| `scenario_error` | erro de preparação, execução ou validação |
| `run_note` | interferência ou observação detectada durante a campanha |
| `run_end` | duração, contagens, status e hash do conteúdo anterior |

Números inteiros maiores que `2^53-1` que possam ser lidos por JavaScript são
gravados também como string decimal ou em campo explicitamente tipado. Tempos
brutos são inteiros em nanossegundos; taxas derivadas podem ser `double`.
Valores ausentes são `null`, nunca zero inventado. Unidades fazem parte do nome
da métrica (`latency_ns`, `bytes`, `objects_per_second`).

### 4.3 Exemplo mínimo

```json
{"schema":"modb.benchmark","schema_version":1,"record":"run_start","run_id":"019f...","sequence":1,"started_at":"2026-07-17T18:42:33.482Z","profile":"standard","seed":"123456"}
{"schema":"modb.benchmark","schema_version":1,"record":"scenario_start","run_id":"019f...","sequence":3,"scenario_id":"storage.slotted_page.insert","parameters":{"payload_bytes":128,"occupancy_percent":75},"cache_state":"warm"}
{"schema":"modb.benchmark","schema_version":1,"record":"sample","run_id":"019f...","sequence":4,"scenario_id":"storage.slotted_page.insert","iteration":1,"valid":true,"metrics":{"operations":100000,"elapsed_ns":42100000,"cpu_ns":41000000,"allocations":0}}
{"schema":"modb.benchmark","schema_version":1,"record":"scenario_summary","run_id":"019f...","sequence":14,"scenario_id":"storage.slotted_page.insert","valid_samples":10,"latency_ns":{"min":410,"p50":419,"p90":431,"p95":438,"p99":452,"max":470},"operations_per_second":2380952.38}
```

### 4.4 Metadados obrigatórios

O registro `environment` contém, quando disponível:

- commit Git completo, branch, árvore limpa/suja e hash do diff quando suja;
- versão do formato de arquivo, protocolo e schema do relatório;
- compilador, versão, standard C++, gerador, preset e flags de compilação/link;
- tipo de build, sanitizers, LTO, RTTI, exceptions e defines como page size;
- SO, versão, kernel, arquitetura e ambiente nativo/VM/container/WSL;
- CPU, sockets, núcleos físicos/lógicos, frequência nominal, governor/plano de
  energia, afinidade e instruções relevantes;
- RAM total/disponível, page size do SO, NUMA e limites do processo;
- dispositivo de armazenamento, filesystem, espaço livre e política de sync;
- hostname anonimizado ou configurado, timezone e relógio usado;
- timer monotônico, resolução observada e overhead medido do timer;
- argumentos completos, variáveis `MODB_BENCH_*`, perfil e seed;
- processos/interferências detectáveis, temperatura e throttling quando
  disponíveis;
- versões e configurações de bibliotecas de compressão;
- endereço do servidor, tipo de enlace e latência-base quando houver rede;
- hash/versão dos módulos de domínio carregados.

Segredos, tokens, senhas, nomes de usuário e conteúdo real dos objetos nunca são
registrados. Caminhos podem ser normalizados ou anonimizados.

## 5. Métricas comuns

Todo cenário registra somente métricas que consegue medir corretamente, mas usa
os mesmos nomes e unidades.

### 5.1 Tempo e taxa

- latência bruta por operação ou lote em ns;
- min, mediana/p50, p90, p95, p99, p99.9 e máximo;
- média, desvio padrão, MAD e coeficiente de variação;
- operações, objetos, registros e bytes por segundo;
- TTFR, tempo até último resultado e duração total;
- tempo de preparação, aquecimento, região medida e validação separados;
- latência de fila, CPU e I/O quando distinguíveis.

### 5.2 Recursos

- CPU de usuário/sistema, ciclos, instruções, IPC, context switches e faults;
- alocações, frees, bytes alocados e pico de alocação;
- RSS atual/pico, working set, memória privada e memória do cache;
- leituras/escritas físicas e lógicas, bytes, operações, sync/fsync e latência;
- páginas lidas, escritas, alocadas, reutilizadas, sujas e evictadas;
- hits, misses e taxa de acerto dos caches;
- tamanho do banco, WAL e arquivos temporários;
- bytes enviados/recebidos, frames, syscalls e retransmissões quando disponíveis;
- tamanho lógico, tamanho codificado e razão de compressão;
- tempo de compressão/descompressão e ciclos por byte.

### 5.3 Qualidade e amplificação

- write, read e space amplification;
- bytes persistidos por byte lógico e por objeto;
- fragmentação interna/externa e ocupação média de página;
- objetos examinados por objeto retornado;
- versões criadas, retidas e coletadas por MVCC;
- erros, retries, cancelamentos e timeouts;
- checksum/hash do resultado lógico para comprovar equivalência entre variantes.

## 6. Metodologia

### 6.1 Preparação

- Usar build Release ou RelWithDebInfo para comparações oficiais.
- Builds Debug/sanitized são diagnósticos e recebem classe própria; nunca são
  comparados automaticamente com Release.
- Fixar afinidade quando suportado e registrar qualquer falha ao fixá-la.
- Usar máquina ociosa; registrar interferência, throttling ou mudança de energia.
- Reservar espaço suficiente e registrar o dispositivo efetivamente utilizado.
- Gerar dataset fora da região medida e validar seu hash.

### 6.2 Aquecimento e repetições

Padrão por caso, substituível pelo perfil:

- calibrar o número de operações para que uma amostra dure ao menos 250 ms;
- 3 repetições de aquecimento não registradas como resultado comparável, mas
  descritas no `scenario_summary`;
- no mínimo 10 amostras válidas e 5 segundos medidos;
- cenários de latência de cauda: no mínimo 30 amostras e 100 mil operações;
- embaralhar variantes com seed registrada para reduzir viés temporal;
- parar apenas quando mínimos forem atendidos e a duração máxima do perfil não
  tiver sido excedida.

Nenhuma amostra é removida. Interferência conhecida gera `run_note` e pode
marcar a amostra `comparable=false`, preservando seus valores.

### 6.3 Cache frio e quente

Cada cenário declara uma política:

- `warm`: dataset e estruturas aquecidos antes da medição;
- `process_cold`: novo processo, sem estado interno anterior;
- `database_reopen`: fecha e reabre o banco, preservando cache do SO;
- `os_cold`: cache do SO descartado por mecanismo documentado e autorizado;
- `oversubscribed`: working set maior que cache/RAM para medir pressão real;
- `mixed`: distribuição temporal explicitamente descrita.

Nunca chamar um teste de “cold” apenas porque o banco foi reaberto. Limpar cache
do SO pode exigir privilégio; se não for possível, registrar `unsupported` em
vez de simular.

### 6.4 Relógio e concorrência

Usar relógio monotônico. Medir seu overhead no início. Latência concorrente é
medida no cliente da operação, com histogramas por thread e agregado; não usar
apenas `tempo_total / operações` como latência.

Threads iniciam por barreira. Registrar afinidade, distribuição de trabalho,
tempo de ramp-up/ramp-down e fairness por thread/conexão.

### 6.5 Validação

Depois da região medida:

- verificar contagens, hashes e invariantes do cenário;
- reabrir o banco quando durabilidade fizer parte do caso;
- executar `database_check` nos cenários de mutação longa;
- marcar toda amostra incorreta como inválida e falhar a campanha conforme o
  perfil; nunca publicar taxa de uma operação logicamente incorreta.

## 7. Dimensões de variação

As dimensões seguintes formam a biblioteca comum de parâmetros:

| Dimensão | Valores iniciais |
|---|---|
| dataset | 1k, 10k, 100k, 1M, 10M objetos; maior que RAM quando aplicável |
| payload | 0, 16, 64, 128, 512 B; 4, 16, 64 KiB; blobs de 1, 16 e 256 MiB |
| forma | fixo, variável, bimodal, Zipf, muitos campos, nested/embedded |
| conteúdo | zeros, texto repetitivo, texto realista sintético, aleatório/incompressível |
| cache | warm, reopen, process cold, OS cold, 0.25×/1×/4×/10× capacidade |
| acesso | sequencial, reverso, uniforme aleatório, Zipf/hotspot, stride |
| mutação | insert, read, update in-place/larger/smaller, delete, churn misto |
| transação | 1, 10, 100, 1k e 10k operações por commit |
| leitura/escrita | 100/0, 95/5, 80/20, 50/50, 0/100 |
| concorrência | 1, 2, 4, 8, 16, 32 threads/conexões até 2× CPUs lógicas |
| seletividade | 0%, 0.01%, 0.1%, 1%, 10%, 50%, 100% |
| resultado | limit 1, 10, 100, 10k e stream completo |
| durabilidade | sem sync diagnóstico, sync/commit real, checkpoint/recovery |
| rede | in-process, loopback, LAN, latência/banda/perda controladas |
| frame | 1, 4, 16, 64, 256 objetos e limites por bytes, sem esperar encher |
| compressão | none e codecs suportados; limiar, compressível/incompressível |
| build | Release oficial; RelWithDebInfo; sanitized apenas diagnóstico |
| plataforma | Windows/Linux; x86-64/ARM64 quando disponíveis |

O perfil `standard` usa pairwise mais casos de risco. `exhaustive` expande
dimensões críticas. Resultados sempre carregam todos os parâmetros efetivos.

## 8. Matriz por camada

### 8.1 Primitivos, codec e CPU

| Grupo | Casos | Métricas principais |
|---|---|---|
| endian/binary | load/store u16/u32/u64, buffers alinhados/desalinhados | ns/op, GB/s, ciclos/byte |
| CRC/checksum | WAL records e buffers de 16 B a 16 MiB | GB/s, ciclos/byte |
| value codec | cada `AttributeType`, null e payload nested | encode/decode/s, bytes/s, alocações |
| object codec | campos 1/4/16/64, strings curtas/longas | objetos/s, ns/campo, bytes lógicos |
| projeção | tipo idêntico, add/remove/convert, plano frio/quente | objetos/s, alocações, cache hit |
| compressão | none/LZ4/Zstd suportados × tamanhos/conteúdos | ratio, GB/s, ns/frame, memória |

Separar construção do objeto, encoding, cópia e checksum para que uma melhora em
um estágio não esconda regressão em outro.

### 8.2 Arquivo, páginas e cache

| Grupo | Casos | Métricas principais |
|---|---|---|
| NativeFile | read/write sequencial e aleatório; 4 KiB–16 MiB | IOPS, MB/s, p99, syscalls |
| sync | arquivo pequeno/grande, uma/muitas escritas por sync | commit-like p50/p99, sync/s |
| PageFile | create/open/allocate/read/write/reopen | páginas/s, bytes, latência |
| PageCache | hit, miss, read-ahead, working set 0.25×–10× | hit rate, páginas/s, RSS |
| BufferPool futuro | pin/unpin, dirty eviction, flush, competição | p99, stalls, writeback |

I/O deve distinguir cache do processo, cache do SO e acesso físico. O resultado
registra filesystem e dispositivo; números de discos distintos não são
comparados automaticamente.

### 8.3 SlottedPage e TableHeap

| Grupo | Casos | Métricas principais |
|---|---|---|
| SlottedPage | insert/get/update/delete por ocupação e tamanho | ns/op, ocupação, moves |
| update | mesmo tamanho, cresce, encolhe, sem espaço | latência, bytes movidos |
| compactação | fragmentação 10–90%, eager/lazy | tempo, bytes recuperados |
| TableHeap | append, lookup, scan e erase | registros/s, páginas lidas |
| churn | sequencial, reverso, stride, aleatório, Zipf | throughput, fragmentação, crescimento |
| reuse | espaço livre 1–100%, payload variável | amplification, bytes/registro |

### 8.4 Modelo de objetos

- `TypeDefinition`, catálogo e baseline: create/find/open com 1–100 mil tipos.
- `IdentityMap`: bind/find/rebind/erase sequencial, aleatório e hotspot.
- `ObjectStore`: create/get/update/remove/scan e reopen.
- `Binding`: `to_field_values`, materialização e acesso por campo.
- `ProjectionPlan`: build frio, cache quente e migração registrada.
- `Handle::get` versus materialização completa.
- caminho atual com dupla decodificação versus variantes futuras.

Métricas: objetos/s, campos/s, ns/op, alocações, páginas/objeto, bytes/objeto,
RSS, tamanho final e amplificação.

### 8.5 Relacionamentos, blobs e coleções

- `Ref`, `OwnedRef`, `Embedded` e resolução de referências válidas/órfãs.
- remoção em cascata por profundidade, largura e número total de objetos.
- `BlobStore` create/read/stream/rewrite grow/shrink/remove por tamanhos.
- `PersistentVector`: create, `push_back`, `at`, scan e crescimento.
- `PersistentSet`/`PersistentMap`: insert/find/update/remove por cardinalidade.
- coleções de `Ref<T>` e grafos realistas sintéticos.
- comparação obrigatória antes/depois de append incremental, cursor por página e
  gestão de espaço livre.

Registrar complexidade observada, bytes reescritos por mutação, páginas órfãs,
TTFR de leitura e pico de memória.

#### 8.5.1 Algoritmos de grafos (Fase 12)

- BFS e DFS em grafos estreitos/profundos, largos/rasos e por densidade.
- Caminho mínimo sem peso com alvos próximos, distantes e inalcançáveis.
- Detecção de ciclo e ordenação topológica em DAGs e grafos cíclicos.
- Componentes conexos por quantidade e distribuição de tamanhos.
- Adjacência outgoing direta, incoming indexada e
  `PersistentVector<Ref<T>>`.
- Cache cold/warm, snapshots concorrentes, refs órfãs e cancelamento/limites.

Registrar vértices/arestas por segundo, TTFR, p50/p99 de expansão, páginas
lidas, RSS e pico do conjunto de visitados. Dataset, seed, direção, política
de órfãs e inclusão de `OwnedRef` acompanham cada amostra.

### 8.6 Transações, WAL e recuperação

| Grupo | Casos | Métricas principais |
|---|---|---|
| begin/rollback | vazia e 1–10k páginas sujas | ns/op, memória liberada |
| commit | 1–10k operações/páginas | p50/p99, tx/s, WAL bytes |
| WAL | append begin/image/commit, sync, scan/CRC | MB/s, records/s, sync time |
| checkpoint | WAL pequeno/grande e interrupção | duração, stalls, I/O |
| recovery | commit único/múltiplos, parcial, WAL truncado | ready time, pages/s |
| idempotência | primeira e segunda recuperação | duração e hash idêntico |
| contention | um escritor e leitores, tentativas concorrentes | fairness, wait, errors |

Durabilidade oficial sempre inclui sync real. Uma variante sem sync pode isolar
CPU/codec, mas deve carregar `durability=disabled_diagnostic` e nunca ser usada
como número de commit durável.

### 8.7 Snapshots e MVCC

- custo de abrir/fechar snapshot;
- leitura na época corrente e anterior;
- update/remove com 0, 1, 10, 100 e 1k snapshots ativos;
- varredura longa enquanto commits ocorrem;
- retenção de versões, crescimento do arquivo e GC;
- leitor mais antigo bloqueando descarte;
- latência de commit e leitura sob GC;
- tempo para recuperar espaço depois do último snapshot antigo.

Registrar versões vivas, bytes retidos, GC pauses, throughput dos leitores,
throughput do escritor e consistência por hash do snapshot.

### 8.8 Índices, consultas e streaming embedded

- B+ tree: bulk load, insert, point lookup hit/miss, range, update e delete.
- altura, splits/merges, ocupação, páginas lidas e write amplification.
- scan completo versus índice por seletividade.
- filtros simples/compostos, projeção, `limit`, offset/cursor e ordenação.
- planner frio/quente e custo de criação do plano.
- TTFR para `limit 1/10/100`, throughput sustentado e tempo total.
- consumo lento, cancelamento e erro após N objetos.
- dataset 10× maior que cache e maior que RAM.

Memória do pipeline deve permanecer limitada independentemente da cardinalidade;
registrar pico de RSS e objetos produzidos menos consumidos.

### 8.9 Protocolo, compressão e rede

| Grupo | Casos | Métricas principais |
|---|---|---|
| frame codec | encode/decode/validate por slots e bytes | frames/s, GB/s, allocs |
| coalescência | 1–256 objetos já disponíveis | TTFR, syscalls, bytes/frame |
| compressão | codec × limiar × conteúdo × frame | ratio, CPU, GB/s, p99 |
| OpCall/Result | payload 0 B–16 MiB | round-trip, encode/decode |
| loopback | 1–32 conexões, streams curtos/longos | RTT, TTFR, throughput |
| LAN | banda/latência reais registradas | objetos/s, MB/s, CPU |
| rede limitada | atraso, banda, jitter e perda controlados | p99, backpressure, memória |
| cliente lento | diferentes taxas de consumo | backlog, RSS, cancel time |

Comparar `none` com cada codec usando exatamente o mesmo conteúdo lógico e hash.
Medir bytes antes/depois, tempo de compressão e descompressão separadamente. O
servidor nunca espera encher um frame apenas para melhorar a razão.

### 8.10 Runtime de módulos de domínio

- lookup no `OperationRegistry` e dispatch vazio;
- decode de argumentos e encode do resultado isolados;
- método `read_only` sob snapshot e `read_write` sob transação;
- chamada local versus RPC loopback para separar overhead da rede;
- `TransferFunds` feliz, saldo insuficiente e contenção por conta;
- métodos com retorno pequeno, grande e streaming;
- carga e validação de módulo por tamanho, quantidade de exports e hash;
- exceção capturada, rollback e restart/recovery após crash em processo filho.

Registrar overhead do runtime sobre a mesma função chamada diretamente, além de
latência total observada pelo cliente.

### 8.11 Ponta a ponta e workloads

Criar workloads versionados, determinísticos e reconhecíveis:

- `accounting`: contas, transferências e consultas de extrato;
- `orders`: clientes, pedidos, itens, estoque e cancelamento em cascata;
- `social_graph`: pessoas, relações e travessias;
- `document_catalog`: objetos variáveis, blobs e busca indexada;
- `time_series_metadata`: muitos objetos pequenos e scans por intervalo;
- `schema_evolution`: dados v1 lidos/escritos por bindings v2/v3;
- `mixed_oltp`: mistura configurada de leitura, criação, atualização e remoção;
- `streaming_export`: snapshot grande até cliente rápido/lento;
- `restart_recovery`: carga, kill em pontos definidos, restart e retomada;
- `database_check`: banco saudável e bancos sintéticos de diferentes tamanhos.

Cada workload define versão, distribuição, invariantes, seed e hash esperado.
Mudança semântica cria nova versão do workload, nunca reutiliza o mesmo id.

### 8.12 Soak, crescimento e estabilidade

- 1 h, 8 h e 24 h de churn com métricas por janela;
- crescimento até 10× cache e além da RAM;
- reopen/checkpoint periódicos;
- snapshots longos e clientes lentos;
- alternância de módulos e workloads sem reiniciar quando suportado;
- monitorar vazamento, fragmentação, degradação por janela, handles e threads;
- validar banco e hash lógico ao final.

O arquivo único pode crescer; JSONL deve ser escrito em streaming. Para soak,
amostras de alta frequência podem ser agregadas por janelas fixas, mas contadores
totais e histogramas completos permanecem registrados.

## 9. Datasets

Todos os datasets são sintéticos, determinísticos e gerados por versão:

```text
dataset_id, dataset_version, seed, generator_commit, parameters, logical_hash
```

Distribuições obrigatórias: uniforme, sequencial, Zipf/hotspot, tamanhos fixos,
variáveis e bimodais. Textos realistas usam corpus sintético versionado, nunca
dados de produção. A geração fica fora da região medida, exceto em benchmarks
que explicitamente medem ingestão fim a fim.

Datasets grandes podem persistir entre cenários, mas o arquivo de resultado
registra seu hash, caminho lógico anonimizado, tamanho e se foi criado, clonado
ou reutilizado. Cenário destrutivo trabalha sobre cópia/reflink quando possível.

## 10. Perfis de campanha

| Perfil | Uso | Meta de duração |
|---|---|---|
| `smoke` | verificar runner e detectar regressão grosseira | 2–5 min |
| `standard` | coleta regular comparável por commit/release | 30–60 min |
| `exhaustive` | matriz ampla por release ou mudança estrutural | 4–12 h |
| `soak` | vazamento, crescimento e degradação temporal | 1–24 h |
| `diagnostic` | cenário/filter específico com instrumentação | sem meta |

O perfil faz parte do resultado. Uma campanha pode terminar `partial` se alguns
casos forem unsupported, mas falha de correção produz status `failed`.

## 11. Baselines e regressões

Uma baseline é um arquivo histórico imutável escolhido explicitamente. O
comparador não modifica os dois arquivos e produz sua análise em stdout ou em
novo arquivo solicitado pelo usuário.

Compatibilidade mínima para comparação automática:

- mesmo `scenario_id` e versão;
- mesmos parâmetros semânticos e dataset/hash;
- mesma classe de build e garantias de durabilidade;
- arquitetura, page size, formato e protocolo compatíveis;
- ambiente considerado comparável por regra configurada.

Limiares iniciais, ajustáveis por cenário:

- alerta: regressão de mediana ou throughput ≥ 5%;
- falha de gate: ≥ 10% com intervalos/variação que não indiquem ruído;
- latência de cauda: p99 ≥ 15%;
- memória, arquivo, WAL ou bytes de rede: aumento ≥ 10%;
- correção: qualquer divergência é falha imediata, independentemente de taxa.

O comparador apresenta diferença absoluta/percentual, dispersão, número de
amostras e compatibilidade. Não declarar vitória com uma única execução; para
decisões relevantes, usar ao menos três campanhas independentes por variante e
alternar a ordem A/B com seed registrada.

## 12. Profiling

Profiling é associado a um `run_id` e `scenario_id`, mas artefatos grandes de
profiler não precisam ser incorporados ao JSONL. Para manter o relatório
autocontido, registrar no arquivo:

- ferramenta e versão;
- comando e configuração;
- intervalo/cenário perfilado;
- top symbols/stacks agregados quando exportáveis em tamanho razoável;
- hash e localização opcional do artefato externo.

O resultado de benchmark continua válido sem o artefato de profiler. Nenhuma
otimização entra apenas com profile: deve haver comparação de benchmark antes e
depois registrada.

## 13. Automação e execução remota

Os scripts devem:

1. validar binário, perfil e espaço livre;
2. criar nome UTC automaticamente;
3. executar o runner sem misturar stdout humano com o JSONL;
4. preservar `.partial` em falha;
5. copiar de volta exatamente um arquivo por campanha remota;
6. imprimir caminho final, tamanho, `run_id`, status e hash SHA-256;
7. nunca sobrescrever resultado existente;
8. nunca armazenar senha ou token no relatório.

Saída humana vai para stderr/console. O writer estruturado é o único que escreve
o arquivo de resultados. Execuções concorrentes recebem nomes e `run_id`s
distintos.

## 14. Migração do benchmark atual

`benchmarks/object_throughput.cpp` torna-se inicialmente o cenário
`object_store.lifecycle`:

- preservar create/delete sequencial e por stride;
- separar create, flush, delete, flush e verificação;
- adicionar amostras repetidas, parâmetros e métricas de arquivo;
- emitir registros pelo runner, não texto ad hoc;
- manter saída humana opcional apenas como resumo;
- registrar compactação eager/lazy, page size, total, stride e dataset hash.

O script remoto atual passa a executar `modb_bench run`, baixar o único JSONL e
mostrar o caminho local. Endereço do servidor deixa de ser fixo no script e vira
parâmetro/configuração não secreta.

## 15. Ordem de implementação

1. Schema JSONL, writer crash-safe, nome timestampado e coleta de ambiente.
2. Runner, registro de casos, filtros, seeds, warmup, samples e summaries.
3. Migrar `object_throughput` e criar perfis `smoke`/`standard`.
4. Primitivos, I/O, páginas, heap e modelo de objetos.
5. Transações/WAL/recovery e coleções existentes.
6. Acrescentar MVCC, query, protocolo e módulos à medida que forem entregues.
7. Métricas de processo/SO portáveis e adaptadores específicos Windows/Linux.
8. Comparador histórico, gates e relatórios de regressão.
9. Perfis `exhaustive` e `soak`, automação local/remota.

Cada etapa deve produzir um arquivo válido no mesmo schema; campos novos são
aditivos dentro da versão. Mudança incompatível incrementa `schema_version`.

## 16. Critério de aceite

O plano estará implementado quando:

- `modb_bench run --profile standard` gerar exatamente um arquivo `.jsonl`
  final com timestamp UTC no nome;
- o arquivo contiver ambiente, configuração, amostras brutas, summaries, erros
  e `run_end`, sem depender de sidecar;
- uma interrupção deixar `.partial` legível até a última linha completa;
- todos os componentes existentes tiverem ao menos um cenário por operação
  crítica e por recurso principal;
- TTFR, throughput, p99, CPU, memória, I/O, tamanho e correção forem cobertos de
  ponta a ponta;
- cache frio/quente, tamanhos, payloads, concorrência, durabilidade, rede e
  compressão estiverem representados pelos perfis;
- o benchmark atual estiver migrado e comparável consigo mesmo;
- o comparador detectar regressões sintéticas conhecidas;
- três campanhas consecutivas no mesmo ambiente apresentarem variação dentro
  dos limites definidos para os cenários-gate;
- a documentação permitir repetir uma campanha em Windows e Linux sem decisões
  implícitas.
