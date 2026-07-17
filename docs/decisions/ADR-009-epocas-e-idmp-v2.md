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
