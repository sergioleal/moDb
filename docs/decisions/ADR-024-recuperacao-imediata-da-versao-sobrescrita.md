# ADR-024 — Recuperação imediata da versão sobrescrita

- Estado: **aceito**
- Data: 2026-09-26
- Medições: [PROFILING_2026-09.md](../../docs-process/PROFILING_2026-09.md), T33.4
- Depende de: [ADR-023](ADR-023-lista-de-paginas-livres-do-heap.md) (sem lista livre, o espaço liberado não seria reusado)

## Contexto

Cada entrada da identidade guarda duas versões: `current` e `previous`
(Fase 6B). `update` grava a versão nova em outro endereço e faz `rebind`:
`current` → `previous`, a nova → `current`. A `previous` que estava lá é
**sobrescrita na identidade**, mas o registro físico continua no heap, órfão,
até que alguém chame `collect_garbage()` — uma varredura do heap inteiro. O
motor nunca chama o GC sozinho; em `crud_full.100k` sem GC o arquivo termina
com 3,8× o tamanho dos dados vivos (T29).

A regra de conflito (`check_snapshot_conflict`) já diz quando é seguro
sobrescrever a `previous`: a escrita só passa se não há snapshot aberto, ou se o
snapshot aberto mais antigo tem época ≥ à época `current` da entrada. Nos dois
casos nenhum snapshot aberto pode resolver para a `previous` antiga (quem a
enxergaria tem época < `current`), e um snapshot aberto depois tem época ≥ à
`current` já commitada. Isso vale mesmo para snapshots abertos por outra thread
durante a transação.

## Decisão

`update` lê a `previous` da entrada antes de escrever e, depois do `rebind`,
**apaga aquele registro na mesma transação** (`ObjectStore::release_overwritten`).
Nada muda no formato, na identidade nem na regra de conflito.

`remove` **não** faz isso. Um objeto removido não volta a ser escrito, então a
`previous` que a remoção sobrescreve é no máximo uma cópia a mais por objeto —
e a própria versão removida já fica para o GC. Liberá-la ali foi medido: o
delete de `crud_full.100k` ficou 2,1× mais lento (páginas de registros grandes
esvaziadas, WAL +36% por operação), sem limitar crescimento nenhum.

Alternativas descartadas:

- **GC periódico** (a cada N commits ou junto do checkpoint): continua sendo
  uma varredura do heap inteiro (~1,1 s a 100k depois da T33.3), com pausa
  proporcional ao banco e não ao trabalho; e ainda deixa o arquivo crescer entre
  execuções.
- **Liberar também no `remove`**: medido e descartado (acima).
- **Liberar também a `current` que vira `previous` quando não há snapshot
  aberto**: um snapshot aberto durante a transação (época = a do último commit)
  precisa exatamente dela. Seria preciso bloquear a abertura de snapshots
  durante escritas; não vale a complexidade.

## Consequências

- Cada objeto vivo tem no máximo **duas versões físicas** (`current` e
  `previous`), com ou sem GC; um removido, no máximo duas até o GC. O
  `collect_garbage()` continua útil para recolher a `previous` referenciada e as
  de objetos removidos, mas deixa de ser necessário para o arquivo não crescer
  sem limite em cargas de update.
- `crud_full.100k` sem GC: arquivo final **107,0 MiB em vez de 159,6** (o
  mesmo que se obtinha chamando o GC entre as fases).
- Custo: um `erase` a mais por `update` que já tinha `previous`, na mesma
  transação. `update_grow` −13% e `update_shrink` −18% em ops/s no motor;
  `update_inplace` (sem `previous` antiga ainda), create, read e delete no
  ruído. É o trabalho que o GC faria depois, pago na hora e sem varredura.
- Rollback desfaz o `erase` junto com o resto; réplicas recebem as páginas pelo
  WAL — nenhum caminho novo.
- Bancos antigos com órfãos acumulados não são limpos por isto: os órfãos já
  não são `previous` de ninguém. Um `collect_garbage()` os recolhe.
