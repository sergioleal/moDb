# ADR-014 — Catálogo de facades e handles de invocação

- Estado: aceito para a Fase 11
- Data: 2026-07-19

## Contexto

A Fase 9 estabeleceu operações de domínio tipadas (`Operation`,
`OperationRegistry`, `client.call<Op>(...)`) e o contrato transacional sob
`ExecutionContext`. O consumidor, porém, ainda enxerga um conjunto plano de
identificadores de operação (`"account.transfer"`, `"finance.close-month"`),
sem agrupamento por superfície de API nem um handle estável que represente
“esta fachada, nesta versão, nesta sessão”.

Aplicações tipicamente organizam regras em **facades** (contas, pedidos,
faturamento). Sem um catálogo descobrível:

- o cliente precisa conhecer cada id de operação a priori;
- não há negociação explícita de versão/capacidades da superfície;
- não há um objeto de sessão que valide “este método pertence a esta facade”;
- a evolução de um conjunto coerente de métodos fica dispersa.

A Fase 9 permanece a base de execução. A Fase 11 acrescenta a camada de
descoberta e invocação por facade, sem substituir o registry nem o codec.

## Decisão

**A Fase 11 introduz um catálogo heterogêneo de facades e um handle tipado
de invocação para o consumidor.**

1. **Catálogo em memória como vetor de descritores.** O servidor mantém
   `std::vector<FacadeDescriptor>`. Cada descriptor contém:
   - `FacadeId` estável (string ou id forte, nunca a posição no vetor);
   - versão da facade;
   - modo agregado (`read_only` / `read_write` / misto documentado);
   - vetor de métodos exportados (`MethodDescriptor`: id de operação,
     assinatura lógica, modo `read_only`/`read_write`).
   A ordem no vetor é apenas de enumeração; a identidade pública é o
   `FacadeId` (+ versão).

2. **Separação de responsabilidades com a Fase 9.**
   - Fase 9: `OperationRegistry`, despacho, commit/rollback, módulos,
     `client.call<Op>(...)`.
   - Fase 11: agrupa operações em facades descobríveis e oferece
     `FacadeHandle<TFacade>` ao consumidor. O handle resolve o método e
     delega ao registry (ou ao equivalente de protocolo `OpCall`).

3. **Handle de consumidor.** `FacadeHandle<TFacade>` guarda conexão/sessão,
   `FacadeId` e versão negociada. Expõe `invoke<Method>(args...)` (ou
   métodos tipados gerados/wrappers). Argumentos e resultados usam o codec
   versionado (ADR-003); nenhum ponteiro, referência ou layout C++ trafega
   pela rede.

4. **Descoberta e negociação.** O cliente pode listar facades/métodos e
   obter um handle somente após negociar versão compatível. Métodos
   invocados devem pertencer à facade do handle; caso contrário,
   `facade_method_not_found`. Facade ausente → `facade_not_found`; versão
   incompatível → `incompatible_facade_version`.

5. **Cancelamento e deadline.** Invocações via handle respeitam o mesmo
   modelo de cancelamento/deadline da rede (Fase 8) e o contrato
   transacional da Fase 9.

6. **Sem sandbox adicional.** Continua valendo a ADR-012: módulos no
   processo, origem confiável, sem isolamento por worker nesta fase.

## Consequências

- O consumidor passa a obter um handle e invocar métodos daquela facade,
  em vez de apenas ids soltos de operação.
- O registry da Fase 9 continua sendo o executor; a facade é agrupamento e
  contrato de superfície, não um segundo motor de execução.
- Descoberta e versionamento de facade tornam a API de domínio evolutiva
  sem alterar o formato físico do banco.
- Erros novos (`facade_not_found`, `facade_method_not_found`,
  `incompatible_facade_version`) entram no mapa de ErrorCodes da Fase 11.
- A implantação serverless (agora Fase 13) pode assumir que a superfície
  pública de domínio já inclui facades e handles, além de `client.call`.
- Persistência de facades/handles como objetos de banco fica fora do
  escopo; o catálogo é runtime (e, se necessário, derivado do manifesto
  dos módulos carregados).

## Complemento Fase 11D

- `ModuleManifest::facades` agrupa métodos já exportados; o hash do
  manifesto inclui essa superfície.
- `register_facades_from_manifest` / `ModuleLoader::load(..., catalog, ...)`
  populam o `FacadeCatalog` após registrar operações.
- `FacadeHandle` guarda o `FacadeDescriptor` negociado e um `FacadeInvoker`
  (embedded → `OperationRegistry::dispatch`; remoto → `Client::call`).
- `Client::open_facade<TFacade>()` negocia versão e devolve handle tipado.
- Guia do consumidor: [FACADES.md](../FACADES.md).
