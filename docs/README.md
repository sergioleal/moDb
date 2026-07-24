# Guia da pasta `docs/`

Este projeto passou por um pivô: nasceu como banco **relacional** e virou o
**Ring0**, um banco **Orientado a Objetos**. O nome `moDb` permanece em
artefatos técnicos existentes, como namespace, CLI e alvo CMake.

Esta pasta reúne a documentação **de funcionalidade** — o que o produto faz e
como usá-lo hoje, independente de quando cada parte foi construída. O
histórico de construção (plano por fases, rastreador de progresso, protocolo
de implementação, relatórios de fechamento, e o curso de treinamento
organizado por fase) vive em [`docs-process/`](../docs-process/README.md).

## Novo por aqui? Comece pelo guia do desenvolvedor

**[DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md)** (em inglês) ensina o produto
passo a passo, do build ao uso avançado (transações, snapshots, consultas,
rede, facades, grafos), com código real citado do repositório. É o ponto de
partida recomendado para quem chega agora.

Quer aprender construindo um projeto próprio, em vez de um tour rápido?
**[training/](training/README.md)** (em inglês, ✅ completo, código real e
verificado) é um curso prático organizado por funcionalidade: você constrói
uma única aplicação —
um diretório de funcionários — do zero, e cada lição soma uma capacidade
nova (persistência, transações, relacionamentos, consultas, rede, operações
remotas, facades, grafos) à mesma base de código.

## 1. Documentos de visão (na raiz do repositório, não em `docs/`)

Não fazem parte desta pasta, mas são o ponto de partida conceitual do
produto:

- **`arquitetura.md`** — o modelo de objetos: identidade, relacionamentos,
  coleções, catálogo-como-objetos, codec genérico, Binding, ProjectionPlan e
  evolução de schema.
- **`codigo-local.md`** — execução de código de domínio dentro do servidor
  (`Operation`, `ExecutionContext`, `OperationRegistry`).
- **`streaming.md`** — streaming assíncrono como modelo nativo de execução de
  consultas (TTFR, coroutines, backpressure).

## 2. Referência por área do produto

Índice geral e definitivo de `docs/`: cada área do produto tem um documento
de referência. Os nove documentos em `reference/` já estão escritos por
completo (✅); um futuro documento novo nesta pasta pode começar como
**esqueleto** (🚧) — links de código real já apontados, prosa ainda por
escrever — até ser preenchido. Não confundir com o
[DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md): aquele é um tutorial narrativo de
ponta a ponta; isto aqui é referência, um documento por área, para consulta
pontual.

| Área | Documento | Status |
|---|---|---|
| Modelo de objetos e catálogo | [reference/object-model.md](reference/object-model.md) | ✅ |
| Ciclo de vida do banco (create/open/options) | [reference/database-lifecycle.md](reference/database-lifecycle.md) | ✅ |
| Transações e WAL | [GARANTIAS_TRANSACIONAIS.md](GARANTIAS_TRANSACIONAIS.md) | ✅ |
| Handles e acesso tipado | [reference/handles.md](reference/handles.md) | ✅ |
| Relacionamentos e coleções | [reference/relationships-collections.md](reference/relationships-collections.md) | ✅ |
| Snapshots e MVCC | [reference/snapshots-mvcc.md](reference/snapshots-mvcc.md) | ✅ |
| Consultas e índices | [reference/queries-indexes.md](reference/queries-indexes.md) | ✅ |
| Rede e protocolo | [reference/networking-protocol.md](reference/networking-protocol.md) | ✅ |
| Operações de domínio remotas | [reference/domain-operations.md](reference/domain-operations.md) | ✅ |
| Facades | [FACADES.md](FACADES.md) | ✅ |
| Grafos | [reference/graphs.md](reference/graphs.md) | ✅ |
| Réplica de leitura e `wal_only` | [OPERACAO_REPLICACAO.md](OPERACAO_REPLICACAO.md) | ✅ |
| I/O assíncrono do WAL | [OPERACAO_IO_ASSINCRONO.md](OPERACAO_IO_ASSINCRONO.md) | ✅ |
| CLI (`modb`) | [USO_DA_CLI.md](USO_DA_CLI.md) | ✅ |
| Formato de arquivo em disco | [FORMATO_DE_ARQUIVO.md](FORMATO_DE_ARQUIVO.md) | ✅ |
| Compatibilidade major/minor | [COMPATIBILIDADE.md](COMPATIBILIDADE.md) | ✅ |
| API pública C++ instalável | [API_PUBLICA.md](API_PUBLICA.md) | ✅ |
| Operação de arquivo (backup/restore) | [OPERACAO.md](OPERACAO.md) | ✅ |
| Modelo de falhas de módulos | [OPERACAO_MODULOS.md](OPERACAO_MODULOS.md) | ✅ |
| Metodologia de benchmarks | [PLANO_BENCHMARKS.md](PLANO_BENCHMARKS.md) | ✅ |
| Fuzzing | [FUZZING.md](FUZZING.md) | ✅ |
| Glossário | [GLOSSARIO.md](GLOSSARIO.md) | ✅ |
| Constituição Ring0 | [CONSTITUTION_RING0.md](CONSTITUTION_RING0.md) | ✅ |
| Decisões arquiteturais | [decisions/](decisions/) (tabela abaixo) | ✅ |

## 3. Decisões arquiteturais — `decisions/`

Registram decisões pontuais e suas justificativas, no formato ADR (Contexto →
Decisão → Consequências). Cada ADR documenta uma decisão de design que
persiste no produto — o cabeçalho pode citar em que fase da construção ela
foi tomada, mas o conteúdo é atemporal.

| ADR | Decisão |
|---|---|
| [ADR-001](decisions/ADR-001-identidade.md) | Identidade (`ObjectId` e demais ids fortes) |
| [ADR-002](decisions/ADR-002-bootstrap-do-catalogo.md) | Bootstrap do catálogo (meta-tipos reservados 1–3) |
| [ADR-003](decisions/ADR-003-tipos-e-encoding.md) | Tipos de atributo e encoding binário dos valores |
| [ADR-004](decisions/ADR-004-pagina-raiz-do-banco.md) | Página raiz do banco (`DBRT`) |
| [ADR-005](decisions/ADR-005-mapa-de-identidade.md) | Mapa de identidade (`IDMD`/`IDMP`) |
| [ADR-006](decisions/ADR-006-destino-do-codigo-relacional.md) | O que fazer com o código relacional existente |
| [ADR-007](decisions/ADR-007-limites-mvp-oo.md) | Limites do MVP OO |
| [ADR-008](decisions/ADR-008-integridade-de-referencias.md) | Integridade de referências e cascata de composição |
| [ADR-010](decisions/ADR-010-protocolo-binario-proximo-do-armazenamento.md) | Protocolo binário próximo do armazenamento lógico, sem expor localização física |
| [ADR-011](decisions/ADR-011-concorrencia-do-servidor.md) | Modelo de concorrência do servidor (leitor, workers, escritor, fila limitada) |
| [ADR-012](decisions/ADR-012-runtime-de-modulos-no-processo.md) | Interface por métodos C++, consultas internas e módulos confiáveis no processo |
| [ADR-014](decisions/ADR-014-catalogo-de-facades-e-handles.md) | Catálogo de facades, `FacadeHandle` tipado e descoberta/negociação |
| [ADR-016](decisions/ADR-016-replica-de-leitura-por-streaming-do-wal.md) | Réplica de leitura read-only por streaming do WAL durável |
| [ADR-017](decisions/ADR-017-primary-wal-only-sem-arquivos-de-dados.md) | Primary `wal_only`: só WAL; arquivos de dados nas réplicas |
| [ADR-018](decisions/ADR-018-handles-de-arestas-e-algoritmos-de-grafos.md) | `EdgeHandle` tipado, snapshot e algoritmos básicos de grafos |
| [ADR-019](decisions/ADR-019-io-assincrono.md) | I/O assíncrono posicional com backpressure |

### ADRs legadas (modelo relacional, `0001`/`0002`)

- [0001-formato-de-armazenamento.md](decisions/0001-formato-de-armazenamento.md)
  e [0002-tipos-e-erros.md](decisions/0002-tipos-e-erros.md).
- **Parcialmente supersedidas**: cada uma tem um aviso no topo dizendo qual
  parte ainda vale. O que sobrevive ao pivô é a camada física (página de 4096
  bytes, little-endian, sem cópia direta de struct, política de erros via
  `Result`/`std::expected`) — o storage reaproveitado pelo Ring0 (ver
  [ADR-006](decisions/ADR-006-destino-do-codigo-relacional.md)). O que não
  sobrevive são os tipos SQL e os metadados relacionais, superados por
  ADR-003/004/005.

## 4. Glossário

- **[GLOSSARIO.md](GLOSSARIO.md)** — termos do modelo OO vigente (Objeto,
  ObjectId, Handle, TypeDefinition, Binding, ProjectionPlan, Snapshot, TTFR
  etc.), seguidos pelos termos gerais de armazenamento e, por fim, uma seção
  separada com os termos relacionais legados, mantida só para quem for ler
  documentação histórica.

## Histórico de construção

Como o produto foi construído, fase a fase — plano, rastreador de progresso,
protocolo de implementação, relatórios de fechamento e o curso de treinamento
em inglês: **[docs-process/README.md](../docs-process/README.md)**.
