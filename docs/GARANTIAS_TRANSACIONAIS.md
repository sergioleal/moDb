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
- **Rollback.** `Transaction::rollback()` descarta o buffer e remove qualquer WAL
  residual. Uma transação que sai de escopo **sem** commit é revertida no
  destrutor (RAII). Um commit que **falha** de verdade (I/O) mantém a transação
  ativa para o destrutor revertê-la — o `PageFile` nunca fica preso em transação.
- **Contrato `transact(fn)`.** Término normal com `Ok` → commit; retorno de erro
  → rollback; exceção → rollback (via destrutor). É o contrato reutilizado pela
  Fase 9. Testado em `modb.recovery` (caso "transact").
- **Toda escrita da API pública exige transação.** `create`, `update`, `remove`
  e as mutações de coleção (`push_back`/`insert`/`put`/`remove`) checam que há
  uma transação ativa (`transaction_required`) e que o token pertence a um banco.
  `bind()` grava o catálogo sob uma **transação interna** (registrar/evoluir um
  tipo é atômico e passa pelo WAL como qualquer outra escrita).

## 5. Matriz de failpoints (`modb.failpoint`)

Cada linha monta o cenário e reabre com o arquivo real, verificando tudo-ou-nada.
Dois mecanismos genuínos de "morte" são usados: **falha de I/O real** (um
`FailpointWalSink` faz uma escrita do WAL retornar `io_error`,
[tests/failpoint_file.hpp](../tests/failpoint_file.hpp)) e **queda pós-sync**
(interrompe-se num ponto e "abandona-se" a instância — detach do registro, para o
destrutor não reverter, e destrói-se o `Database`; o buffer em memória some, o
disco permanece).

| Ponto de falha | Mecanismo | Estado após reabertura |
|---|---|---|
| Falha de I/O na escrita do WAL | `FailpointWalSink` (io_error real) | transação revertida; banco não preso; objeto ausente |
| Antes do registro de commit | interrupção + abandono | transação ausente por completo |
| Após o commit durável, antes de aplicar | interrupção + abandono | transação presente por completo (redo) |
| No meio da aplicação das páginas | apply-failpoint real + abandono | presente por completo (reaplicação idempotente) |
| Durante o checkpoint (WAL residual) | interrupção + abandono | presente; WAL reaplicado e removido |

O apply-failpoint (`PageFile::set_apply_failpoint`) aplica só N páginas ao
arquivo de dados e então falha, deixando o estado parcial real no disco — a
recuperação reaplica tudo. Nenhuma página é escrita à mão pelo teste.

## 6. Limitações e desvios documentados (MVP)

- **Contadores em memória não são revertidos.** O contador de `ObjectId` (no
  `DatabaseRoot`) avança em memória mesmo numa transação revertida; a escrita no
  disco é descartada. O efeito é um **gap** de ids (nunca reuso), permitido pelo
  [ADR-001](decisions/ADR-001-identidade.md). Leituras após rollback são
  consistentes porque `IdentityMap`/`TableHeap` são respaldados por página (o
  descarte do buffer restaura o estado do disco).
- **`allocate_page` é imediato.** Páginas alocadas por uma transação abortada
  ficam órfãs no arquivo (sem free list no MVP), visíveis ao `database_check`.
- **Checkpoint = remoção do WAL.** Não há checkpoint incremental; o WAL é
  removido inteiro após a aplicação. Suficiente para single-writer.
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
completa (51 testes) verde em Debug, `-Werror` e `sanitizers`.
