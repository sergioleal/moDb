# ADR-027 — Concorrência: leitores em paralelo, um escritor exclusivo

- Estado: aceito
- Data: 2026-09-27
- Relacionados: ADR-009 (MVCC por objeto), ADR-011 (concorrência do servidor), ADR-025 (servidor com procs), docs-process/PLANO_CONCORRENCIA.md (C9)

## Contexto

O motor não era seguro entre threads: o caminho de leitura escrevia em estado
compartilhado sem lock (LRU do `BufferPool`, o `view_scratch_` do `PageFile`,
o cache `peeked_` do `ObjectStore`, os planos de projeção, contadores), e o
servidor de rede serializava tudo num `engine_mutex_`. Com o servidor de
procs (ADR-025), isso vira uma chamada por vez para o banco inteiro.

A linha de base no ThreadSanitizer (C4) confirmou o diagnóstico do plano e
achou uma corrida a mais, em `NativeSocket::close`.

O plano previa, para o objetivo final, leitores concorrentes **com** um
escritor em andamento: escrita privada até o commit, latch de aplicação e uma
visão de leitura imutável trocada a cada commit (C7/C8). Ao detalhar a C8, a
cópia da visão se mostrou cara: o `TableHeap` guarda estruturas do tamanho do
banco (o conjunto de páginas e o índice de capacidade), e copiá-las a cada
commit custaria milissegundos por commit a 1M objetos.

## Decisão

Em duas etapas.

**Etapa 1 (feita): leitores em paralelo, escritor exclusivo.**

1. **Caminho de leitura seguro entre threads (C6).** `BufferPool` com mutex e
   frames `shared_ptr<const Page>` imutáveis: `put` troca o ponteiro, despejo
   só solta a referência, então quem leu continua com a versão que leu.
   `PageFile::view` devolve `PageRef` (mantém a página viva) no lugar do
   ponteiro cru. `peeked_` virou por thread; planos de projeção sob
   `shared_mutex`; contadores atômicos; `ScratchPagePool` com mutex.
2. **Lock de leitores e escritor no `Database` (C10).** Leituras de qualquer
   thread tomam o lock compartilhado por chamada (um stream, por item, nunca
   entre itens: `Database::guarded`). Uma escrita -- transação, bind,
   create_index, GC, checkpoint -- toma o exclusivo do `begin` até o fim; o
   `begin` de outra thread **espera a vez** (antes: `transaction_active`). A
   thread escritora lê o próprio estado sem travar. O lock é reentrante por
   thread para leituras aninhadas, e escrever no meio de uma leitura da mesma
   thread é erro, não deadlock.
3. **Servidor sem lock global (C11).** `engine_mutex_` removido: procs de
   leitura e consultas correm em paralelo; procs de escrita, uma por vez, pelo
   `begin`. Uma proc de leitura usa o snapshot da chamada em tudo (`read`,
   `where`/`all`, `find` -- `Database::query(snapshot)`), para ver um estado
   só mesmo com commits de outras chamadas no meio.

**Etapa 2 (condicional): leitores durante uma escrita em andamento (C7/C8).**
Só se a medição (C12, máquina dedicada) mostrar que o escritor exclusivo
limita a vazão -- por exemplo, leituras esperando transações longas. O
desenho continua o do plano, com uma mudança: em vez de copiar o
`ObjectStore` inteiro a cada commit, separar dele o pequeno estado que os
leitores usam (raízes, contadores, catálogo, índices) numa visão publicada, e
deixar as estruturas grandes do `TableHeap` só para o escritor.

## Por que não MVCC de páginas nem multi-writer

Como no plano: o MVCC por objeto (ADR-009) já dá o isolamento lógico, e
multi-writer exigiria gerenciador de locks e WAL com vários escritores. Fica
fora.

## Consequências

- Várias threads (e várias procs de leitura no servidor) leem ao mesmo tempo,
  sem lock na aplicação.
- Uma transação longa bloqueia as leituras novas enquanto dura. Transações
  devem ser curtas -- no servidor de procs, uma chamada.
- O lock é o `RwLock` do motor, não o `std::shared_mutex`: no MinGW o rwlock
  da winpthreads falhava sob contenção (SRWLOCK no lugar), e no Linux o
  rwlock padrão da glibc prefere leitores e deixava o escritor esperando para
  sempre com leitores contínuos (preferência ao escritor no lugar). Com
  preferência ao escritor, leituras novas esperam uma escrita pendente -- é o
  preço de a escrita não morrer de fome.
- Contrato de API (C10.1): leituras e snapshots de qualquer thread; uma
  transação por vez, terminada na thread que a abriu; DDL exclusiva;
  `Handle`/`Snapshot` usáveis de qualquer thread, mas uma `Transaction` não
  deve mudar de thread; não abrir transação dentro do visitor de `scan`.
- **Snapshot e lock, nessa ordem não.** Um snapshot aberto que espera o lock de
  leitura atrás de escritores fica mais velho que o último commit, e a regra de
  duas versões (ADR-009) faz as escritas de um objeto disputado baterem em
  `snapshot_conflict`. O padrão é tomar `read_guard()` antes de abrir o
  snapshot e segurá-lo pela leitura -- o `OperationRegistry` faz isso em toda
  proc de leitura. `transact` ainda repete os conflitos que sobram (com dreno:
  os escritores que chegam esperam os snapshots antigos fecharem antes de pedir
  o lock).
