# ADR-022 — Menos `fsync` por commit: um sync de WAL e checkpoint preguiçoso

- Estado: **aceito** (medições em
  [PROFILING_2026-09.md](../../docs-process/PROFILING_2026-09.md), tarefas T5, T7.3 e T9).
  A/B alternado, sem power throttling (medianas; ver "P1 revisado" no
  relatório): só a parte B (T9) rende `mixed_oltp` **2,30×** e `snapshot_hold`
  **2,51×**; com a parte A e o CRC da T7.1, **2,65×** e **3,51×** sobre o motor
  anterior. Fases de `crud_full.100k` +6% a +34%, nenhuma pior. *(Uma versão
  anterior citava 2,72×/2,80×, medidos com o processo sob power throttling.)*
- Data: 2026-09-26
- Substitui a descrição do protocolo de commit em
  [GARANTIAS_TRANSACIONAIS.md §2](../GARANTIAS_TRANSACIONAIS.md)

## Contexto

A T3 do plano de desempenho mediu o commit do modo `full` e achou **quatro
`fsync` por commit**, a ~330 µs cada nesta máquina, somando ~85% do custo do
commit — que por sua vez é 93% de uma operação em `mixed_oltp` e 77% da fase
`update_shrink`. Os quatro, na ordem de `Database::commit_transaction`:

1. `wal->sync()` depois das imagens de página;
2. `wal->sync()` depois do registro de commit — **o ponto de commit**;
3. `file_->flush()` depois de aplicar as páginas ao arquivo de dados — a
   barreira que torna as páginas duráveis antes do checkpoint;
4. `file_->flush()` depois de gravar `checkpoint_lsn` no DBRT.

Só o 2 é exigido pela garantia de durabilidade. Os outros três existem por
escolhas de desenho que podem ser trocadas sem enfraquecer a garantia.

## Decisão

### Parte A — um único sync de WAL por commit (remove o 1)

Cada registro do WAL tem CRC32, e a leitura usada pela recuperação
(`Wal::read_all`, modo `soft_eof`) **para no primeiro registro truncado ou com
CRC inválido**: o que vem depois nunca é visto. Portanto, se uma queda deixar o
registro de commit no disco mas uma imagem anterior da mesma transação não, a
leitura para na imagem ruim, o commit não é visto e a transação é descartada
— o mesmo resultado de uma queda antes do commit. O sync entre as imagens e o
registro de commit não protege nada que o CRC não proteja. Ele é mantido
apenas no caminho de teste `CommitPhase::stop_after_images`, cujo contrato é
"imagens duráveis, sem commit".

### Parte B — checkpoint preguiçoso (remove o 3 e o 4 da maioria dos commits)

O checkpoint deixa de avançar a cada commit. O commit passa a ser: imagens e
registro de commit no WAL, **um** sync do WAL, aplicar as páginas ao arquivo
de dados **sem** sync. A cada `checkpoint_interval` commits (e no fechamento
do banco), um checkpoint faz: `flush` do arquivo de dados, grava
`checkpoint_lsn` = LSN do último commit aplicado e `next_lsn`, `flush`.

Invariante que torna isso correto: **toda página cujo conteúdo no arquivo de
dados pode diferir do estado no `checkpoint_lsn` gravado tem uma imagem
inteira no WAL depois desse LSN.** Vale porque o checkpoint só avança depois
de um `flush` do arquivo de dados, e toda página modificada por um commit tem a
imagem no WAL daquele commit. A recuperação já reaplica, a partir do
checkpoint, as imagens das transações commitadas — idempotente, na ordem do
log. Uma página rasgada por uma queda no meio da escrita é sobrescrita pela
imagem.

## Consequências

- **Durabilidade inalterada.** Um commit confirmado tem o registro de commit
  sincronizado no WAL, como antes.
- **Recuperação mais longa**, limitada por `checkpoint_interval` commits de
  replay. O WAL já não é truncado hoje, então o tamanho do WAL não muda.
- **Superbloco além do fim do arquivo.** `allocate_page` grava o `page_count`
  direto no superbloco, fora do WAL. Sem sync a cada commit, uma queda pode
  persistir o superbloco sem persistir a extensão do arquivo. A abertura hoje
  recusa esse arquivo (`stored page count is larger than the database file`).
  Passa a aceitar, usando a contagem que cabe no arquivo; a recuperação
  re-estende a partir das imagens do WAL (`write_recovered_page` já faz isso).
  É seguro pela invariante acima: as páginas além do fim foram alocadas depois
  do último checkpoint e têm imagem no WAL.
- **DBRT rasgado.** Se a página DBRT (que guarda o `checkpoint_lsn`) ficar
  ilegível, a abertura já cai para `after_lsn = 0` e reaplica o WAL inteiro, o
  que reescreve a DBRT a partir da imagem mais recente.
- **Bootstrap de réplica** (`repl/bootstrap.cpp`) usa `checkpoint_lsn` como
  corte do snapshot. Com o checkpoint atrasado, o corte fica mais antigo e a
  réplica reaplica mais WAL a partir dele; as imagens são inteiras e
  idempotentes, então converge para o mesmo estado.
- **Backup do arquivo de dados sozinho** passa a exigir o WAL junto (ou um
  checkpoint antes da cópia), porque o arquivo de dados pode estar até
  `checkpoint_interval` commits atrás. `docs/OPERACAO.md` passa a dizer isso.
- `Durability::disabled_diagnostic` continua pulando todos os syncs, para
  medição.

## Alternativas consideradas

- **Manter os quatro syncs.** Descartado pela medição: é o maior custo do motor
  nos workloads com commit frequente.
- **Group commit** (vários commits concorrentes num sync). Ortogonal e
  complementar; só ajuda com escritores concorrentes, que o motor hoje
  serializa, e exigiria abortar em cascata quem leu um commit cujo `fsync`
  falhou. Adiado até haver medição de servidor com escritores concorrentes
  (T8 em [PROFILING_2026-09.md](../../docs-process/PROFILING_2026-09.md)).
- **Registro lógico/delta em vez de imagens inteiras** (T16). Reduz bytes, não
  o número de syncs; muda o formato e a recuperação. Fica para depois.
