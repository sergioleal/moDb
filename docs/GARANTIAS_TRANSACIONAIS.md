# Garantias transacionais — Fase 5 (Transações, WAL e recuperação)

> Relatório das garantias oferecidas pelo gerenciador de transações, o WAL e a
> recuperação. Entregável da [Fase 5](PLANO_ODB.md#fase-5--transações-wal-e-recuperação).
> A fonte de verdade sobre o que **funciona** é a suíte (`ctest`): `modb.wal`,
> `modb.recovery`, `modb.failpoint`, mais os testes de escrita que passaram a
> exigir transação (`modb.binding`, `modb.schema_evolution`, `modb.relationship`,
> `modb.collection`).

## 1. Modelo

**WAL redo-only com after-images de página, single-writer.** Durante uma
transação, toda página modificada é bufferizada em memória pelo `PageFile` (não
toca o arquivo de dados). No commit, as imagens completas das páginas sujas são
escritas no write-ahead log (`<db>.wal`), o log é sincronizado, e só então as
páginas são aplicadas ao arquivo de dados. Rollback descarta o buffer — nada foi
aplicado.

- Um único escritor por vez: `Database::begin()` falha com `transaction_active`
  se já houver transação em andamento.
- O formato do WAL (`include/modb/tx/wal.hpp`): cabeçalho `MOWL` de 32 bytes +
  registros `| lsn | tx_id | tipo | page_id | length | payload | crc32 |`. Um
  registro com CRC inválido ou truncado marca o **fim lógico** do log.

## 2. Protocolo de commit

`Transaction::commit()` executa, em ordem
([src/object/database.cpp](../src/object/database.cpp), `commit_transaction`):

1. escreve `begin` + uma `page_image` por página suja no WAL;
2. **`sync` do WAL** (as imagens ficam duráveis);
3. escreve o registro `commit` e **`sync` do WAL** — *este é o ponto de commit*;
4. aplica as páginas ao arquivo de dados e faz **`flush`** (durabilidade real);
5. checkpoint: remove o WAL (as páginas já estão duráveis no arquivo de dados).

A ordem WAL-antes-dos-dados é o que garante atomicidade e durabilidade: uma
queda antes do passo 3 não deixa commit no log (a transação some); uma queda
depois do passo 3 deixa o commit durável no log (a transação é refeita).

## 3. Recuperação (na abertura)

`Database::open()` roda `tx::recover()` **antes** de reconstruir o catálogo
([src/tx/recovery.cpp](../src/tx/recovery.cpp)):

1. sem `<db>.wal` → nada a fazer;
2. lê os registros íntegros até o fim lógico; identifica as transações que têm
   registro `commit`;
3. reaplica as `page_image` das transações commitadas, na ordem do log;
4. `flush` do arquivo de dados e remove o WAL.

A recuperação é **idempotente**: reaplicar as mesmas imagens leva ao mesmo
estado, então uma segunda queda durante a própria recuperação apenas reaplica na
próxima abertura (testado em `modb.recovery`, caso "idempotent").

## 4. Garantias

- **Atomicidade.** Após uma queda, cada transação aparece **por completo** ou
  **não aparece** — nunca parcial. Comprovado pela matriz de failpoints.
- **Durabilidade.** Depois que `commit()` retorna `Ok`, os efeitos sobrevivem a
  uma queda (o registro `commit` está sincronizado no WAL antes de `commit()`
  retornar; a recuperação o reaplica se as páginas ainda não tinham sido gravadas).
- **Rollback e commit durável.** Antes de o registro de commit chegar ao WAL,
  `Transaction::rollback()` descarta o buffer e remove qualquer WAL residual; a
  transação que sai de escopo sem commit também é revertida pelo destrutor (RAII).
  Depois que o commit está durável, a transação é consumida mesmo se o `apply` ou
  `flush` falhar: o WAL é preservado, `rollback()` é recusado e a instância exige
  reabertura (`commit_recovery_required`) para fazer o redo idempotente. Assim o
  destrutor nunca apaga um commit já durável.
- **Contrato `transact(fn)`.** Término normal com `Ok` → commit; retorno de erro
  → rollback; exceção → rollback (via destrutor). É o contrato reutilizado pela
  Fase 9. Testado em `modb.recovery` (caso "transact").
- **Toda escrita da API pública exige transação.** `create`, `update`, `remove`
  e a criação/mutações de coleção (`Persistent*::create`,
  `push_back`/`insert`/`put`/`remove`) recebem `Transaction&` e checam que há
  uma transação ativa (`transaction_required`) e que o token pertence ao mesmo
  banco que originou a coleção.
  `bind()` é configuração e só é aceito sem transação do chamador; ele grava o
  catálogo sob uma **transação interna** e atualiza o binding em memória apenas
  depois da persistência confirmada (registrar/evoluir um tipo é atômico e passa
  pelo WAL como qualquer outra escrita).
  `Database::blobs()` também exige transação para `create`/`rewrite`/`remove`;
  o `BlobStore` cru construído sobre um `PageFile` permanece disponível para
  diagnóstico e ferramentas de baixo nível.

## 5. Matriz de failpoints (`modb.failpoint`)

Cada linha monta o cenário e reabre com o arquivo real, verificando tudo-ou-nada.
Dois mecanismos genuínos de "morte" são usados: **falha de I/O real** (um
`FailpointWalSink` faz escrita ou `sync` do WAL retornar `io_error`,
[tests/failpoint_file.hpp](../tests/failpoint_file.hpp)) e **queda pós-sync**
(interrompe-se num ponto e "abandona-se" a instância — detach do registro, para o
destrutor não reverter, e destrói-se o `Database`; o buffer em memória some, o
disco permanece).

| Ponto de falha | Mecanismo | Estado após reabertura |
|---|---|---|
| Falha de I/O na escrita do WAL | `FailpointWalSink` (io_error real) | transação revertida; banco não preso; objeto ausente |
| Falha no `sync` antes/depois do record `commit` | `FailpointWalSink` (io_error real) | transação revertida; WAL incompleto removido |
| Antes do registro de commit | interrupção + abandono | transação ausente por completo |
| Após o commit durável, antes de aplicar | interrupção + abandono | transação presente por completo (redo) |
| No meio da aplicação das páginas | apply-failpoint real + destrutor normal | presente por completo (reaplicação idempotente) |
| Durante o checkpoint (WAL residual) | interrupção + abandono | presente; WAL reaplicado e removido |

A matriz também executa queda pós-commit para `update`, remoção com cascata de
`OwnedRef` e um `PersistentMap` apoiado por `BlobStore`; após reabrir, cada
resultado é verificado no arquivo real. `transact()` é coberto para sucesso,
retorno de erro e exceção lançada pelo callback.

O apply-failpoint (`PageFile::set_apply_failpoint`) aplica só N páginas ao
arquivo de dados e então falha, deixando o estado parcial real no disco — a
recuperação reaplica tudo. O caso pós-commit deixa o destrutor real rodar para
provar que ele não remove o WAL durável. Nenhuma página é escrita à mão pelo teste.

Um WAL de zero bytes é descartado como log vazio. Já um cabeçalho parcial ou
ilegível não é considerado fim lógico: a abertura falha com `wal_corrupt` e
preserva `<db>.wal` para diagnóstico. Um registro final truncado ou com CRC
inválido mantém a semântica de fim lógico do formato.

## 6. Limitações e desvios documentados (MVP)

- **Rollback ressincroniza o `ObjectStore` inteiro a partir do disco.**
  `TableHeap`/`IdentityMap`/`CatalogStore`/`TypeRegistry` guardam contadores e
  cadeias em memória (`record_count`, `page_count`, `first`/`last`, mapas de
  capacidade) atualizados otimisticamente durante a transação — só o buffer de
  páginas do `PageFile` é descartado num rollback. Sem mais nada, a PRÓXIMA
  escrita real usaria esses contadores avançados (que nunca existiram em disco),
  corrompendo a raiz do heap na primeira reabertura seguinte. Corrigido:
  `Database::rollback_transaction()` chama `resync_store_after_rollback()`
  (reconstrói `store_` via `ObjectStore::open`, uma operação puramente de
  leitura) depois de descartar o buffer. Isto foi um bug real, encontrado ao
  exercitar `rollback → create → commit → reopen` pela CLI (`modb tx demo`) —
  a suíte de testes anterior não cobria esse caminho.
- **O contador de `ObjectId` nunca retrocede, mesmo com o resync acima.** Como
  o contador vive na mesma página (DBRT) que os demais campos gravados
  transacionalmente, reconstruir `store_` do disco reverteria também o avanço
  que a transação abortada fez nele — permitindo que um id já entregue por
  `create()` (ainda que nunca durável) fosse reatribuído a um objeto diferente
  depois. `rollback_transaction()` guarda esse contador (`watermark`) antes do
  resync e, se o valor em disco for menor, restaura-o com uma escrita imediata
  e durável (não há transação ativa nesse ponto, então não há risco de vazar
  outro campo do DBRT ainda em voo). Preserva a garantia do
  [ADR-001](decisions/ADR-001-identidade.md): `ObjectId` nunca é reutilizado,
  mesmo através de um rollback — apenas cria um **gap**.
- **`allocate_page` é imediato.** Páginas alocadas por uma transação abortada
  ficam órfãs no arquivo (sem free list no MVP), visíveis ao `database_check`.
- **Checkpoint = remoção do WAL.** Não há checkpoint incremental; o WAL é
  removido inteiro após a aplicação. Falha ao removê-lo não é silenciosa: o
  commit/abertura retorna erro e o WAL fica para redo idempotente na reabertura;
  um commit futuro recria o WAL normalmente. Não há teste portável que force
  essa falha de `std::filesystem::remove`.
- **`BlobStore` é primitivo de baixo nível.** Não recebe `Transaction&`: quando
  usado pelas coleções (API pública OO), as escritas são capturadas pela
  transação ativa do `PageFile` (as coleções exigem transação); usado direto
  (comando `blob` da CLI), é ferramenta de diagnóstico, como `record`/`heap`.
- **O "PageCache embrião" do protocolo** virou o buffer de páginas sujas dentro
  do próprio `PageFile` (mesmo papel: acumular páginas sujas e aplicá-las no
  commit), em vez de uma classe `storage::PageCache` separada. O BufferPool
  completo continua na Fase 10.
- **Link estático do MinGW desligado por padrão.** A toolchain do CLion migrou
  para GCC 15.2, cujo `libwinpthread.a` estático deixa símbolos indefinidos no
  link `-static` (`__intrinsic_setjmpex`/`__ms_vsnprintf`). O padrão passou a ser
  link dinâmico (`MODB_STATIC_MINGW_RUNTIME=OFF`); binários portáteis exigem uma
  toolchain onde o link totalmente estático funcione.

## 7. Critério de conclusão

✅ Matriz de failpoints 100% verde: nenhuma linha exibe transação parcial. Suíte
completa (59 testes) verde em Debug, `-Werror` e `sanitizers`.

## 8. Leituras versionadas (Fase 6B — snapshots)

A Fase 6B acrescenta leituras consistentes sobre o mesmo motor single-writer.
A fonte de verdade é `modb.snapshot` (mais `modb.identity_map`/`modb.object_store`
para as camadas). Detalhes de formato/época no
[ADR-009](decisions/ADR-009-epocas-e-idmp-v2.md).

- **Snapshot fixa uma época.** `Database::snapshot()` devolve um `Snapshot` RAII
  que captura a época corrente e a registra em memória
  (`std::multiset<epoch>`); o destrutor a desregistra. `get`/`scan` recebendo um
  `Snapshot` leem a versão **visível** naquela época: `current` se
  `current_epoch ≤ snapshot.epoch`, senão `previous`, senão o objeto não existia
  (criação/remoção/update posteriores são invisíveis ao snapshot). Leituras sem
  snapshot continuam devolvendo o último commit.
- **Uma única versão anterior por objeto.** O IdentityMap v2 guarda apenas
  `current` + `previous`. Uma segunda alteração de um objeto **enquanto a
  `previous` ainda é visível a um snapshot aberto mais antigo** falha com
  `snapshot_conflict`, antes de qualquer escrita — não há sobrescrita parcial. Se
  não houver snapshot aberto anterior à época corrente, a alteração reusa a
  posição `previous` normalmente.
- **`update` sempre insere; `remove` não apaga fisicamente.** Para que a versão
  `previous` sobreviva, `ObjectStore::update()` grava **sempre** um registro
  físico novo (nunca reusa o slot in-place) e `ObjectStore::remove()` apenas
  marca o tombstone lógico na identidade — o registro físico antigo permanece.
  Consequência: `scan`/`scan_at` passam a filtrar cada registro físico contra a
  identidade resolvida (`find`/`find_at`), ignorando cópias antigas/órfãs, para
  não enumerar duplicatas.
- **Recuperação de espaço fica para a Fase 6C.** Como no MVP não há free list,
  os registros físicos preservados por `update`/`remove` **não** são recuperados
  na Fase 6B; a retenção por época e o GC das versões obsoletas são o escopo
  explícito da [Fase 6C](PLANO_ODB.md#fase-6c--retenção-gc-e-concorrência). É um
  desvio de espaço consciente, no mesmo espírito do "`allocate_page` é imediato"
  da Fase 5.

Critério 6B: ✅ suíte completa (62 testes) verde em Debug, `-Werror` e
`sanitizers`; snapshot devolve o estado lógico da época mesmo com commits
concorrentes intercalados (`modb.snapshot`, `modb mvcc snapshot-demo`).
