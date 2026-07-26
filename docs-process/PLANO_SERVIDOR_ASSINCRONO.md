# Plano de servidor concorrente

- Data: 2026-07-25
- Branch de trabalho: `codex/async-server-architecture-plan`
- Objetivo: remover a serializacao artificial do servidor, preservando o
  contrato transacional simples do Ring0: um escritor por banco, multiplos
  leitores consistentes por snapshot e evolucao incremental para I/O assincrono
  quando os benchmarks mostrarem ganho.
- Decisao principal: comecar por concorrencia real entre conexoes e seguranca
  fisica do caminho de leitura. Coroutines e sockets async entram depois, como
  otimizacao de runtime, nao como pre-condicao para paralelismo.

## 0. Estado do corte atual

Implementado neste corte:

- `serve_forever()` aceita varias conexoes e cria uma thread por sessao.
- `serve_one()` permanece como caminho compativel para testes/demos de uma
  conexao.
- escritas remotas (`OpCall` read-write) podem chegar por conexoes diferentes;
  elas continuam serializadas pelo `engine_mutex_`, preservando o motor
  single-thread ate a Fase C/D.
- codec negociado passa a ser estado de sessao para o worker de query; o ultimo
  codec negociado continua exposto apenas como diagnostico.
- `last_stream_stats()` passa a retornar uma copia protegida por mutex.
- `modb.operation_server` cobre duas conexoes chamando `TransferFunds` em
  paralelo via `serve_forever()`.

Ainda nao implementado neste corte:

- fila dedicada de writer com metricas;
- reducao do `engine_mutex_`;
- seguranca fisica do `BufferPool`/`ScratchPagePool` para leitores paralelos
  sem lock global;
- cancelamento/fechamento ativo de sessoes longas durante `request_stop()`;
- coroutines ou socket async.

## 1. Diagnostico atual

A avaliacao do codigo aponta dois bloqueios principais antes de qualquer
runtime async:

1. `serve_forever()` chama `serve_one()` e atende uma conexao ate o peer fechar.
   Na pratica, o servidor atual nao aceita varias conexoes simultaneas.
2. `Server::engine_mutex_`, em `include/modb/net/server.hpp`, protege
   `run_query` e a execucao de `OpCall`. Isso serializa o avanco dos generators
   e foi mantido porque ja houve indicio de `STATUS_HEAP_CORRUPTION`.

Coroutines nao removem esses bloqueios por si so. Se o runtime async for criado
antes de atacar `serve_forever()` e `engine_mutex_`, o projeto pode ganhar
complexidade sem ganhar leitura paralela.

Tambem ha limites fisicos no caminho de leitura:

- `BufferPool` usa `std::list` e `std::unordered_map`, e `get()` altera LRU.
  Portanto, uma leitura aparentemente read-only ainda pode mutar estruturas
  internas compartilhadas.
- paginas sujas do writer e leituras compartilham o mesmo pool.
- `ScratchPagePool` e documentado como single-thread.
- o MVCC atual e de uma versao anterior por objeto, nao de retencao ilimitada.
  O risco operacional relevante e `snapshot_conflict` quando um segundo update
  do mesmo objeto precisa preservar uma versao anterior ainda visivel por uma
  snapshot longa.

O I/O assincrono por plataforma ja existe em `storage::AsyncFile`, mas ele e um
backend de arquivo. Ele nao e automaticamente um runtime de rede. Em especial,
POSIX AIO nao resolve sockets, e a implementacao Windows com IOCP por arquivo
nao deve ser tratada como loop de rede reutilizavel sem novo desenho.

## 2. Arquitetura alvo incremental

```text
accept loop
  |
  +-- session thread 1 -- SessionContext -- read-only snapshot
  |
  +-- session thread 2 -- SessionContext -- read-only snapshot
  |
  +-- session thread N -- SessionContext -- read/write command
                                      |
                                      v
                              writer queue unica
                                      |
                                      v
                         begin -> apply -> WAL -> commit
```

O primeiro corte deve ser uma thread por sessao, porque isso remove a
serializacao de conexoes com mudanca pequena e mensuravel. O servidor continua
podendo evoluir para coroutines depois, mas apenas quando houver uma razao
observada: alto custo de threads, muitas conexoes ociosas, backpressure
complexo ou ganho claro em A/B.

### Estado por sessao

Todo estado mutavel que hoje fica no `Server` e depende da conexao deve migrar
para `SessionContext`, incluindo:

- codec negociado ou `selected_codec_`;
- estatisticas da ultima operacao ou `last_stats_`;
- mapa de cancelamento por `query_id`;
- limites efetivos de stream e creditos da conexao.

O `Server` deve guardar apenas estado compartilhado e explicitamente protegido:
referencia ao banco, listener, configuracao global, registry de sessoes,
metricas agregadas e fila do writer.

## 3. Invariantes

- Um banco tem um unico escritor ativo por vez.
- O protocolo de commit continua WAL-antes-de-paginas.
- A ordem garantida e por conexao; nao ha promessa de ordem global entre
  conexoes diferentes.
- Toda leitura remota longa roda sob `Snapshot`.
- O snapshot registry ja e uma parte thread-safe do desenho; o plano nao deve
  refaze-lo.
- Leitura paralela so pode sair do `engine_mutex_` depois que `BufferPool`,
  scratch pages e estruturas tocadas por scan/get estiverem fisicamente seguras.
- Cancelamento e cooperativo; cancelar uma stream nao mata a conexao.
- Backpressure e explicito, com limite de creditos/bytes/frames pendentes; nao
  existe fila ilimitada.
- Se uma replica servir leitura enquanto aplica WAL, o apply da replica conta
  como writer local e obedece a mesma regra de escritor unico.

## 4. Decisoes de desenho

### 4.1 Multi-conexao antes de coroutine

O primeiro passo e alterar o accept loop para criar uma sessao por conexao. A
implementacao inicial pode ser thread por sessao, com join/stop ordenado no
shutdown. Isso da um baseline concreto para decidir se coroutines valem o custo.

### 4.2 Caminho de leitura fisicamente seguro

Antes de permitir `1 writer + N readers`, escolher e implementar uma estrategia
de isolamento/sincronizacao para leitura:

| Opcao | Uso esperado | Custo |
|---|---|---|
| `shared_mutex` no `BufferPool` e scratch por thread | menor alteracao correta | paralelismo limitado pela LRU |
| pool/cache read-only por sessao ou por worker | boa isolacao inicial | mais memoria e mais cache miss |
| latches por pagina/shard no `BufferPool` | melhor escala | maior complexidade |

Recomendacao: comecar por uma solucao conservadora e mensuravel, como
`shared_mutex` para a integridade do `BufferPool` mais `ScratchPagePool` por
sessao/thread. Depois, se a contencao aparecer nas metricas, evoluir para
sharding/latches.

### 4.3 Reducao do `engine_mutex_`

O `engine_mutex_` deve deixar de cobrir a execucao inteira de queries e
generators. A reducao e faseada:

1. manter o lock amplo enquanto multi-conexao e estado por sessao sao criados;
2. mover estado compartilhado acidental para `SessionContext`;
3. proteger explicitamente `BufferPool`, scratch e pontos mutaveis;
4. reduzir o lock para commit/escrita ou substitui-lo pela writer queue.

### 4.4 MVCC de uma versao anterior

O plano nao deve prometer GC como solucao para crescimento ilimitado de versoes,
porque o modelo atual guarda apenas a versao corrente e a anterior. O caso que
precisa ser tratado e conflito:

```text
T1 abre snapshot no epoch N
T2 atualiza objeto A e preserva previous para T1
T3 tenta atualizar A de novo antes de T1 fechar
=> snapshot_conflict ou retry, conforme politica escolhida
```

Obrigatorio medir `snapshot_conflict` por commit, idade da snapshot mais antiga
e tempo ate fechamento. Se o produto precisar de varias versoes anteriores por
objeto, isso deve virar uma ADR propria, pois muda o modelo MVCC.

### 4.5 Backpressure antes de escrita async

O backpressure atual e basicamente o bloqueio de `send_all`. Quando o envio
ficar async, e preciso um modelo explicito de credito por conexao/query antes
de produzir objetos indefinidamente. `max_in_flight_objects` pode virar parte
desse contrato, mas deve limitar bytes/frames ou ter conversao clara para eles.

### 4.6 Coroutines e socket async

Coroutines entram como uma fase posterior e opcional:

- se thread por conexao escalar mal;
- se houver muitas conexoes ociosas;
- se backpressure/cancelamento ficarem mais simples com `co_await`;
- se benchmark A/B mostrar ganho real.

`storage::AsyncFile` continua util para WAL e arquivo, mas sockets async exigem
desenho proprio por plataforma.

## 5. Fases de implementacao

### Fase A -- Processo e ADR

Objetivo: registrar a decisao corrigida antes de mudar o servidor.

Tarefas:

- [ ] Criar ADR que estenda ou substitua a ADR-011 para concorrencia de
      servidor, single-writer e leitores paralelos.
- [ ] Adicionar uma Fase 17 em `docs-process/PLANO_ODB.md`.
- [ ] Adicionar entradas correspondentes no `docs-process/RASTREADOR.md`.
- [ ] Declarar se replica de leitura esta fora do escopo desta fase ou se o
      apply de WAL sera tratado como writer local.

Criterio de pronto:

- A documentacao deixa claro que a primeira entrega e multi-conexao segura, nao
  runtime coroutine.

### Fase B -- Multi-conexao com thread por sessao

Objetivo: remover a serializacao de `serve_forever()`.

Tarefas:

- [x] Alterar o accept loop para criar uma sessao independente por conexao.
- [x] Introduzir estado por sessao no servidor (`SessionState`, ainda local ao
      modulo).
- [x] Usar codec negociado por sessao nos workers de query.
- [x] Proteger `last_stats_` contra corrida e retornar copia.
- [ ] Mover diagnosticos restantes para um `SessionContext` publico/estavel se
      a API precisar expor sessoes.
- [ ] Implementar shutdown ativo de sessoes longas quando `request_stop()` for
      chamado.
- [x] Testar dois clientes conectados ao mesmo tempo executando writes remotos.
- [ ] Testar dois clientes conectados ao mesmo tempo com cliente lento e
      disconnect.

Criterio de pronto:

- Duas conexoes conseguem fazer handshake e manter requests simultaneos sem
  depender do fechamento uma da outra. Para writes, o corte atual ja cobre duas
  conexoes chamando `OpCall` read-write em paralelo, serializadas pelo lock do
  motor.

### Fase C -- Seguranca fisica do caminho de leitura

Objetivo: tornar scans/gets read-only seguros fora do lock global.

Tarefas:

- [ ] Auditar `BufferPool`, `ScratchPagePool`, codecs, iteradores e facades
      usados por `run_query`.
- [ ] Escolher estrategia inicial: `shared_mutex` + scratch por sessao/thread,
      cache read-only por sessao, ou latches por shard.
- [ ] Implementar protecao/isolamento minimo.
- [ ] Adicionar teste de stress de leituras paralelas para reproduzir ausencia
      de corrupcao.
- [ ] Usar TSAN ou ferramenta equivalente quando disponivel no ambiente.

Criterio de pronto:

- Leituras paralelas repetidas nao corrompem heap, LRU, scratch pages ou estado
  de codec.

### Fase D -- Reducao do `engine_mutex_`

Objetivo: permitir que queries read-only avancem em paralelo.

Tarefas:

- [ ] Medir tempo de posse do `engine_mutex_` antes da mudanca.
- [ ] Reduzir o lock para regioes realmente compartilhadas.
- [ ] Garantir que commits/mutacoes continuem serializados.
- [ ] Testar N scans read-only simultaneos com snapshots distintas.
- [ ] Testar read-only longa enquanto outra conexao faz request independente.

Criterio de pronto:

- `run_query` read-only nao fica serializado por um mutex global durante toda a
  geracao do resultado.

### Fase E -- Baseline de rede

Objetivo: transformar medicao em regressao automatizada.

Dependencia: alvo de cliente de carga planejado na Subfase G do plano de carga.

Tarefas:

- [ ] Criar cenarios `server.concurrent_reads` e
      `server.one_writer_many_readers`.
- [ ] Medir TTFR, p50/p95/p99, throughput, conexoes ativas, uso de memoria,
      tempo de lock e bytes enviados.
- [ ] Rodar com `wal_io=sync` e `wal_io=async` quando aplicavel.
- [ ] Versionar o formato dos resultados.

Criterio de pronto:

- O baseline mostra se a mudanca melhorou concorrencia real ou apenas moveu
  gargalo para lock, CPU, rede ou WAL.

### Fase F -- Um escritor e N leitores

Objetivo: habilitar o fluxo principal do produto.

Tarefas:

- [ ] Classificar requests como read-only ou mutaveis.
- [ ] Executar read-only sob `Snapshot` propria da stream.
- [ ] Manter mutacoes serializadas.
- [ ] Testar leitor longo vendo epoch antigo enquanto commit independente
      avanca.
- [ ] Testar segundo update do mesmo objeto enquanto snapshot antiga continua
      aberta.
- [ ] Definir politica de `snapshot_conflict`: erro imediato, retry com
      backoff, ou retry controlado pelo cliente.

Criterio de pronto:

- O servidor suporta `1 writer + N readers` com consistencia definida e conflito
  de snapshot observavel.

### Fase G -- Writer queue dedicada

Objetivo: tornar a serializacao de escrita explicita e mensuravel.

Tarefas:

- [ ] Implementar fila MPSC de `WriteCommand`.
- [ ] Garantir ordering apenas dentro da mesma conexao.
- [ ] Definir `WriteResult` com erro de dominio, erro de commit e cancelamento
      antes de iniciar.
- [ ] Medir profundidade da fila, tempo de espera e tempo de execucao.
- [ ] Impor limite de fila e erro claro em saturacao.
- [ ] Testar rollback quando a operacao falha.

Criterio de pronto:

- Varias conexoes podem enviar writes, mas apenas uma transacao mutavel executa
  por vez, com fila, metricas e limites claros.

### Fase H -- Creditos e backpressure

Objetivo: preparar streaming concorrente sem fila invisivel.

Tarefas:

- [ ] Definir janela por conexao/query em bytes, frames ou objetos convertidos
      para bytes.
- [ ] Aplicar credito antes de produzir o proximo lote.
- [ ] Testar cliente lento, cancelamento no meio da stream e reutilizacao da
      conexao apos cancelamento.
- [ ] Medir creditos pendentes, buffers em memoria e latencia de cancelamento.

Criterio de pronto:

- Um cliente lento nao aumenta memoria indefinidamente e nao bloqueia sessoes
  independentes.

### Fase I -- WAL async no servidor

Objetivo: medir ganho real do `WalIoMode::async` no caminho remoto.

Tarefas:

- [ ] Adicionar opcao de servidor/CLI para `wal_io=sync|async`.
- [ ] Expor em diagnostico ou handshake o modo efetivo do WAL.
- [ ] Rodar `server.write_queue.{sync,async}` com 1, 4 e 16 clientes.
- [ ] Separar tempo de fila, tempo de WAL, tempo de apply e tempo de rede.

Criterio de pronto:

- Existe comparacao A/B reproduzivel entre WAL sync e async no servidor.

### Fase J -- Coroutines e socket async, se necessario

Objetivo: substituir ou complementar thread por sessao somente com evidencia.

Tarefas:

- [ ] Definir `net::Task<T>` ou runtime interno equivalente.
- [ ] Criar scheduler minimo com `post`, timers, cancelamento e completion de
      I/O de rede.
- [ ] Implementar `AsyncSocket` Windows/Linux.
- [ ] Portar handshake e streaming para o caminho async mantendo wrapper
      sincrono para compatibilidade.
- [ ] Comparar thread por sessao vs coroutine em conexoes ociosas, cliente
      lento, throughput e memoria.

Criterio de pronto:

- O caminho coroutine melhora um cenario relevante ou reduz complexidade sem
  regredir os testes de protocolo.

### Fase K -- Executor de leitura/CPU, se necessario

Objetivo: escalar trabalho CPU-bound sem confundir isso com I/O async.

Entrada nesta fase so se benchmark mostrar gargalo em decode, filtro,
compressao, materializacao, B-tree ou algoritmos de grafo.

Tarefas:

- [ ] Criar read executor com tamanho configuravel.
- [ ] Enfileirar lotes pesados preservando ordering por `query_id`.
- [ ] Medir overhead de handoff.
- [ ] Garantir que cancelamento remove trabalho ainda nao iniciado.

Criterio de pronto:

- O executor melhora cenario CPU-bound sem piorar o caminho leve alem do limite
  aceito.

### Fase L -- Politica operacional de snapshots

Objetivo: observar e controlar custo de snapshots longas no MVCC atual.

Tarefas:

- [ ] Medir `open_snapshot_count`, oldest epoch, idade da snapshot mais antiga
      e `snapshot_conflict` por commit.
- [ ] Adicionar alerta/limite para snapshots longas.
- [ ] Testar leitor lento + updates repetidos no mesmo objeto.
- [ ] Criar ADR separada se houver decisao por multiplas versoes anteriores por
      objeto.

Criterio de pronto:

- O sistema torna visivel quando leitores longos impedem updates do mesmo
  objeto e qual politica foi aplicada.

## 6. Metricas obrigatorias

| Metrica | Por que |
|---|---|
| conexoes ativas e sessoes em execucao | prova que o servidor deixou de ser mono-conexao |
| tempo de posse do `engine_mutex_` | mostra se o lock global ainda serializa leitura |
| espera em locks/latches do `BufferPool` | separa seguranca fisica de gargalo novo |
| TTFR por query | mede resposta inicial sob concorrencia |
| p50/p95/p99 de request | mostra cauda de latencia |
| throughput com N readers | prova paralelismo de leitura |
| throughput com 1 writer + N readers | prova objetivo central |
| profundidade e espera da writer queue | mostra gargalo de escrita |
| tempo de WAL e apply | decide se `WalIoMode::async` ajuda |
| `snapshot_conflict` por commit | mede limite real do MVCC single-previous |
| idade da snapshot mais antiga | antecipa conflitos e retencao |
| creditos/buffers pendentes | valida backpressure |
| RSS/pico de buffers | protege contra fila invisivel |
| latencia de cancelamento | garante cancelamento cooperativo util |

## 7. Riscos

1. **Heap corruption mascarada por lock global**: reduzir `engine_mutex_` cedo
   demais pode reabrir o bug. Mitigacao: Fase C antes da Fase D.
2. **Coroutine parecer paralelismo**: coroutine melhora espera, nao torna
   estruturas compartilhadas seguras. Mitigacao: thread por sessao e metricas
   antes do runtime async.
3. **LRU virar gargalo**: `BufferPool::get()` muta estado em leitura. Mitigacao:
   medir espera em lock e evoluir para sharding/latches se necessario.
4. **Snapshot longa gerar conflitos**: o modelo guarda uma versao anterior.
   Mitigacao: medir `snapshot_conflict`, definir retry e alertar idade.
5. **Backpressure quebrado por envio async**: sem credito, producer pode correr
   a frente do socket. Mitigacao: Fase H antes de socket async amplo.
6. **Ordering global acidental**: uma writer queue pode parecer contrato global.
   Mitigacao: documentar e testar apenas ordering intra-conexao.
7. **AsyncFile superestimado**: arquivo async existente nao resolve rede.
   Mitigacao: tratar sockets async como fase propria.

## 8. Criterio de aceite do plano completo

O plano estara concluido quando:

- o servidor aceitar multiplas conexoes simultaneas;
- estado mutavel por sessao nao ficar compartilhado no `Server`;
- read-only sob snapshot rodar sem serializacao integral por `engine_mutex_`;
- o caminho de leitura estiver fisicamente protegido;
- writes de multiplos clientes forem serializados por writer queue observavel;
- `snapshot_conflict` por segundo update em snapshot longa tiver teste e
  politica definida;
- backpressure por credito impedir crescimento ilimitado de buffers;
- `WalIoMode::sync` e `WalIoMode::async` forem comparaveis no servidor;
- coroutines/socket async forem adotados apenas se trouxerem ganho ou simplificacao
  mensurada;
- a documentacao publica explicar que o servidor e concorrente, mas o banco
  preserva single-writer.

## 9. Ordem recomendada

1. Fase A: ADR, Fase 17 e rastreamento.
2. Fase B: multi-conexao com thread por sessao.
3. Fase C: seguranca fisica do caminho de leitura.
4. Fase D: reducao do `engine_mutex_`.
5. Fase E: baseline de rede baseado no alvo de carga.
6. Fase F: `1 writer + N readers` com politica de `snapshot_conflict`.
7. Fase G: writer queue dedicada.
8. Fase H: creditos e backpressure.
9. Fase I: WAL async no servidor e A/B.
10. Fase J: coroutines/socket async somente se necessario.
11. Fase K: executor de leitura/CPU somente se necessario.
12. Fase L: politica operacional de snapshots.

Essa ordem preserva a ideia de evoluir para async, mas troca a aposta inicial:
primeiro o servidor precisa aceitar trabalho concorrente de verdade e provar que
o caminho de leitura e seguro. Depois, com gargalos medidos, coroutines,
socket async, WAL async e executores entram onde pagarem a propria
complexidade.
