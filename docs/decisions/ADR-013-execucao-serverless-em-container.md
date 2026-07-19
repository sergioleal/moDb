# ADR-013 — Execução serverless em container

- Estado: aceito para a Fase 13
- Data: 2026-07-18

## Contexto

Após o servidor de rede (Fase 8), o runtime de módulos (Fase 9), a
estabilização (Fase 10), o catálogo de facades (Fase 11) e os algoritmos de
grafos (Fase 12), o moDb precisa de um modo de implantação moderno: imagem
OCI, escala a zero e operação em plataformas de containers "serverless".

O motor, porém, é **stateful**. A durabilidade depende de escrita posicional
e de `fsync`/`FlushFileBuffers` no arquivo do banco e no WAL
([`NativeFile`](../../src/storage/native_file.cpp)). Disco efêmero do
container, réplicas writers concorrentes e armazenamento objeto eventual não
preservam as garantias das Fases 5 e 6.

"Serverless" no sentido de funções estritamente stateless (muitas réplicas
efêmeras sem volume compartilhado consistente) conflita com o modelo
single-writer do banco.

O `NativeFile` atual também executa I/O bloqueante: `pread`/`pwrite`/`fsync`
no POSIX e `ReadFile`/`WriteFile` sobre handle síncrono no Windows. Coroutines
no pipeline de consultas não tornam essas chamadas assíncronas; bloquear o
executor durante I/O reduz a concorrência e prejudica backpressure e latência
em containers com CPU limitada.

## Decisão

**A Fase 13 empacota o servidor moDb como container serverless stateful**,
não como função sem estado.

1. **Volume persistente obrigatório.** Os arquivos `<db>` e `<db>.wal` ficam
   em armazenamento que suporte I/O posicional e flush durável. O filesystem
   efêmero do container não é fonte de verdade.
2. **Uma instância ativa por banco.** Escala horizontal de escrita,
   multi-writer e HA distribuída permanecem fora do plano. Escala a zero
   (0↔1 réplica) é o alvo.
3. **Cold start = open + recovery.** Readiness só fica verde depois que o
   banco abriu e o WAL recovery terminou. Liveness não deve matar a
   instância durante recovery longo; usa-se startup probe com prazo adequado.
4. **I/O assíncrono real.** A Fase 13 introduz uma abstração de arquivo
   orientada a completion: `io_uring` no Linux e IOCP no Windows. Não será
   considerado assíncrono apenas deslocar operações bloqueantes para
   `std::async` ou uma thread por requisição. Submissão, completion,
   cancelamento, lifetime de buffers e fila limitada fazem parte do contrato.
   O ordering de durabilidade continua explícito: completion da escrita e do
   flush do WAL ocorre antes da liberação das escritas de páginas.
5. **Fallback detectado em runtime.** Kernels antigos, políticas seccomp e
   filesystems/volumes incompatíveis usam o backend síncrono existente. O
   backend escolhido e a razão do fallback são expostos por métricas; uma
   configuração pode exigir I/O assíncrono e falhar no startup em vez de
   degradar silenciosamente.
6. **Imagem mínima e não privilegiada.** Build multi-stage, usuário sem
   root, rootfs somente leitura, dados apenas no volume montado.
7. **Configuração externa.** Porta, caminhos, limites e secrets vêm do
   ambiente da plataforma. A imagem não carrega credenciais nem bancos.
8. **Término gracioso.** Em `SIGTERM`, a instância deixa de aceitar
   conexões, drena streams, conclui ou reverte transações em voo, sincroniza
   e sai antes do grace period. Término forçado continua coberto pelo WAL
   recovery no próximo cold start.
9. **Protocolo inalterado em espírito.** O ingresso da plataforma adapta
   transporte/porta; não altera o contrato lógico da [ADR-010](ADR-010-protocolo-binario-proximo-do-armazenamento.md)
   nem enfraquece backpressure ou cancelamento.

A plataforma de referência concreta (Kubernetes com scale-to-zero, Cloud Run
com volume, etc.) é escolhida na implementação da Fase 13 e registrada no
guia operacional, desde que satisfaça volume durável e no máximo uma
réplica writer.

## Consequências

- O moDb pode operar com custo próximo de zero quando ocioso, sem abrir mão
  da recuperação transacional.
- Cold start inclui custo de recovery; TTFR operacional é distinto do TTFR
  de consulta da Fase 7.
- O backend Linux da imagem depende de `io_uring` liberado pelo kernel,
  seccomp e volume da plataforma; compatibilidade deve ser comprovada no
  ambiente de referência, não presumida.
- Windows usa IOCP para manter a mesma semântica assíncrona fora do container
  Linux; ambos os backends compartilham testes de contrato e durabilidade.
- Fila e bytes em voo são limitados para preservar memória e backpressure.
- Operadores não devem configurar duas réplicas writers no mesmo volume.
- Backup continua sendo cópia quiescente de `<db>` + `<db>.wal`, agora a
  partir do volume persistente.
- Replicação, sharding e multi-tenant por instância seguem fora do escopo.
