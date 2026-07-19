# Operação serverless do moDb (Fase 13E)

Guia de referência para subir, operar e recuperar o servidor moDb em
container stateful, conforme [ADR-013](decisions/ADR-013-execucao-serverless-em-container.md).

## Modelo

- **Uma instância writer por banco.** Não configure duas réplicas ativas no
  mesmo volume.
- **Volume persistente obrigatório** para `<db>` e `<db>.wal`. Disco efêmero
  do container não é fonte de verdade.
- **Cold start = open + recovery.** Readiness (`MODB_READY_FILE`) só existe
  depois do listen bem-sucedido.
- **Escala a zero (0↔1)** é o alvo; escala horizontal de escrita fica fora.

## Variáveis de ambiente

| Variável | Default | Função |
|---|---|---|
| `MODB_DB_PATH` | `/data/db.modb` | Caminho do arquivo no volume |
| `MODB_HOST` | `0.0.0.0` | Bind do protocolo Fase 8 |
| `MODB_PORT` | `7400` | Porta TCP |
| `MODB_READY_FILE` | `/tmp/modb-ready` | Probe de readiness/startup |
| `MODB_LIVE_FILE` | `/tmp/modb-live` | Probe de liveness |
| `MODB_METRICS_FILE` | (opcional) | JSONL de eventos operacionais |

Segredos e credenciais **nunca** entram na imagem; só via env/secrets da
plataforma.

## Compose local

```text
cd deploy
docker compose up --build -d
```

Handshake (host):

```text
modb ping 127.0.0.1 7400 db.modb
```

Parada graciosa: `docker compose stop` envia SIGTERM; o processo remove o
arquivo ready e encerra o listener (`Server::request_stop`).

## Kubernetes

Manifesto mínimo: `deploy/k8s/modb.yaml` (PVC `ReadWriteOnce`, `replicas: 1`,
`Recreate`, probes por arquivo, `terminationGracePeriodSeconds: 30`).

## Backup e restauração

1. Pare a instância writer (SIGTERM) ou use janela de manutenção.
2. Copie `<db>` e `<db>.wal` do volume de forma consistente.
3. Restaure ambos no volume de destino **antes** do próximo cold start.
4. Suba uma única réplica; o open executa recovery automaticamente.

## I/O assíncrono

A API `AsyncFile` (Fase 13B) expõe `backend_name()` / `fallback_reason()`.
Neste build o backend típico é `sync_fallback` (NativeFile) até `io_uring`/
IOCP completos. O evento `ready` em `MODB_METRICS_FILE` registra
`async_file_backend` e a razão.

## Restrições

- Não compartilhe o volume entre writers.
- Não use object storage eventual como único store do arquivo do banco.
- Réplica de leitura por streaming do WAL é a Fase 14
  ([ADR-016](decisions/ADR-016-replica-de-leitura-por-streaming-do-wal.md)).

## CI da imagem

Workflow `.github/workflows/oci-image.yml`:

1. build multi-stage (`deploy/Dockerfile`);
2. SBOM (Syft);
3. scan (Trivy);
4. publish versionado em `ghcr.io` nas tags `0.0.*`.
