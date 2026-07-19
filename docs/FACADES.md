# Facades e handles — contrato do consumidor

Documento da Fase 11D. Complementa [API_PUBLICA.md](API_PUBLICA.md),
[OPERACAO_MODULOS.md](OPERACAO_MODULOS.md) e
[ADR-014](decisions/ADR-014-catalogo-de-facades-e-handles.md).

## Cadeia de responsabilidades

```text
consumidor
  → Client::open_facade<TFacade>() / ops::open_facade<TFacade>(...)
  → FacadeHandle<TFacade>  (id + versão negociados + descriptor)
  → invoke<Method>(args...)
  → valida Method ∈ facade
  → encode_args (codec tipado)
  → embedded: OperationRegistry::dispatch
    remoto:   Client::call → OpCall/OpResult
  → ExecutionContext + commit/rollback (Fase 9)
```

A facade **não** é um segundo motor: agrupa e versiona a superfície. A execução
permanece no registry / `OpCall`.

## Manifesto → catálogo

O `ModuleManifest` pode declarar `facades` (vetor de `FacadeDescriptor`). Cada
método da facade deve:

1. constar em `manifest.methods` com o mesmo `OperationMode`;
2. estar registrado no `OperationRegistry` após o `Registrar` do módulo.

`ModuleLoader::load(..., registry, catalog, registrar)` valida o hash, carrega
as operações e chama `register_facades_from_manifest`. O cliente **nunca** envia
binários nem escolhe caminhos de módulo (ADR-012).

Exemplo: `examples/accounts_facade/` — módulo `accounts_facade` exporta
`account.transfer` e a facade `accounts` v1.

## Evolução de versão

| Situação | Resultado |
|---|---|
| Cliente pede `(id, versão)` existente | `FacadeOpenOk.ok = true` → handle |
| Id conhecido, versão diferente | `incompatible_facade_version` |
| Id ausente | `facade_not_found` |
| `invoke` com operação fora da facade | `facade_method_not_found` (cliente) |

Regras:

- Identidade pública = `FacadeId` + `facade_version` (nunca índice do vetor).
- Nova versão incompatível → novo número; clientes antigos continuam na vN.
- Adicionar método em versão nova; remover/renomear exige versão nova.
- Hash do manifesto inclui facades; mudar superfície → novo hash na allowlist.

## Caminhos de uso

**Embedded**

```cpp
ops::FacadeCatalog catalog;
// ... register_facades_from_manifest / ModuleLoader::load com catalog
auto handle = ops::open_facade<AccountsFacade>(catalog, registry, database);
auto result = handle->invoke<TransferFunds>(alice, bob, 40);
```

**Rede**

```cpp
server.set_operation_registry(registry);
server.set_facade_catalog(catalog);
auto client = net::Client::connect(...);
auto handle = client->open_facade<AccountsFacade>();
auto result = handle->invoke<TransferFunds>(alice, bob, 40);
```

Erro de domínio (ex.: saldo insuficiente) propaga como `Result` falho e a
transação no servidor faz rollback — o mesmo contrato da Fase 9.

## Testes e demos

| Artefato | Papel |
|---|---|
| `modb.facade_catalog` | catálogo local |
| `modb.facade_handle` | invoke embedded |
| `modb.facade_server` | list/open + invoke remoto + rollback |
| `modb ops facade-demo` | demo CLI ponta a ponta |
