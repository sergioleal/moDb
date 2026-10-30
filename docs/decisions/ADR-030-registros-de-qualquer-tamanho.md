# ADR-030 — Registros de qualquer tamanho: páginas de overflow no TableHeap

- Estado: aceito
- Data: 2026-10-30
- Relacionados: [ADR-009](ADR-009-epocas-e-idmp-v2.md) (versões),
  [ADR-023](ADR-023-lista-de-paginas-livres-do-heap.md) (lista livre),
  [ADR-024](ADR-024-recuperacao-imediata-da-versao-sobrescrita.md) (versão sobrescrita),
  [ADR-027](ADR-027-leitores-concorrentes-escritor-exclusivo.md) (um escritor)

## Contexto

Um objeto persistido é um registro do `TableHeap`, e um registro precisava caber
numa slotted page: no máximo `page_size − 36` bytes (16348 no SDK). Um objeto
maior falhava com `record_too_large`, e a aplicação tinha que dividir o conteúdo
por conta própria em blobs ou coleções.

## Decisão

Uma camada dentro do `TableHeap`, entre a API de registros e as páginas físicas,
guarda um registro de qualquer tamanho numa cadeia de **páginas de overflow** e
deixa na slotted page só um **stub** de tamanho fixo. A camada fica por baixo de
`insert`, `read`, `scan_records`, `read_page_records`, `update` e `erase`. Quem
usa o heap (`ObjectStore`, MVCC, índices, GC, catálogo, consultas, CLI) continua
recebendo os bytes inteiros do registro e não muda.

### Registros marcados

Heaps novos ligam o bit 0 das flags da raiz THRP ("registros marcados"). Num heap
marcado, cada registro na slotted page começa com um byte:

```text
0x00 | bytes do registro                               registro inline
0x01 | tamanho_total u64 | primeira_página u64           stub (17 bytes)
```

Um registro fica inline quando `1 + tamanho` cabe na slotted page; senão, vai
para overflow. O `RecordId` é sempre o do slot: o mapa de identidade e as versões
do MVCC não mudam.

A marca fica no registro porque o slot não tem bit livre, e o conteúdo de um
registro é arbitrário: sem um byte de tipo, um stub poderia ser confundido com um
registro comum.

### Página de overflow (`OVFP`)

```text
| "OVFP" | versão u16 (1) | reservado u16 | next u64 (0 = fim) | tamanho u32 | reservado u32 | dados |
```

O cabeçalho tem 24 bytes, e cada página leva `page_size − 24` bytes de dados. A
cadeia é ligada por `next`; a soma dos tamanhos é o `tamanho_total` do stub.

### Alocação, liberação e transações

- As páginas de overflow vêm da mesma `acquire_page` do heap: primeiro da lista
  livre (ADR-023), depois do arquivo.
- Ao apagar ou substituir um registro com stub, a cadeia volta para a lista
  livre do heap, no formato de página livre que a lista já usa.
- As páginas são escritas pelo `PageFile` dentro da transação. Por isso entram
  no WAL, são recuperadas depois de uma queda e vão para as réplicas como
  qualquer página. O rollback descarta o que a transação escreveu.
- O MVCC não muda: cada versão é um registro separado, e o `erase` (da
  ADR-024 e do GC) libera a cadeia da versão junto com o slot.

### Compatibilidade

- Heaps sem a flag (bancos anteriores) continuam abrindo e funcionando, com o
  limite antigo de um registro por página.
- Um binário anterior recusa um heap marcado ("unknown flags") em vez de ler um
  stub como se fosse o objeto.
- O `database_check` conhece a página `OVFP` e confere cada cadeia: tipo das
  páginas, tamanhos, ciclos e páginas reclamadas duas vezes.

## Alternativas avaliadas

| Opção | Por que não |
|---|---|
| Guardar objetos grandes no `BlobStore` | o `BlobStore` aloca direto do arquivo, sem lista livre e sem liberar no rollback; e ficaria na camada de objetos, que teria de saber de stubs |
| Um bit no slot | o slot não tem bit livre; o `record_size` só tem o bit alto livre com páginas de até 32 KB |
| Reconhecer o stub pelo conteúdo, sem byte de tipo | um registro comum poderia começar com os mesmos bytes |
| Páginas maiores | só adia o limite e desperdiça espaço com objetos pequenos |

## Consequências

- Não há mais limite de tamanho por objeto no armazenamento. Sobram os limites
  de transporte: um `OpResult` cabe num frame de 16 MiB, e o objeto inteiro é
  montado em memória para ser lido e gravado.
- Cada registro de um heap marcado ocupa 1 byte a mais.
- Ler um objeto grande lê a cadeia inteira. As varreduras (consultas, índices,
  GC) leem todos os registros, então objetos grandes deixam as varreduras mais
  caras.
- O `data_pages_read` (critério TTFR) continua contando só as páginas de dados.
