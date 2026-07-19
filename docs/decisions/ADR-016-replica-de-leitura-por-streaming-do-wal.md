# ADR-016 — Réplica de leitura por streaming do WAL

- Estado: aceito para a Fase 14
- Data: 2026-07-19

## Contexto

Depois do servidor de rede (Fase 8), do runtime de módulos (Fase 9), da
estabilização (Fase 10) e da implantação serverless (Fase 13), o moDb ainda
tem uma única instância que serve leituras e escritas. Cargas de leitura
pesadas competem com o escritor e não há como escalar leitura nem manter uma
cópia quente do banco atualizada de forma contínua.

O modelo transacional permanece **single-writer** (ADR-011, ADR-013): um único
produtor de commits por banco. Replicação, alta disponibilidade e execução
distribuída estavam explicitamente fora do plano. Esta ADR abre uma exceção
estreita: **uma réplica de leitura (follower read-only) alimentada por
streaming do WAL do primary**, sem promoção, failover automático nem
multi-writer.

O WAL atual, porém, não sustenta replicação como está:

- é recriado do zero a cada commit e removido após o apply
  ([`src/tx/wal.cpp`](../../src/tx/wal.cpp),
  [`src/object/database.cpp`](../../src/object/database.cpp));
- o `lsn` é um número de registro local ao arquivo e reinicia em `1` a cada
  sessão, assim como `tx_id`;
- não há histórico retido para reconexão, nem checkpoint como posição
  persistente (checkpoint é hoje a simples remoção do WAL);
- o banco não tem identidade persistente (`DatabaseId` é de runtime e nunca é
  gravado) nem noção de *timeline*;
- o leitor local para silenciosamente no primeiro registro truncado, o que um
  stream de rede não pode interpretar como fim lógico válido.

Portanto, a Fase 14 precisa primeiro transformar o WAL numa **sequência global,
durável e retida**, e só então transmiti-la.

## Decisão

**A Fase 14 introduz um WAL durável com LSN global, identidade persistente do
banco e um follower read-only que aplica o stream de forma idempotente,
expondo leituras consistentes.** Promoção e failover ficam fora da fase.

### 1. Identidade persistente do banco

- Cada banco ganha um `DatabaseUuid` estável (gerado uma vez, gravado no DBRT
  ou em página de controle) e um `timeline_id`.
- O `timeline_id` muda quando o banco é restaurado/recriado de forma
  divergente, permitindo ao follower detectar que sua origem não é mais a mesma.
- `DatabaseId` (runtime) e `BaselineId` (estrutura do catálogo) **não**
  substituem `DatabaseUuid`.

### 2. WAL v2 durável e retido

- LSN global monotônico por banco, **nunca reiniciado** por commit ou processo;
  `commit_lsn` marca a fronteira replicável de cada transação.
- WAL segmentado e append-only; o registro de commit passa a carregar o
  `commit_lsn`.
- Checkpoint deixa de ser "apagar o WAL" e passa a ser uma **posição
  persistente**; segmentos são retidos até (a) as páginas locais estarem
  checkpointadas e (b) a política de retenção/ACK do follower permitir descarte.
- O leitor de replicação valida magic, versão, page size, continuidade de LSN,
  sequência begin/commit e CRC; truncamento de rede é erro, não fim lógico.

### 3. Bootstrap consistente do follower

1. O follower informa `DatabaseUuid`/`timeline_id`/`applied_lsn` conhecidos ou
   pede bootstrap inicial.
2. O primary toma uma barreira do escritor, conclui o commit em voo e fixa um
   corte `(cut_lsn, epoch, baseline)`.
3. Copia o arquivo para um snapshot base consistente e garante retenção do WAL
   estritamente posterior ao corte antes de liberar o escritor.
4. Envia manifesto + chunks com CRC/hash; o follower grava em temporário,
   sincroniza, valida e renomeia atomicamente.
5. O follower abre o snapshot e assina o WAL a partir de `cut_lsn + 1`.

Como `allocate_page()` altera arquivo/superbloco fora do WAL, o bootstrap sem
barreira poderia misturar `page_count`, DBRT e páginas de commits diferentes;
a barreira do escritor é obrigatória.

### 4. Aplicação idempotente

- Valida UUID, timeline, formato, page size, continuidade de LSN e CRC.
- Persiste a transação recebida em spool local sincronizado antes de aplicar.
- Ignora commit com `commit_lsn <= applied_lsn`; exige o próximo LSN esperado e
  **nunca** pula gap.
- Aplica todas as after-images sob lock exclusivo, faz flush, persiste
  `applied_lsn`, ressincroniza o `ObjectStore` e só então envia ACK.
- A idempotência reaproveita a garantia atual do recovery: reescrever páginas
  completas com a mesma after-image é seguro sob repetição.

### 5. Follower read-only

- O follower tem arquivo local próprio; **nunca** compartilha volume com o
  primary. O *applier* é o único escritor interno.
- APIs públicas de escrita (`begin`, bind/evolução de schema, GC, operações
  read-write) retornam erro `replica_read_only`. Consultas, snapshots e
  operações declaradas read-only continuam permitidas.
- "Read-only" é para clientes; o applier ainda precisa de acesso de escrita ao
  arquivo local.

### 6. Consistência de leitura na réplica (primeira versão)

Dado o IDMP v2 (ADR-009) mantém apenas uma versão anterior e as páginas
recebidas refletem as decisões de retenção do **primary**, aplicar WAL físico
concorrentemente a snapshots longos no follower não é seguro. Nesta fase:

- cada query/snapshot fixa a época aplicada e mantém um lock compartilhado
  durante todo o seu fluxo;
- o applier adquire lock exclusivo por transação; a nova época só fica visível
  após todas as páginas e metadados aplicados;
- o follower **não** executa GC local independente.

Consultas longas aumentam o lag, mas nunca leem estado misto. Aplicação
realmente concorrente exigiria MVCC de páginas/copy-on-write no follower —
mudança maior, fora desta fase.

### 7. Canal de replicação privilegiado

A ADR-010 proíbe expor páginas e WAL no protocolo público. A Fase 14 registra
a exceção: um **canal de replicação interno, autenticado e distinto** do
protocolo de consulta, reutilizando framing, validação, reader dedicado,
cancelamento e backpressure da Fase 8. Mensagens previstas:

`ReplicationHello`/`ReplicationHelloOk`, `BootstrapRequest`,
`BootstrapBegin`/`BootstrapChunk`/`BootstrapEnd`,
`WalSubscribe{database_uuid,timeline,from_lsn}`,
`WalFrame{first_lsn,last_lsn,bytes,crc}`, `WalAck{applied_lsn}`,
`WalGap{oldest_available_lsn}`, `ReplicationHeartbeat`,
`ReplicationError`/`Cancel`.

### 8. Reconexão, gap e lag

- Reconexão sempre parte de `applied_lsn + 1`; duplicatas são aceitas.
- Ordem divergente, UUID/timeline incompatível ou gap interrompem o apply.
- Se o LSN pedido ainda estiver retido, retoma incremental; se estiver abaixo
  de `oldest_available_lsn`, o primary responde `WalGap` e o follower exige novo
  bootstrap. Nunca há descarte silencioso nem salto automático.
- Métricas: `received_lsn`, `applied_lsn`, `primary_commit_lsn`, lag em
  bytes/commits/tempo; heartbeat informa o commit do primary mesmo sem tráfego.

## Consequências

- Leitura passa a escalar horizontalmente por followers read-only sem violar o
  single-writer nem mudar o modelo transacional do primary.
- O WAL deixa de ser efêmero: ganha LSN global, segmentação, retenção e
  checkpoint como posição — mudança que também beneficia diagnóstico e backup.
- O banco ganha identidade persistente (`DatabaseUuid`, `timeline_id`), útil
  além da replicação.
- Custo: consultas longas na réplica aumentam o lag; a réplica não oferece
  isolamento snapshot concorrente com o apply nesta primeira versão.
- Ficam **fora do escopo**: promoção do follower, eleição de líder, failover
  automático, fencing distribuído, replicação síncrona/quórum, múltiplos
  followers com consenso e resolução de conflito multi-writer.
- Novos erros da Fase 14: `replica_read_only`, `replication_gap`,
  `timeline_mismatch`, `database_uuid_mismatch` e `bootstrap_required`.
- A ADR-013 é atualizada: o item que excluía "replicação" passa a remeter a
  esta ADR para a réplica **de leitura**, mantendo HA/failover/distribuição
  fora do plano.
