# Guia da pasta `docs/`

Este projeto passou por um pivô: nasceu como banco **relacional** e virou um
banco **Orientado a Objetos** (ODB++). Os documentos abaixo misturam material
vigente (OO) com material legado (relacional, mantido por histórico). Este
guia explica como eles se relacionam, para não ler o arquivo errado como se
fosse atual.

## Linha do tempo em uma frase

```text
Visão (3 MDs na raiz) → Plano OO → Protocolo por fase → ADRs da Fase 0
                                                              ↑
                              (substituem o Plano/Escopo/ADRs relacionais)
```

## Quer saber só "onde estamos agora"?

**[RASTREADOR.md](RASTREADOR.md)** — o rastreador de andamento. Lista as
~103 tarefas das 11 fases com status (`⬜`/`🔄`/`✅`/`🚫`), o teste automatizado
de cada fase e o painel geral de progresso. É o único documento desta pasta
que reflete estado vivo; os demais (Plano, Protocolo, ADRs) definem escopo e
não mudam a cada tarefa concluída.

## 1. Documentos de visão (na raiz do repositório, não em `docs/`)

Não fazem parte desta pasta, mas são o ponto de partida de tudo que está aqui:

- **`arquitetura.md`** — o modelo de objetos: identidade, relacionamentos,
  coleções, catálogo-como-objetos, codec genérico, Binding, ProjectionPlan e
  evolução de schema.
- **`codigo-local.md`** — execução de código de domínio dentro do servidor
  (`Operation`, `ExecutionContext`, `OperationRegistry`).
- **`streaming.md`** — streaming assíncrono como modelo nativo de execução de
  consultas (TTFR, coroutines, backpressure).

Esses três documentos descrevem *o que* o banco deve ser. Tudo em `docs/`
descreve *como* chegar lá.

## 2. Plano e protocolo (vigentes) — comece por aqui

- **[PLANO_ODB.md](PLANO_ODB.md)** — o plano de desenvolvimento vigente.
  Traduz os três documentos de visão em **11 fases verticais** (0 a 10), cada
  uma com objetivo, tarefas, entregáveis e critério de aceite. Define o MVP OO
  (fases 0–3) e a ordem recomendada de execução.
- **[PROTOCOLO_FASES.md](PROTOCOLO_FASES.md)** — o mesmo plano, mas no nível de
  execução: para cada fase do `PLANO_ODB.md`, especifica os arquivos a criar,
  os layouts binários byte a byte, as assinaturas de API e os testes
  automatizados caso a caso. É o documento que uma pessoa implementando uma
  fase deve ter aberto.

Relação entre os dois: `PLANO_ODB.md` é o "o quê e por quê" (nível de
gerência/arquitetura); `PROTOCOLO_FASES.md` é o "como", fase a fase (nível de
implementação). As fases e a numeração são as mesmas nos dois documentos.

- **[USO_DA_CLI.md](USO_DA_CLI.md)** — referência de uso da CLI **atual**
  (`demo`, `db`, `page`, `record`, `heap`, `codec`, `catalog`). É a CLI do
  modelo relacional legado, ainda a única que existe até a Fase 2 introduzir
  os comandos OO; será revisado quando isso acontecer.

## 3. Decisões arquiteturais — `decisions/`

Registram decisões pontuais e suas justificativas, no formato ADR (Contexto →
Decisão → Consequências). Duas gerações:

### ADRs vigentes (pivô OO, `ADR-001` a `ADR-007`)

Detalham decisões que o `PROTOCOLO_FASES.md` pressupõe como já tomadas —
principalmente as da **Fase 0**:

| ADR | Decisão |
|---|---|
| [ADR-001](decisions/ADR-001-identidade.md) | Identidade (`ObjectId` e demais ids fortes) |
| [ADR-002](decisions/ADR-002-bootstrap-do-catalogo.md) | Bootstrap do catálogo (meta-tipos reservados 1–3) |
| [ADR-003](decisions/ADR-003-tipos-e-encoding.md) | Tipos de atributo e encoding binário dos valores |
| [ADR-004](decisions/ADR-004-pagina-raiz-do-banco.md) | Página raiz do banco (`DBRT`) |
| [ADR-005](decisions/ADR-005-mapa-de-identidade.md) | Mapa de identidade (`IDMD`/`IDMP`) |
| [ADR-006](decisions/ADR-006-destino-do-codigo-relacional.md) | O que fazer com o código relacional existente |
| [ADR-007](decisions/ADR-007-limites-mvp-oo.md) | Limites do MVP OO |

### ADRs legadas (modelo relacional, `0001`/`0002`)

- [0001-formato-de-armazenamento.md](decisions/0001-formato-de-armazenamento.md)
  e [0002-tipos-e-erros.md](decisions/0002-tipos-e-erros.md).
- **Parcialmente supersedidas**: cada uma tem um aviso no topo dizendo qual
  parte ainda vale. Em resumo, o que sobrevive ao pivô é a camada física
  (página de 4096 bytes, little-endian, sem cópia direta de struct, política
  de erros via `Result`/`std::expected`) — isso é justamente o storage
  reaproveitado pelo ODB++ (ver [ADR-006](decisions/ADR-006-destino-do-codigo-relacional.md)).
  O que não sobrevive são os tipos SQL e os metadados relacionais, superados
  por ADR-003/004/005.

## 4. Documentos supersedidos (modelo relacional abandonado)

Mantidos apenas para histórico — cada um tem um aviso ⚠️ no topo apontando
para o documento vigente que o substitui:

- **[PLANO_DE_DESENVOLVIMENTO.md](PLANO_DE_DESENVOLVIMENTO.md)** — plano de
  marcos do banco relacional. Substituído por `PLANO_ODB.md`.
- **[ESCOPO_MVP.md](ESCOPO_MVP.md)** — escopo do MVP relacional
  (`CREATE TABLE`/`INSERT`/`SELECT`). Substituído pelo MVP OO (fases 0–3 do
  `PLANO_ODB.md`) e pelos limites da [ADR-007](decisions/ADR-007-limites-mvp-oo.md).
- **`../README.md`** (raiz do repositório) — também supersedido; será
  reescrito na Fase 10.

**`FORMATO_DE_ARQUIVO.md` não tem aviso de supersedido** porque descreve o
formato físico de página que continua em uso (superbloco, little-endian,
slotted page) — mas não cobre as páginas novas do modelo OO (`DBRT`, `IDMD`,
`IDMP`, `BLBP`, `BTIN`/`BTLF`, WAL). Esse documento será reescrito na Fase 10
do `PLANO_ODB.md`; até lá, o mapa de páginas do modelo OO vive só no Apêndice B
do [PROTOCOLO_FASES.md](PROTOCOLO_FASES.md#apêndice-b--mapa-de-páginas-do-formato).

## 5. Glossário

- **[GLOSSARIO.md](GLOSSARIO.md)** — termos do modelo OO vigente (Objeto,
  ObjectId, Handle, TypeDefinition, Binding, ProjectionPlan, Snapshot, TTFR
  etc.), seguidos pelos termos gerais de armazenamento que continuam válidos
  e, por fim, uma seção separada com os termos relacionais legados (AST,
  Binder, RowId...) mantida só para quem for ler os documentos supersedidos.

## Qual documento ler primeiro?

- **Quer entender a visão do produto?** Leia os três MDs da raiz
  (`arquitetura.md`, `codigo-local.md`, `streaming.md`).
- **Quer saber o status atual, o que já foi feito?** `RASTREADOR.md`.
- **Quer saber o que fazer agora e em que ordem?** `PLANO_ODB.md`.
- **Vai implementar uma fase?** `PROTOCOLO_FASES.md`, seção da fase; consulte
  a ADR correspondente para o porquê de cada decisão.
- **Precisa entender um termo?** `GLOSSARIO.md`.
- **Leu algo sobre tabelas, SQL ou `Catalog`/`Schema`/`Row`?** Você caiu em um
  documento supersedido — confira o aviso no topo e vá para o equivalente OO.
