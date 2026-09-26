# ADR-023 — Lista de páginas livres do TableHeap

- Estado: **aceito**
- Data: 2026-09-26
- Medições: [PROFILING_2026-09.md](../../docs-process/PROFILING_2026-09.md), T33.1–T33.2

## Contexto

Quando o `erase` esvazia uma página de dados, o `TableHeap` a retira da cadeia
de páginas (`first` → `next` → … → `last`) e a esquece. O motor não tem lista
de páginas livres: `PageFile::allocate_page` só estende o arquivo, e o código já
registrava "a página física órfã será reaproveitada por um futuro free-page
manager".

O efeito aparece com o GC de versões. Em `crud_full.100k`, cada update grava a
versão nova e mantém a anterior; chamar `collect_garbage()` entre as fases
recolhe as 100 mil versões mortas, mas as páginas que ele esvazia saem da
cadeia e nunca voltam: o arquivo cresceu **exatamente igual** com e sem GC
(41,9 → 79,1 → 144,2 → 159,6 MB).

## Decisão

Cada `TableHeap` mantém a própria lista de páginas livres:

- a raiz `THRP` ganha um campo novo **no fim**, `free_head u64` — a primeira
  página livre, 0 = nenhuma. Raízes gravadas antes têm zero ali, e zero é "lista
  vazia"; a versão do formato não muda;
- a página esvaziada pelo `erase` é regravada como uma `SlottedPage` **vazia e
  válida** cujo `next_page` aponta para a livre seguinte, e vira o topo da lista.
  Não há tipo de página novo: o `database_check` a vê como uma página de heap
  vazia, que é o que ela é;
- quando o heap precisa de uma página (primeira inserção, ou anexar depois de
  `last`), `acquire_page()` desempilha da lista antes de pedir uma nova ao
  arquivo, e confere que a página desempilhada está vazia;
- `repair_table_heap` preserva o `free_head` (ele não é derivável da cadeia de
  dados).

Tudo acontece em páginas da transação: vai para o WAL, é aplicado pelas
réplicas como qualquer outra imagem de página e é desfeito por um rollback.

## Consequências

- Com GC entre as fases de `crud_full.100k`, o arquivo para em **107,0 MB** em
  vez de 159,6 MB; `update_shrink` não cresce nada.
- Sem GC, nenhuma fase muda além do ruído (A/B alternado: −2,2% a +2,5%).
- A lista é por heap: uma página livre do heap de dados não é usada pelo
  `IdentityMap`, pelos índices nem pelo `BlobStore`. Uma lista global no
  `PageFile` exigiria que `allocate_page` fosse transacional, o que hoje não é
  (a alocação é imediata e sobrevive a rollback).
- O arquivo não encolhe: páginas livres continuam ocupando espaço até serem
  reusadas. Devolver espaço ao sistema de arquivos (truncar o fim) fica para
  depois.
- Um arquivo gravado por esta versão abre numa versão anterior do motor: o
  campo novo é ignorado, e as páginas livres parecem páginas órfãs vazias, como
  antes. Nada se perde.
