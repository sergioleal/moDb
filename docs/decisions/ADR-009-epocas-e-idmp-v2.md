# ADR-009 — Épocas e IDMP v2

## Contexto

A Fase 6 introduz snapshots e MVCC. O mapa de identidade precisa conservar a
localização atual e uma localização anterior, e o banco precisa de uma época
global monotônica para ordenar commits.

## Decisão

- O `DBRT` passa à versão 2 e persiste `epoch` como `u64`, inicialmente zero.
  Cada commit avança a época dentro da mesma transação protegida pelo WAL.
- `IDMP` passa à versão 2. Cada entrada ocupa 48 bytes:
  `current(page,slot,generation,flags)`, `current_epoch`,
  `previous(page,slot,generation,flags)`, `previous_epoch`.
- O tamanho de 48 bytes é deliberado. Duas localizações com `PageId u64`
  exigem 24 bytes; as duas épocas exigem mais 16. Os 8 bytes restantes mantêm
  flags explícitas e alinhamento estável, sem compactar tombstones.
- Na primeira abertura de um IDMP v1, o banco regrava as entradas para páginas
  IDMP v2 novas e só então publica a nova raiz no DBRT. Uma queda anterior à
  publicação deixa a raiz v1 íntegra e permite repetir a migração.
- A 6A apenas prepara o formato: campos `previous` e épocas por entrada são
  inicializados, mas snapshots, retenção e GC ficam para 6B/6C.

## Consequências

O IDMP v2 armazena menos entradas por página do que o v1, mas preserva todos os
ObjectIds, RecordIds e tombstones durante a migração. Um DBRT v1 é aceito uma
vez, recebe época zero e é regravado como v2; DBRT/IDMP v2 reabrem sem nova
migração.

## Adendo (Fase 6B) — snapshots sobre o formato versionado

A 6B consome o formato preparado pela 6A. Decisões concretizadas:

- **Visibilidade por época.** `IdentityMap::find_at(id, e)` resolve `current` se
  `current_epoch ≤ e` e o objeto não foi removido nessa época; senão `previous`
  se `previous_epoch ≤ e`; senão `record_not_found`. `find` (sem época) continua
  resolvendo só o `current`.
- **Escrita move `current → previous`.** `bind`/`rebind`/`erase` recebem a época
  do commit e são puramente mecânicos no IdentityMap: o `current` antigo desce
  para `previous` e o novo `current` é carimbado com a época. A época usada é
  `store.epoch() + 1` — determinística porque o modelo é single-writer e nenhuma
  outra transação se intercala entre a escrita e o commit.
- **Limite de uma versão anterior ⇒ `snapshot_conflict`.** Como só há uma posição
  `previous`, `ObjectStore` recusa (antes de qualquer escrita) uma segunda
  alteração do mesmo objeto quando existe snapshot aberto com época
  `< current_epoch` **e** a `previous` já está ocupada. A regra vive em
  `ObjectStore::check_snapshot_conflict()`; o registro de épocas abertas é o
  `std::multiset` mantido pelo `Database` via `Snapshot` RAII.
- **Preservação física.** O `TableHeap::update()` reusa o slot in-place, o que
  destruiria os bytes de que a `previous` depende. Por isso `ObjectStore::update()`
  **sempre** insere um registro físico novo e `ObjectStore::remove()` **não**
  chama `data_heap_.erase()` — apenas o tombstone lógico na identidade. `scan`/
  `scan_at` filtram cada registro físico contra a identidade resolvida para não
  enumerar cópias antigas.
- **Escopo adiado.** A recuperação do espaço ocupado pelos registros físicos
  preservados (retenção por época + GC) é explicitamente da 6C. A 6B aceita o
  crescimento de espaço como desvio consciente do MVP.

## Adendo (Fase 6C) — retenção, coleta de lixo e concorrência

A 6C recicla o espaço que a 6B preservava, mantendo a retenção correta:

- **GC por reconciliação.** `Database::collect_garbage()` (transação própria,
  via WAL) varre o heap de dados e, para cada registro, consulta
  `IdentityMap::inspect(id)` — que devolve `current`/`previous` e suas épocas
  mesmo para um tombstone. A `current` viva nunca é tocada; a `previous`
  referenciada é liberada quando `oldest_open_snapshot_epoch >= current_epoch`
  (nenhum snapshot aberto ainda a vê); qualquer registro que não seja `current`
  nem a `previous` referenciada é órfão e é sempre liberado.
- **Compactação da entrada.** Liberar a `previous` referenciada chama
  `IdentityMap::clear_previous(id)`, que zera só o slot anterior. Como o
  `ObjectId` nunca é reusado (ADR-001), um tombstone sem `previous` permanece
  alocado, apenas sem versões físicas.
- **Regra de coletabilidade = espelho do conflito.** A condição de reciclagem
  (`OOSE >= current_epoch`) é a mesma, invertida, da recusa de `snapshot_conflict`
  da 6B: enquanto um snapshot mais antigo que a época `current` estiver aberto, a
  `previous` é retida e uma segunda alteração conflita; quando esse snapshot
  fecha, a `previous` é reciclável e uma nova alteração volta a caber.
- **Sem free list geral; sem GC automático.** A reciclagem reusa
  `TableHeap::erase` (compacta a página, retira páginas vazias). O GC é
  explícito (não roda em todo commit nem no destrutor do `Snapshot`, que
  permanece livre de I/O), o que mantém o commit barato; o custo por coleta é
  `O(registros do heap)`. Otimizar com um índice de reclamação por época fica
  para a Fase 10.
- **Concorrência.** Commits seguem serializados pela guarda single-writer de
  `begin()`. O registro de épocas de snapshots abertos é protegido por um mutex
  curto, sincronizando leitor (abre/fecha snapshot) e escritor (escrita/GC) sem
  bloquear a duração das leituras.
