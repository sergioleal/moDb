# ADR-013 — Execução serverless em container

- Estado: aceito para a Fase 11
- Data: 2026-07-18

## Contexto

Após o servidor de rede (Fase 8), o runtime de módulos (Fase 9) e a
estabilização (Fase 10), o moDb precisa de um modo de implantação moderno:
imagem OCI, escala a zero e operação em plataformas de containers
"serverless".

O motor, porém, é **stateful**. A durabilidade depende de escrita posicional
e de `fsync`/`FlushFileBuffers` no arquivo do banco e no WAL
([`NativeFile`](../../src/storage/native_file.cpp)). Disco efêmero do
container, réplicas writers concorrentes e armazenamento objeto eventual não
preservam as garantias das Fases 5 e 6.

"Serverless" no sentido de funções estritamente stateless (muitas réplicas
efêmeras sem volume compartilhado consistente) conflita com o modelo
single-writer do banco.

## Decisão

**A Fase 11 empacota o servidor moDb como container serverless stateful**,
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
4. **Imagem mínima e não privilegiada.** Build multi-stage, usuário sem
   root, rootfs somente leitura, dados apenas no volume montado.
5. **Configuração externa.** Porta, caminhos, limites e secrets vêm do
   ambiente da plataforma. A imagem não carrega credenciais nem bancos.
6. **Término gracioso.** Em `SIGTERM`, a instância deixa de aceitar
   conexões, drena streams, conclui ou reverte transações em voo, sincroniza
   e sai antes do grace period. Término forçado continua coberto pelo WAL
   recovery no próximo cold start.
7. **Protocolo inalterado em espírito.** O ingresso da plataforma adapta
   transporte/porta; não altera o contrato lógico da [ADR-010](ADR-010-protocolo-binario-proximo-do-armazenamento.md)
   nem enfraquece backpressure ou cancelamento.

A plataforma de referência concreta (Kubernetes com scale-to-zero, Cloud Run
com volume, etc.) é escolhida na implementação da Fase 11 e registrada no
guia operacional, desde que satisfaça volume durável e no máximo uma
réplica writer.

## Consequências

- O moDb pode operar com custo próximo de zero quando ocioso, sem abrir mão
  da recuperação transacional.
- Cold start inclui custo de recovery; TTFR operacional é distinto do TTFR
  de consulta da Fase 7.
- Operadores não devem configurar duas réplicas writers no mesmo volume.
- Backup continua sendo cópia quiescente de `<db>` + `<db>.wal`, agora a
  partir do volume persistente.
- Replicação, sharding e multi-tenant por instância seguem fora do escopo.
