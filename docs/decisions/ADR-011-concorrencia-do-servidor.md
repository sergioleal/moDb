# ADR-011 — Modelo de concorrência do servidor

- Estado: aceito para a Fase 8A
- Data: 2026-07-18

## Contexto

Até a Fase 7 o motor embedded opera sob a premissa single-thread /
single-writer: um processo, um escritor de transação, e componentes como o
`ScratchPagePool` sem sincronização. A Fase 8 introduz um processo servidor
com conexões de rede. Uma thread por conexão que faz `send` bloqueante não
consegue, ao mesmo tempo, receber `Cancel` nem multiplexar várias consultas
na mesma conexão.

O [PROTOCOLO_FASES.md](../PROTOCOLO_FASES.md) e o
[streaming.md](../../streaming.md) exigem backpressure fim-a-fim (fila mínima),
cancelamento cooperativo e, na Fase 8E, `co_await stream.next()`. A
[ADR-010](ADR-010-protocolo-binario-proximo-do-armazenamento.md) já fixou o
transporte (`NativeSocket`) e o contrato lógico; falta o modelo de threads e
filas.

Premissas que permanecem válidas:

- uma instância do servidor dedicada a uma aplicação (poucas conexões);
- um único escritor de transação no motor (Fase 5);
- o `DatabaseRegistry` já protege o mapa de bancos com `std::mutex`.

Premissas que deixam de valer no processo servidor:

- “todo o motor é single-thread”;
- uso livre do `ScratchPagePool` a partir de várias conexões sem isolamento.

## Decisão

O servidor usa papéis separados por conexão, não “uma thread que faz tudo”:

1. **Aceitação** — uma thread (ou o laço principal) faz `accept` e cria o
   contexto da conexão.
2. **Leitor por conexão** — uma thread lê frames do socket, despacha
   `Hello`/`Query`/`Cancel` e nunca bloqueia em `send` longo.
3. **Workers de consulta** — a execução do pipeline preguiçoso (Fase 7) corre
   em worker(s); o snapshot permanece vivo até o fim do fluxo ou cancelamento.
4. **Escritor dedicado por conexão** — uma thread (ou tarefa) serializa a
   escrita no socket; mensagens de várias consultas na mesma conexão são
   enfileiradas e enviadas em ordem de prontidão, preservando `query_id`.

### Fila de saída e backpressure

A fila entre a produção do generator e o escritor limita-se a **no máximo um
`ObjectFrame`** (ou constante pequena equivalente de objetos em trânsito).
Quando a fila está cheia ou o `send` bloqueia, o generator não avança e o
scan suspende — a mesma propagação natural da Fase 7, agora atravessando a
fronteira de rede.

Não há buffer intermediário de resultados “para encher o frame”: coalescência
física continua oportunista (ADR-010).

### Cancelamento e multiplexação

O leitor permanece ativo durante o envio. `Cancel` marca o
`CancellationToken` da consulta correspondente; o worker para de produzir e o
escritor pode emitir o encerramento adequado. A conexão permanece utilizável
para novas consultas (Fase 8E).

Vários `query_id` na mesma conexão são permitidos; a escrita no socket é
serializada pelo escritor dedicado.

### API assíncrona do cliente (`co_await`)

O cliente C++ expõe consumo incremental. Na Fase 8E,
`co_await stream.next()` usa um executor explícito documentado na API
(thread do chamador com espera no socket, ou executor injetável). A
semântica de `next()` permanece: sucesso com objeto, `nullopt` em
`StreamEnd`, ou `Error` em `StreamError`/falha de protocolo.

### Componentes single-thread a revisar

| Componente | Situação | Ação no servidor |
|---|---|---|
| `DatabaseRegistry` | Já usa `mutex_` | Manter; attach/find/detach sob o lock |
| `ScratchPagePool` | Vetor sem sincronização; documentado single-thread | Não compartilhar entre conexões: pool por conexão/worker ou serializar o acesso |
| Transações / WAL | Single-writer | Uma escrita de cada vez; leituras sob snapshot podem ser concorrentes |
| Cursores / generators | Estado por fluxo | Um fluxo por consulta; cancelamento cooperativo já existente |

A escolha fina (pool por conexão vs. mutex global no scratch) fica na
implementação das subfases 8B–8E, desde que duas conexões nunca mutem o mesmo
`ScratchPagePool` sem sincronização.

## Consequências

- Backpressure e cancelamento deixam de depender de “uma thread bloqueada no
  `send` ainda conseguir ler o socket”.
- O motor continua single-writer para mutações; concorrência é de leitura e de
  I/O de rede.
- `ScratchPagePool` e quaisquer outros caches sem lock precisam de isolamento
  por conexão ou proteção explícita antes de aceitar múltiplas conexões.
- A API `co_await` do cliente fica alinhada ao modelo (leitura desacoplada da
  produção no servidor), sem exigir asio no MVP.
- Asio permanece fora do MVP (ADR-010); se a complexidade do modelo acima
  crescer demais, reavalia-se numa ADR futura.
