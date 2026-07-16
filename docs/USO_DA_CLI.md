# Uso da CLI (`modb`)

> ⚠️ **A maior parte desta CLI ainda é do modelo relacional legado** — os
> comandos `db`, `page`, `record`, `heap` e `codec` que existem hoje no
> código. O projeto pivotou para um banco Orientado a Objetos (ver
> [PLANO_ODB.md](PLANO_ODB.md)). O comando **`types`** já é OO: demonstra em
> memória o modelo da Fase 1 (`TypeDefinition`/`TypeRegistry`/
> `validate_object`), mas ainda **não persiste nada** — é o mesmo espírito do
> `catalog` relacional (uma vitrine em memória). Os primeiros comandos OO
> persistentes (`type define`, `object create/get`) só chegam ao final da
> **Fase 2** (ver [RASTREADOR.md](RASTREADOR.md)). Este documento será
> revisado quando a Fase 2 aposentar os comandos relacionais (decisão
> registrada na [ADR-006](decisions/ADR-006-destino-do-codigo-relacional.md)).

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
  catalog  Exercise the in-memory catalog.
  types    Exercise the in-memory object model (ODB++).

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

## `modb catalog` — catálogo em memória

```text
modb catalog
```

Sem argumentos: cria uma tabela e insere uma linha usando o
[`Catalog`](../include/modb/catalog.hpp) **só em memória** — nada é
persistido (o aviso final da saída é literal: o catálogo relacional atual
não sobrevive ao fechamento do processo; ver
[ADR-006](decisions/ADR-006-destino-do-codigo-relacional.md)).

```text
$ modb catalog
Created table: users
Inserted row
Rows in users:
1 | Ana
Note: this catalog exists only in memory.
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
persistido**: é a vitrine em memória do modelo da Fase 1, no mesmo espírito
do `modb catalog` acima.

```text
$ modb types
Registered type: Employee (id 16)
Attributes:
  1: name (STRING, not null)
  2: salary (FLOAT64, not null)
  3: country (STRING, nullable, default: BR)
Valid object: 1=Ana | 2=15000 | 3=US
Valid object (country omitted, covered by its default): 1=Beatriz | 2=12000
Note: this type registry exists only in memory; persistence arrives in Fase 2 (see docs/PLANO_ODB.md).
```

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
modb catalog
modb types

modb db check demo.modb
modb db repair demo.modb
modb db delete demo.modb
```

## O que vem a seguir

Segundo o [RASTREADOR.md](RASTREADOR.md), a Fase 2 do plano OO
(`docs/PLANO_ODB.md`) prevê explicitamente:

- remoção dos comandos relacionais (`record`/`catalog` no sentido atual);
- comandos OO **persistentes**: `modb type define <db> <nome> <campo:tipo[:null]>...`,
  `modb type list <db>`, `modb object create/get/remove <db> ...` — o
  `modb types` atual (em memória) provavelmente é absorvido por eles.

Este documento será reescrito quando isso acontecer.
