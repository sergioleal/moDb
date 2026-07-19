# Glossário

> O produto é um banco Orientado a Objetos (ODB++). Os termos OO abaixo são os
> vigentes; os termos relacionais ao final são legados, mantidos para leitura do
> histórico. Ver [PLANO_ODB.md](PLANO_ODB.md).

## Modelo Orientado a Objetos

- **Objeto**: unidade persistente do banco; possui identidade, tipo e atributos.
- **ObjectId**: identidade permanente de um objeto (u64, monotônico, nunca
  reutilizada). Não muda mesmo que o objeto mude de página.
- **Handle**: referência leve a um objeto (`DatabaseId` + `ObjectId`);
  representa apenas identidade, nunca contém o objeto.
- **TypeDefinition**: definição imutável de um tipo (nome, atributos,
  relacionamentos, índices). Uma nova versão do tipo cria uma nova definição.
- **AttributeDefinition**: definição de um atributo (FieldId, nome, tipo,
  nullable, default, se é coleção/embedded/owned).
- **FieldId**: identificador estável de um atributo dentro de um tipo (u16).
- **Baseline**: snapshot estrutural completo e imutável do catálogo (conjunto
  de TypeDefinitions).
- **Binding**: ligação, na aplicação atual, entre FieldIds e membros de uma
  classe C++. Existe apenas um binding por tipo por versão da aplicação.
- **Codec genérico**: único codec binário; interpreta qualquer objeto a partir
  do catálogo, sem conhecer classes C++.
- **ProjectionPlan**: plano que reconcilia a TypeDefinition persistida com o
  binding atual (copy, convert, default, ignore, resolve_reference); cacheado
  por tipo.
- **Migração preguiçosa**: objeto antigo permanece como está e só é regravado
  com a definição atual quando modificado.
- **Ref / OwnedRef / Embedded**: associação (referência por ObjectId),
  composição (referência com remoção em cascata) e objeto sem identidade
  serializado no pai.
- **BlobStore**: armazenamento de dados grandes (textos, binários, coleções) em
  páginas encadeadas; o objeto guarda apenas o `BlobId`.
- **PersistentVector/Set/Map**: coleções com armazenamento próprio (via blob),
  fora do payload do objeto.
- **Snapshot**: visão consistente do banco numa época; leituras de um snapshot
  não enxergam escritas posteriores.
- **Operation**: unidade transacional de código de domínio executada dentro do
  servidor (ex.: `TransferFunds`).
- **ExecutionContext**: única porta de entrada de uma Operation para o banco
  (transação, objetos, log); nunca expõe páginas ou WAL.
- **Facade**: agrupamento descobrível de operações de domínio sob um
  `FacadeId` e versão; o catálogo é um vetor de descritores cuja posição não
  é identidade (Fase 11, ADR-014).
- **FacadeHandle**: handle tipado do consumidor (sessão + `FacadeId` + versão
  negociada) que invoca métodos daquela facade via `invoke<Method>(...)`,
  delegando ao `OperationRegistry`.
- **Aresta**: relacionamento direcionado entre dois objetos com identidade;
  persiste como `Ref<T>` ou `OwnedRef<T>`.
- **EdgeHandle**: visão runtime tipada de uma aresta (`DatabaseId`, origem,
  alvo e `FieldId`); não é persistida e resolve extremidades sob `Snapshot`.
- **GraphView**: provedor de adjacência que fixa snapshot, direção, política
  para refs órfãs, inclusão de ownership e limites da travessia.
- **Grafo não direcionado**: view que interpreta cada relacionamento nos dois
  sentidos; não altera a direção persistida da `Ref`.
- **Primary**: instância única escritora do banco; único produtor de commits.
- **Follower / Réplica de leitura**: instância read-only que aplica o WAL do
  primary em arquivo local próprio e serve apenas leituras (Fase 14, ADR-016).
- **LSN**: *Log Sequence Number* — posição global monotônica e persistente de
  um registro no WAL v2; `commit_lsn` marca a fronteira replicável de uma
  transação. No WAL v1 o `lsn` era local ao arquivo e reiniciava por sessão.
- **DatabaseUuid / timeline_id**: identidade persistente do banco e da linha do
  tempo; o follower usa ambos para confirmar que a origem não divergiu.
- **Bootstrap**: cópia inicial de um snapshot base consistente do primary
  (sob barreira do escritor) a partir da qual o follower começa a aplicar o WAL.
- **Lag de replicação**: distância entre `applied_lsn` do follower e
  `primary_commit_lsn`, em bytes, commits ou tempo.
- **Gap**: LSN pedido pelo follower já descartado pela retenção do primary
  (`WalGap`); exige novo bootstrap, nunca salto silencioso.
- **TTFR**: *Time To First Result* — tempo até o primeiro objeto de uma
  consulta; principal métrica de desempenho do streaming.
- **Streaming**: modelo nativo de execução de consultas; resultados fluem assim
  que produzidos, com memória O(1) e backpressure até o storage.

## Termos gerais de armazenamento (vigentes)

- **Página**: bloco de tamanho fixo (4096 bytes) para leitura e escrita no
  arquivo.
- **PageId**: identificador lógico e estável de uma página.
- **Superbloco**: página zero com identidade, versão, tamanho de página e a
  raiz do banco (`catalog_root`).
- **Slotted page**: página que armazena registros de tamanho variável com um
  diretório de slots.
- **TableHeap**: cadeia de páginas que armazena registros; base física do
  ObjectStore.
- **Mapa de identidade**: tradução persistente `ObjectId → (página, slot)`.
- **Buffer pool**: cache em memória das páginas lidas do arquivo.
- **WAL**: log gravado e sincronizado antes das páginas de dados, para permitir
  recuperação atômica e durável.
- **Scan**: varredura sequencial dos objetos armazenados.
- **Executor**: componente que executa os operadores de um plano de consulta.

## Termos relacionais (legados)

> Vocabulário do modelo relacional abandonado no pivô. Mantido apenas para
> leitura do histórico e dos documentos supersedidos.

- **AST**: árvore que representa a estrutura sintática de um comando SQL.
- **Binder**: etapa que resolvia nomes de tabelas e colunas.
- **Catálogo (relacional)**: metadados sobre tabelas, colunas e outros objetos.
  No modelo OO, o catálogo é composto por objetos.
- **Plano lógico**: representação das operações relacionais de uma consulta.
- **RowId**: localização lógica de uma linha; substituído por ObjectId +
  mapa de identidade.
- **MVP (relacional)**: menor versão que comprovava o fluxo persistente
  relacional; substituído pelo MVP OO (Fases 0–3 do PLANO_ODB).
