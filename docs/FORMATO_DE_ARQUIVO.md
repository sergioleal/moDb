# Formato de arquivo do moDb

## Estado

Este documento descreve a versão `1` do formato inicial. O formato ainda é
experimental e poderá mudar antes da primeira release estável.

## Regras gerais

- tamanho de página fixado na compilação: 4096 bytes por padrão, 8192 bytes
  na variante de 8 KiB ou 16384 bytes na variante de 16 KiB;
- todos os inteiros são codificados em little-endian;
- a contagem de páginas inclui o superbloco;
- `PageId` corresponde ao índice da página no arquivo;
- o offset físico de uma página é `PageId * page_size`;
- estruturas C++ nunca são copiadas diretamente para o arquivo;
- bytes reservados são escritos como zero.

## Página zero — superbloco

| Offset | Tamanho | Campo | Valor inicial |
|---:|---:|---|---|
| 0 | 4 | magic number | `MODB` |
| 4 | 2 | versão do formato | `1` |
| 6 | 2 | reservado | `0` |
| 8 | 4 | tamanho da página | valor de `MODB_PAGE_SIZE` |
| 12 | 8 | quantidade de páginas | `1` |
| 20 | 8 | raiz futura do catálogo | `UINT64_MAX` |
| 28 | `page_size - 28` | reservado | zeros |

O superbloco somente pode ser alterado internamente pelo gerenciador de páginas.
A API pública rejeita tentativas de sobrescrever a página zero.

## Validação durante a abertura

Um arquivo é rejeitado quando:

- possui menos de uma página;
- seu tamanho não é múltiplo do tamanho de página compilado;
- não começa com `MODB`;
- declara uma versão diferente de `1`;
- declara outro tamanho de página;
- a quantidade de páginas não corresponde ao tamanho físico do arquivo.

## Alocação

Páginas são alocadas sequencialmente e inicializadas com zeros. A primeira
página disponível para dados recebe `PageId{1}`. Reutilização de espaço livre
será adicionada em uma versão posterior.

## Inteiros e leitura binária

`BinaryWriter` grava inteiros sem sinal de 8, 16, 32 e 64 bits. Tipos maiores
que um byte usam little-endian. `BinaryReader` mantém uma posição na entrada e
rejeita qualquer leitura que ultrapasse os bytes disponíveis.

Os codecs públicos exigem que todo o conteúdo seja consumido. Bytes adicionais
depois de um objeto completo são tratados como erro.

## Value

Todo `Value` começa com uma tag de um byte:

| Tag | Tipo | Conteúdo após a tag |
|---:|---|---|
| 0 | `NULL` | nenhum |
| 1 | `BOOLEAN` | 1 byte, obrigatoriamente `0` ou `1` |
| 2 | `INTEGER` | 8 bytes com os bits de `int64_t` |
| 3 | `REAL` | 8 bytes com os bits IEEE 754 de `double` |
| 4 | `TEXT` | tamanho `uint32_t` seguido pelos bytes do texto |

As tags são valores do formato de arquivo e não dependem dos valores internos
dos enums C++.

## Row

Uma linha é codificada como:

```text
uint16_t value_count
Value values[value_count]
```

Cada `Value` possui tamanho variável e é lido de acordo com sua própria tag.

## Schema

Um schema começa com sua quantidade de colunas em `uint16_t`. Cada coluna é
codificada como:

```text
uint8_t name_size
byte name[name_size]
uint8_t data_type
uint8_t nullable
```

`data_type` usa as tags `1` a `4` da tabela de `Value`. A tag de `NULL` não é um
tipo de coluna válido. `nullable` aceita somente `0` ou `1`.

Depois da decodificação, o schema passa novamente pelas validações de nomes,
duplicidades e limite máximo de 256 colunas.

## Slotted page

Uma página de registros possui a assinatura `SLPG` e a versão interna `3`.
Seu cabeçalho ocupa 28 bytes:

| Offset | Tamanho | Campo |
|---:|---:|---|
| 0 | 4 | assinatura `SLPG` |
| 4 | 1 | versão da slotted page |
| 5 | 1 | reservado |
| 6 | 2 | quantidade de slots |
| 8 | 2 | primeiro byte livre depois do diretório |
| 10 | 2 | primeiro byte ocupado pela área de registros |
| 12 | 8 | `PageId` da próxima página do `TableHeap`; zero indica fim |
| 20 | 8 | `PageId` da página anterior do `TableHeap`; zero indica início |

O diretório começa no offset 28 e cresce em direção ao final da página. Cada
slot ocupa oito bytes:

```text
uint16_t record_offset
uint16_t record_size
uint16_t record_capacity
uint16_t generation
```

`record_size` é o conteúdo lógico. Novas inserções e atualizações mantêm
`record_capacity` igual a `record_size`, sem reservar folga para crescimento. Um
slot removido mantém sua geração, mas zera offset, tamanho e capacidade. Ao reutilizá-lo, a geração é
incrementada. Um slot removido que alcançou `UINT16_MAX` é aposentado e não volta
a receber registros, evitando que a geração retorne a um valor associado a um
`RecordId` antigo.

Os registros crescem do final da página em direção ao diretório. Uma inserção
precisa da capacidade reservada e, quando não existe slot livre, mais oito bytes
para uma nova entrada. Remoções compactam as capacidades restantes e devolvem o
intervalo recuperado ao espaço livre central.

```text
0                                                        4095
| cabeçalho | slots -> | espaço livre | <- registros |
```

O endereço estável é formado por `RecordId{PageId, SlotId, generation}`. A geração
faz um identificador antigo falhar depois que seu slot é removido e reutilizado.
Esta versão implementa inserção, leitura, atualização, remoção, reutilização de
slots e compactação.

## TableHeap

Um `TableHeap` possui uma raiz dedicada de metadados, separada das slotted pages
que armazenam registros. A raiz usa a assinatura `THRP`, versão interna `1`, e
os primeiros 40 bytes têm este formato:

| Offset | Tamanho | Campo |
|---:|---:|---|
| 0 | 4 | assinatura ASCII `THRP` |
| 4 | 2 | versão da raiz (`1`) |
| 6 | 2 | flags reservadas (`0`) |
| 8 | 8 | `PageId` da primeira página de dados; zero indica heap vazio |
| 16 | 8 | `PageId` da última página de dados; zero indica heap vazio |
| 24 | 8 | quantidade de páginas de dados |
| 32 | 8 | quantidade de registros |

Os bytes restantes da raiz são reservados e permanecem zerados. Seu `PageId` é
a identidade estável usada para reabrir o heap. Um heap recém-criado possui
ambos os extremos e os dois contadores zerados, sem alocar uma slotted page.

Quando uma linha não cabe nas páginas existentes, uma nova página é alocada e
ligada pelos campos `next_page` e `previous_page`. A raiz é atualizada por último
com o novo `last_page`, `page_count` e `record_count`. O scan parte de
`first_page`, segue as ligações à frente e visita os slots em ordem crescente.
A ligação reversa permite retirar uma página vazia sem procurar sua antecessora
desde o início da cadeia.

Durante `open()`, o heap percorre a cadeia uma vez e reconstrói em memória o
conjunto de páginas pertencentes ao heap e a capacidade real de inserção de cada
página. Inserções consultam esse mapa para reutilizar espaço recuperado sem I/O
de páginas certamente cheias. Quando nenhuma candidata comporta o registro, a
nova página é ligada diretamente depois de `last_page`, sem percorrer a cadeia.

O maior registro aceito ocupa `page_size - 36` bytes: o tamanho da página menos
28 bytes do cabeçalho e 8 bytes para sua entrada de slot. Isso corresponde a
4.060 bytes na build padrão, 8.156 bytes na variante de 8 KiB e 16.348 bytes na
variante de 16 KiB. Uma linha maior é rejeitada; ela não é dividida entre
páginas nesta versão. A abertura valida assinatura e versão da raiz, extremos,
contadores, todas as páginas alcançáveis, ligações nas duas direções e ciclos.

Quando uma página de dados fica vazia, ela é retirada da cadeia lógica, inclusive
se for a primeira, a última ou a única. Nesse último caso, a raiz volta ao estado
vazio. O espaço físico órfão ainda não é reutilizado pelo `PageFile`; isso
dependerá de um gerenciador persistente de páginas livres.

Ao abrir uma slotted page, o moDb valida assinatura, versão, fronteiras, slots,
intervalos, lacunas e sobreposições antes de entregar qualquer registro.
