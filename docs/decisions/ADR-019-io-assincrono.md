# ADR-019 — I/O assíncrono

- Estado: aceito para a Fase 13
- Data: 2026-07-20

## Contexto

Ring0 já possui `NativeFile`, que oferece I/O posicional e `sync` durável com
contrato simples e previsível. Esse caminho síncrono continua sendo a base de
durabilidade. A Fase 13 acrescenta um backend assíncrono para permitir múltiplas
leituras/escritas em voo sem mudar o formato em disco nem a semântica do WAL.

O objetivo não é esconder custo nem introduzir filas ilimitadas: produtores
devem respeitar backpressure e usar barreiras explícitas quando a ordem de
durabilidade importa.

## Decisão

Adicionar `storage::AsyncFile` como camada opcional de I/O posicional
assíncrono:

1. **Backends por plataforma.** Windows usa IOCP com `FILE_FLAG_OVERLAPPED`.
   Linux/POSIX usa AIO (`aio_read`, `aio_write`, `aio_suspend`, `aio_fsync`).
2. **Contrato próximo de `NativeFile`.** A API expõe `read_at`, `write_at`,
   `sync`, `close` e também `submit_*` + `drain`/`barrier`.
3. **Barreiras explícitas.** Grupos de leitura/escrita podem executar em
   paralelo; `submit_sync`/`barrier` força completion antes de prosseguir.
   Assim o padrão WAL → sync → páginas continua expresso no chamador.
4. **Backpressure.** `max_inflight` limita operações aceitas antes de
   `drain`; exceder o limite retorna `invalid_argument`.
5. **Cancelamento.** `cancel_all` descarta operações ainda não drenadas. A API
   não promete cancelar trabalho já em execução por outra thread; no desenho
   atual, submissão ao SO acontece durante `drain`.
6. **Fallback.** A enumeração reserva `sync_fallback`, mas Windows e Linux
   devem usar backend nativo; `require_async` falha apenas quando um backend
   futuro precisar cair para fallback.
7. **Sem troca automática do storage.** `PageFile`/WAL continuam usando
   `NativeFile` até uma etapa separada decidir onde habilitar `AsyncFile` por
   medição.

## Consequências

- A fase adiciona uma superfície testável para I/O assíncrono sem alterar o
  formato do banco.
- O código de storage pode migrar para `AsyncFile` por pontos de uso e sempre
  manter `NativeFile` como baseline de comparação.
- Chamadores precisam ser explícitos sobre barreiras de durabilidade; submissão
  assíncrona isolada não substitui `sync`.
- Benchmarks devem comparar latência, throughput e uso de memória entre o
  caminho síncrono e o assíncrono antes de promover o backend para hot paths.
