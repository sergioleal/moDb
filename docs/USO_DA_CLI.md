# Uso da CLI (`modb`)

> A CLI mistura dois grupos de comandos:
>
> - **Orientado a Objetos (ODB++), persistente** — `oo` usa classes C++ reais,
>   `Binding`, `Handle` e `ProjectionPlan`; `type`, `baseline` e `object`
>   inspecionam o formato genérico persistente. É o caminho vigente do produto
>   (Fase 3 do [PLANO_ODB.md](PLANO_ODB.md)).
> - **Ferramentas de armazenamento cru** — `db`, `page`, `record`, `heap` e
>   `codec` operam nas camadas físicas (páginas, slotted pages, TableHeap) e
>   usam `Row`/`Value` como registro de exemplo. Úteis para inspeção/depuração.
> - **`types`** é uma vitrine **em memória** do modelo de objetos (não
>   persiste); os equivalentes persistentes são `type`/`object`.
>
> O modelo de dados relacional (`Catalog`/`Table`/o antigo comando `catalog`)
> foi removido no pivô (ver
> [ADR-006](decisions/ADR-006-destino-do-codigo-relacional.md)).

## Compilar

```sh
cmake --preset debug
cmake --build build/debug
```

O executável sai em `build/debug/modb` (ou `modb.exe` no Windows). Os
exemplos abaixo usam `modb` — ajuste o caminho conforme seu preset.

## Visão geral

```text
$ modb --help
Usage:
  modb <command> [arguments]

Commands:
  demo     Print a step-by-step tour of the CLI.
  db       Manage database files.
  page     Manage individual database pages.
  record   Manage records stored in one page.
  heap     Manage multi-page table heaps.
  codec    Encode and decode a row in memory.
  types    Exercise the in-memory object model (ODB++).
  type     Define and list persistent object types (ODB++).
  baseline Inspect immutable catalog baselines (ODB++).
  object   Create, read and remove persistent objects (ODB++).
  oo       Use compiled C++ bindings, handles and schema projection.
  blob     Store and read chained BLBP blobs (ODB++ Fase 4).
  graph    Demo an object graph: refs, embedded, cascade (ODB++ Fase 4).
  coll     Demo persistent vector/set/map collections (ODB++ Fase 4).
  tx       Exercise transactions, the WAL and recovery (ODB++ Fase 5).

Options:
  -h, --help     Show this help.
  -v, --version  Show the moDb version.

Run 'modb <command> --help' for command-specific usage.
```

Cada subcomando tem sua própria ajuda: `modb <comando> --help`.

## Convenções

### Erros e código de saída

Erros de domínio (arquivo não encontrado, argumento inválido, etc.) imprimem
`Error: <mensagem>` em stderr e saem com código **1**:

```text
$ modb db info nao-existe.modb
Error: database file not found: nao-existe.modb
```

Um comando desconhecido ou uso incorreto da linha de comando sai com
código **2**:

```text
$ modb foo
Unknown command: foo
Use --help for usage.
```

### Sintaxe `typed-value`

Comandos que recebem valores tipados (`record insert-values`,
`heap insert-values`, `heap update-values`) usam a sintaxe `tipo:conteúdo`:

| Sintaxe | Tipo | Exemplo |
|---|---|---|
| `null` | `NULL` (sem `:`) | `null` |
| `boolean:true` / `boolean:false` | `BOOLEAN` | `boolean:true` |
| `integer:<n>` | `INTEGER` (i64 decimal com sinal) | `integer:-42` |
| `real:<n>` | `REAL` (double finito) | `real:3.14` |
| `text:<conteúdo>` | `TEXT` (todos os bytes após o primeiro `:`) | `text:Beatriz` |

Um valor mal formado retorna erro explícito, por exemplo:

```text
$ modb record insert-values demo.modb 2 integer:abc
Error: id must be a signed 64-bit decimal integer
```

### Identificação de registro (`page:slot:generation`)

Comandos de `heap` que atualizam ou removem um registro pedem
`<page> <slot> <generation>` (três argumentos separados, não uma string só).
A saída do `scan`/`list`, porém, mostra no formato compacto
`page:slot:generation`, por exemplo `4:0:1`.

## `modb demo`

Imprime um roteiro guiado com todos os comandos abaixo, na ordem certa, prontos
para copiar e colar (assume que `demo.modb` ainda não existe).

```text
modb demo              # imprime o roteiro
modb demo run [-force] # executa o roteiro de verdade, passo a passo
```

`-force` permite rodar mesmo se `demo.modb` já existir (apaga e recomeça). É a
forma mais rápida de ver a CLI inteira funcionando: `modb demo run -force`.

## `modb db` — arquivo de banco

```text
modb db create <file>
modb db info <file>
modb db check <file>
modb db repair <file>
modb db delete <file>
```

- **`create`**: cria um arquivo novo (superbloco + versão do formato). Falha
  se o arquivo já existir.
- **`info`**: mostra versão do formato, tamanho de página e contagem de
  páginas.
- **`check`**: valida a estrutura do arquivo inteiro (superbloco, páginas
  soltas, slotted pages, raízes de `TableHeap`) — ver
  [database_check](../src/storage/database_check.cpp).
- **`repair`**: reparo estrutural. Percorre **todas** as raízes `TableHeap`
  do arquivo e reconstrói os contadores de cada uma (`first`/`last`/
  `page_count`/`record_count`) a partir da cadeia de páginas autodescritiva —
  tornando abrível de novo um heap cujos metadados divergiram após uma falha
  parcial de escrita. É idempotente: num banco saudável não reescreve nada.
  Uma raiz irreparável (ciclo ou página de dados corrompida) é reportada sem
  impedir o reparo das demais. Não confundir com a recuperação baseada em WAL
  (recovery), um mecanismo separado previsto para a Fase 5 do
  [plano OO](PLANO_ODB.md).
- **`delete`**: remove o arquivo.

```text
$ modb db create demo.modb
Database created
Path: "demo.modb"
Format version: 1
Page size: 4096
Page count: 1

$ modb db info demo.modb
Path: "demo.modb"
Format version: 1
Page size: 4096
Page count: 1

$ modb db check demo.modb
Database check: "demo.modb"
Page count: 5
Unformatted pages: 1
Slotted pages: 2
TableHeap roots: 1
Database is valid

$ modb db repair demo.modb
Database repair: "demo.modb"
TableHeap roots found: 1
  root 3: pages=1 records=1 (already consistent)
Roots rewritten: 0
Database repair complete

$ modb db delete demo.modb
Database deleted: "demo.modb"
```

### Exemplo de recuperação (contador de raiz corrompido)

```text
# um heap com 2 registros teve o record_count da raiz corrompido:
$ modb heap scan loja.modb 1
Error: TableHeap root counters or chain endpoints are inconsistent

$ modb db repair loja.modb
Database repair: "loja.modb"
TableHeap roots found: 1
  root 1: pages=1 records=2 (rewritten)
Roots rewritten: 1
Database repair complete

$ modb heap scan loja.modb 1
Records in TableHeap 1: 2
2:0:1 | 10 | Ana
2:1:1 | 20 | Beatriz
```

## `modb page` — páginas cruas

```text
modb page create <file>
modb page info <file> <page-id>
```

- **`create`**: aloca uma página nova, sem formatá-la (fica zerada).
- **`info`**: despeja o conteúdo bruto da página em hexdump (16 bytes por
  linha, offset à esquerda).

```text
$ modb page create demo.modb
Allocated page: 1
Page count: 2

$ modb page info demo.modb 1
00000000  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
...
```

## `modb record` — registros em uma única página (slotted page)

```text
modb record page-create <file>
modb record insert <file> <page-id> <id> <name>
modb record insert-values <file> <page-id> <typed-value>...
modb record read <file> <page-id> <slot-id>
modb record list <file> <page-id>
modb record page-layout <file> <page-id>
```

- **`page-create`**: aloca e formata uma página como slotted page vazia.
- **`insert`**: atalho para inserir a linha fixa `(INTEGER id, TEXT name)`.
- **`insert-values`**: insere uma linha com colunas arbitrárias via
  `typed-value` (ver [convenções](#sintaxe-typed-value)).
- **`read`**: lê um slot específico.
- **`list`**: lista todos os slots ocupados da página.
- **`page-layout`**: imprime um diagrama do layout físico (header, diretório
  de slots, espaço livre, área de registros) — útil para entender
  [slotted_page.cpp](../src/storage/slotted_page.cpp).

```text
$ modb record page-create demo.modb
Record page created: 2
Free space: 4068 bytes

$ modb record insert demo.modb 2 1 Ana
Record inserted: page 2, slot 0, generation 1
Free space: 4041 bytes

$ modb record insert-values demo.modb 2 integer:2 text:Beatriz boolean:true
Record inserted: page 2, slot 1, generation 1
Free space: 4008 bytes

$ modb record read demo.modb 2 0
Record 2:0: 1 | Ana

$ modb record list demo.modb 2
Records in page 2: 2
Slot 0 (generation 1): 1 | Ana
Slot 1 (generation 1): 2 | Beatriz | true
Free space: 4008 bytes

$ modb record page-layout demo.modb 2
Slotted page layout
PageId: 2
Format version: 3
Total size: 4096 bytes
Record count: 2

Slot count: 2
Record capacity policy: exact logical size
Previous page: start
Next page: end

Areas:
  Header:         [0, 28) = 28 bytes
  Slot directory: [28, 44) = 16 bytes
  Free space:     [44, 4052) = 4008 bytes
  Records:        [4052, 4096) = 44 bytes

Logical layout:
  [ HEADER 28 B ][ SLOTS 16 B ][ FREE 4008 B ][ RECORDS 44 B ]

Slots:
  Id  State  Gen  Directory entry  Record range       Size/Capacity
  000  used  001  [0028, 0036)    [4077, 4096)    19/19 bytes
  001  used  001  [0036, 0044)    [4052, 4077)    25/25 bytes
```

## `modb heap` — registros em múltiplas páginas (`TableHeap`)

```text
modb heap create <file>
modb heap insert-values <file> <root-page-id> <typed-value>...
modb heap scan <file> <root-page-id>
modb heap layout <file> <root-page-id>
modb heap update-values <file> <root> <page> <slot> <generation> <typed-value>...
modb heap delete <file> <root> <page> <slot> <generation>
modb heap repair <file> <root-page-id>
```

- **`create`**: aloca a página raiz dedicada (`THRP`) de um heap vazio.
- **`insert-values`**: insere um registro, alocando páginas de dados
  conforme necessário (ver [table_heap.cpp](../src/storage/table_heap.cpp)).
- **`scan`**: varre sequencialmente todos os registros vivos do heap.
- **`layout`**: lista as páginas de dados da cadeia (registros, bytes livres,
  ligação `next`).
- **`update-values`**: sobrescreve o registro identificado por
  `page slot generation`.
- **`delete`**: remove o registro; a página é compactada/desligada da cadeia
  se ficar vazia.
- **`repair`**: reconstrói a raiz a partir da cadeia física, sem confiar nos
  contadores persistidos — usar quando `db check` reportar inconsistência.

```text
$ modb heap create demo.modb
TableHeap created
Root page: 3

$ modb heap insert-values demo.modb 3 integer:10 text:Ana
Record inserted: page 4, slot 0, generation 1

$ modb heap insert-values demo.modb 3 integer:20 text:Beatriz
Record inserted: page 4, slot 1, generation 1

$ modb heap scan demo.modb 3
Records in TableHeap 3: 2
4:0:1 | 10 | Ana
4:1:1 | 20 | Beatriz

$ modb heap layout demo.modb 3
TableHeap layout
Root page: 3
Page  Records  Free bytes  Next
4     2        4010        end
Pages: 1
Records: 2

$ modb heap update-values demo.modb 3 4 0 1 integer:10 text:Ana-Maria
Record updated: page 4, slot 0, generation 1

$ modb heap delete demo.modb 3 4 1 1
Record deleted: 4:1:1

$ modb heap scan demo.modb 3
Records in TableHeap 3: 1
4:0:1 | 10 | Ana-Maria
```

## `modb codec` — round-trip de codificação em memória

```text
modb codec
```

Sem argumentos: codifica e decodifica uma linha fixa em memória (não toca
disco), só para demonstrar o [codec](../src/storage/codec.cpp).

```text
$ modb codec
Original row: 1 | Ana
Encoded size: 19 bytes
Decoded row: 1 | Ana
Round-trip: OK
```

## `modb types` — modelo de objetos em memória (ODB++)

```text
modb types
```

Sem argumentos: define um tipo `Employee` (atributos `name`, `salary`
obrigatórios e `country` opcional com default `"BR"`), registra no
[`TypeRegistry`](../include/modb/object/type_registry.hpp), valida dois
objetos lógicos contra o tipo — um completo e outro que omite `country` e é
completado pelo default — usando
[`validate_object`](../include/modb/object/type_definition.hpp). **Nada é
persistido** — é uma vitrine em memória. Os equivalentes **persistentes** são
os comandos `type` e `object` documentados adiante.

```text
$ modb types
Registered type: Employee (id 16)
Attributes:
  1: name (STRING, not null)
  2: salary (FLOAT64, not null)
  3: country (STRING, nullable, default: BR)
Valid object: 1=Ana | 2=15000 | 3=US
Valid object (country omitted, covered by its default): 1=Beatriz | 2=12000
Note: this example is in memory; use type/object/oo for persistence.
```

## `modb type` — tipos persistentes (ODB++)

```text
modb type define <file> <name> <attr[#id]:type[:null][=default]>...
modb type list <file>
modb type history <file> <name>
```

- **`define`**: registra uma versão persistente. `#id` mantém o FieldId estável
  entre versões; quando omitido, usa a posição por compatibilidade. `:null`
  marca nullable e `=valor` declara o default. Tipos: `boolean`, `int64`,
  `float64`, `string`, `bytes`. Cria a
  hierarquia do object store (DBRT/mapa/heaps) na primeira vez, num arquivo
  criado por `db create`.
- **`list`**: lista os tipos registrados e seus atributos.
- **`history`**: lista todas as versões, marcando a ativa.

```text
$ modb db create loja.modb
$ modb type define loja.modb Employee name#1:string salary#2:float64 country#3:string=BR
Type defined: Employee (id 16)

$ modb type list loja.modb
Employee (id 16)
  1: name (STRING, not null)
  2: salary (FLOAT64, not null)
  3: country (STRING, not null)
```

## `modb object` — objetos persistentes (ODB++)

```text
modb object get <file> <object-id> [--definition]
```

- **`get`**: recupera e imprime um objeto pelo id; `--definition` inclui a
  TypeDefinition histórica usada pelo registro.

As antigas escritas cruas `object create` e `object remove` estão desabilitadas:
elas não possuíam `Transaction` e podiam contornar o WAL. Escritas devem passar
pela API `Database` transacional.

## `modb baseline` — snapshots imutáveis do catálogo

```text
modb baseline list <file>
modb baseline show <file> <baseline-id>
```

`list` mostra todas as baselines e marca a corrente. `show` resolve os
TypeDefinitionIds históricos que compõem um snapshot.

## `modb oo employee` — API tipada da Fase 3

Este grupo contém `EmployeeV1` e `EmployeeV2` compilados na própria CLI. Assim,
ele exercita a API que depende de ponteiros para membros C++ reais:

```text
modb oo employee init <file> --schema <1|2>
modb oo employee create <file> <name> <salary> [country] --schema <1|2>
modb oo employee evolve <file> --schema <1|2>
modb oo employee get <file> <object-id> --schema <1|2>
modb oo employee set-salary <file> <object-id> <salary> --schema <1|2>
modb oo employee demo <file> [--force]
```

O caminho mais curto para comprovar a Fase 3 é:

```text
$ modb oo employee demo phase3.modb --force
v1 wrote Employee{id=18, name=Ana, salary=15000}
v2 projected old object: country=BR annual_salary=180000
lazy migration rewrote Employee 18 as v2
v2 wrote Employee{id=21, country=PT}
Phase 3 OO demo: OK

$ modb type history phase3.modb Employee
Employee (id 16)
Employee (id 19) [current]

$ modb baseline list phase3.modb
Baseline 17 (1 types)
Baseline 20 (1 types) [current]
```

`get --schema 2` materializa objetos da versão 1 através do `ProjectionPlan`; o
default de `country` é `"BR"`. `set-salary --schema 2` usa `Handle::set` e regrava o
objeto antigo com a definição corrente, demonstrando a migração preguiçosa.

## `modb blob` — binários encadeados (ODB++ Fase 4)

Exercita o `BlobStore` diretamente: grava um texto numa cadeia de páginas `BLBP`
e o lê de volta. `put` cria o arquivo se ele não existir e imprime o `BlobId` da
primeira página; `info` percorre a cadeia sem materializar tudo.

```text
modb blob put <file> <text>
modb blob get <file> <blob-id>
modb blob info <file> <blob-id>
```

```text
$ modb blob put dados.modb "hello phase 4 blob"
BlobId 1
$ modb blob get dados.modb 1
hello phase 4 blob
$ modb blob info dados.modb 1
Blob 1: pages=1 bytes=18
```

## `modb graph demo` — grafo de objetos (ODB++ Fase 4)

Mostra os quatro tipos de relacionamento numa passada de ponta a ponta:
associação (`Ref`), valor embutido (`Embedded`), composição (`OwnedRef`) e uma
`PersistentVector<Ref<Project>>`. Grava o grafo, reabre o arquivo, resolve cada
aresta e então remove o pai para evidenciar a cascata (o filho `owned` some; os
objetos de associação sobrevivem).

```text
modb graph demo <file> [--force]
```

```text
$ modb graph demo grafo.modb --force
wrote Staff{id=28, name=Ana}
  dept -> Department{id=24} (association)
  home -> Address{street=Rua das Flores} (embedded)
  badge -> Badge{id=27, code=7} (owned)
  projects -> PersistentVector<Ref<Project>> with 2 refs (blob 8)

reopened and resolved Staff 28:
  name=Ana
  home.street=Rua das Flores (embedded, no id)
  dept -> Engenharia
  projects -> Apollo Gemini
  badge -> code 7 (owned)

removed Staff 28:
  owned Badge cascaded away: yes
  associated Department survived: yes
  referenced Projects survived: yes

Phase 4 graph demo: OK
```

## `modb coll demo` — coleções persistentes (ODB++ Fase 4)

Exercita `PersistentVector`, `PersistentSet` (deduplicação e ordem) e
`PersistentMap` (`put`/`get`/`remove`), tudo sobrevivendo a uma reabertura.

```text
modb coll demo <file> [--force]
```

```text
$ modb coll demo colecoes.modb --force
wrote PersistentVector<int64> with 5 elements
wrote PersistentSet<int64> from 7 inserts -> 4 unique elements
wrote PersistentMap<string,int64>: put ana/bia, replaced ana, removed bia -> 1 entry

reopened collections:
  vector sum = 150 (expected 150)
  map[ana] = 15 (expected 15)
  map[bia] = absent (expected absent)

Phase 4 collection demo: OK
```

## `modb tx` — transações, WAL e recuperação (ODB++ Fase 5)

```text
modb tx demo <file> [--force]
modb tx crash <file> <before-commit|after-commit|mid-apply|before-cleanup> [--force]
modb tx wal-info <file>
modb tx get <file> <object-id>
```

- **`demo`**: narra commit, rollback explícito, rollback por destrutor (sem
  `commit()`) e o contrato `transact()` (Ok → commit; erro → rollback
  automático) num único processo. Reabre o arquivo ao final e confere que só o
  que foi commitado sobrevive.
- **`crash`**: grava um `Account` numa transação, leva o commit até a fase
  pedida e então chama `std::exit` — **nenhum destrutor roda**, simulando de
  verdade um processo morto (não é um truque de teste; é o processo real
  terminando). Imprime o `ObjectId` gravado, para inspecionar depois.
- **`wal-info`**: leitura pura do arquivo `<file>.wal` (sem abrir o banco nem
  disparar recuperação) — mostra quantos registros existem, de que tipo, e
  quais transações têm registro de `commit`.
- **`get`**: abre o banco (o que **dispara a recuperação automaticamente**,
  antes de qualquer outra coisa) e busca o objeto pelo id — é assim que se
  observa o resultado de um `crash`.

O par `crash`/`wal-info`/`get` é a forma mais direta de ver o WAL e a
recuperação funcionando de verdade, através de processos separados (o "crash"
é real, não simulado dentro do mesmo processo):

```text
$ modb tx crash conta.modb before-commit --force
staged Account{id=18, owner=Ana, balance=1000}
commit stopped BEFORE the commit record: only page images reached the WAL
recovery will discard this transaction entirely
WAL present: yes
exiting now without further cleanup (simulated crash) -- inspect with:
  modb tx wal-info conta.modb
  modb tx get conta.modb 18

$ modb tx wal-info conta.modb
WAL: conta.modb.wal
records: 6 (begin=1 page_image=5 commit=0 checkpoint=0)
transactions seen: 1
  tx 2: NOT committed (recovery will discard it)

$ modb tx get conta.modb 18
WAL before opening: present
WAL after opening (recovery already ran): absent
Account 18: absent -- that transaction never became durable
```

Repita com `after-commit`, `mid-apply` ou `before-cleanup` no lugar de
`before-commit`: nesses três casos o `Account` **aparece** depois do `get`
(`owner=Ana balance=1000 -- present after recovery`) — a recuperação refaz a
transação inteira porque o registro de commit já estava durável no WAL antes
da queda simulada.

## Roteiro completo (equivalente a `modb demo run`)

```sh
modb --version
modb --help

modb db create demo.modb
modb db info demo.modb

modb page create demo.modb
modb page info demo.modb 1

modb record page-create demo.modb
modb record insert demo.modb 2 1 "Ana"
modb record insert-values demo.modb 2 integer:2 text:Beatriz boolean:true
modb record read demo.modb 2 0
modb record list demo.modb 2
modb record page-layout demo.modb 2

modb heap create demo.modb
modb heap insert-values demo.modb 3 integer:10 text:Ana
modb heap insert-values demo.modb 3 integer:20 text:Beatriz
modb heap scan demo.modb 3
modb heap layout demo.modb 3
modb heap update-values demo.modb 3 4 0 1 integer:10 text:Ana-Maria
modb heap delete demo.modb 3 4 1 1
modb heap scan demo.modb 3

modb codec
modb types

modb oo employee demo phase3-demo.modb --force
modb type history phase3-demo.modb Employee
modb baseline list phase3-demo.modb

modb db check demo.modb
modb db repair demo.modb
modb db delete demo.modb
modb db delete phase3-demo.modb
```

O roteiro combina as ferramentas de armazenamento cru com o cenário tipado
completo da Fase 3.

## MVCC — Fase 6A

```text
modb mvcc status <file>
modb mvcc upgrade <file>
modb mvcc tick <file>
```

`status` abre o banco e informa a época global. `upgrade` exercita a abertura
compatível que converte DBRT/IDMP v1 para v2 quando necessário. `tick` faz um
commit sem objetos para demonstrar que a época avança pelo WAL.

Roteiro passo a passo:

```text
# 1. Crie o banco (só na primeira vez).
modb db create exemplo-6a.modb

# 2. Confirme que a época inicial é zero e o formato é v2.
modb mvcc status exemplo-6a.modb

# 3. Execute um commit WAL que avança somente a época.
modb mvcc tick exemplo-6a.modb

# 4. Consulte novamente; a época deve ter avançado de 0 para 1.
modb mvcc status exemplo-6a.modb

# 5. Em um banco criado por versão anterior, execute a atualização compatível.
modb mvcc upgrade exemplo-6a.modb
```
