# ADR-017 — Primary `wal_only`: só WAL, dados nas réplicas de leitura

- Estado: aceito para a Fase 15
- Data: 2026-07-19

## Contexto

A Fase 14 (ADR-016) replica o WAL de um primary que ainda mantém o arquivo de
dados local. Em muitos cenários o papel do escritor é só **produzir commits
duráveis e transmiti-los**; o estado de páginas (arquivo de dados) deve viver
nas instâncias de leitura, que já aplicam o stream.

Isso alinha o produto ao Princípio V (Disaster Recovery) da
[CONSTITUTION_RING0.md](../CONSTITUTION_RING0.md): o mecanismo primário de DR
é streaming contínuo de commits para réplicas em domínio de falha externo. O
primary `wal_only` reduz a superfície local do escritor (só o log) e deixa as
réplicas como donas dos arquivos de dados.

## Decisão

**A Fase 15 introduz um parâmetro de instância no primary de escrita que
desliga a criação/manutenção dos arquivos de dados.** Nesse modo o primary
persiste apenas o WAL (e metadados mínimos de identidade/controle do log) e
replica para as instâncias de leitura, que mantêm e aplicam nos arquivos de
dados.

### 1. Parâmetro de instância

- Nome de configuração (contrato): `primary_storage` com valores
  `full` (padrão, comportamento da Fase 14) e `wal_only`.
- Equivalentes de superfície: flag CLI / opção de abertura
  (`--primary-storage=wal_only`) e, se houver env, mapeamento explícito
  documentado — sem inferência silenciosa.
- O parâmetro só é válido na **instância primary de escrita**. Follower
  read-only (Fase 14) sempre mantém arquivos de dados; configurar
  `wal_only` nele é erro (`invalid_instance_config`).
- `full` continua suportado: um único processo com dados + WAL permanece o
  caminho embutido e o default.

### 2. O que o primary `wal_only` faz e não faz

- **Faz:** aloca identidade (`DatabaseUuid` / `timeline_id`), produz
  registros de WAL v2 com LSN global e `commit_lsn`, faz fsync do WAL,
  serve o canal privilegiado de replicação, retém segmentos conforme ACK
  das réplicas que possuem dados.
- **Não faz:** criar, abrir para escrita durável nem checkpointar o arquivo
  de páginas/dados do banco; não é fonte de bootstrap por cópia de arquivo
  local (não há snapshot de dados no primary).
- Páginas/after-images necessárias para montar o WAL podem existir só em
  memória (ou scratch não durável). Crash do primary `wal_only` recupera o
  log; o estado de páginas vive nas réplicas.

### 3. Papel das réplicas de leitura

- Continuam read-only para clientes (ADR-016).
- São as **únicas donas** dos arquivos de dados nesse modo: aplicam o WAL,
  fazem flush das páginas e expõem leituras.
- Pelo menos uma réplica com dados é necessária para o sistema ter estado
  consultável; o primary sozinho não serve leituras de objetos persistidos.

### 4. Durabilidade e visibilidade de commit

- Commit no primary `wal_only` torna-se durável no log quando o WAL local é
  sincronizado; a política de **quando a escrita é reconhecida ao cliente**
  (só WAL vs. aguardar ACK de N réplicas com dados) é configurável e
  documentada na Fase 15 — default conservador: exigir ACK de pelo menos uma
  réplica de dados antes de confirmar ao cliente, para não declarar durável
  um estado que só existe no log sem apply externo.
- Retenção de segmentos no primary governa-se pelos ACKs das réplicas de
  dados (e pela política de retenção da Fase 14B), não por checkpoint de
  páginas locais inexistentes.

### 5. Bootstrap e seed

- Sem arquivo de dados no primary, o bootstrap da Fase 14 por “copiar o
  arquivo do primary” não se aplica.
- Seed inicial: (a) réplica nova parte de arquivo vazio compatível com
  UUID/timeline e aplica o WAL desde o LSN 1 / origem; ou (b) uma réplica
  já populada doa snapshot base a outra réplica (canal privilegiado entre
  réplicas ou ferramenta operacional), nunca exigindo dados no primary.
- Gap/`WalGap` e rebootstrap seguem ADR-016, com a fonte de snapshot sendo
  uma réplica de dados, não o primary `wal_only`.

### 6. Fora do escopo desta fase

- Promoção automática do follower a primary escritor, eleição, multi-writer.
- Primary sem WAL (só memória) — o log local permanece obrigatório.
- Compartilhar volume de dados entre primary e réplicas.

## Consequências

- Separação clara: escritor = log + stream; leitores = arquivos de dados.
- Alinha DR ao streaming externo (constituição); o primary fica mais leve em
  disco.
- Bootstrap e operação mudam: documentação e CLI precisam distinguir
  `primary_storage=full` vs `wal_only`.
- Custo: o primary `wal_only` sozinho não reconstrói o banco de páginas;
  perda de todas as réplicas de dados + retenção insuficiente do WAL é
  perda de estado — fail loud, nunca silent.
- Novos erros: `invalid_instance_config`, `data_files_disabled`,
  `no_data_replica`, `commit_await_replica_timeout` (nomes finais no mapa
  da Fase 15).
- A ADR-016 permanece válida para o modo `full`; esta ADR acrescenta o modo
  `wal_only` na Fase 15.
