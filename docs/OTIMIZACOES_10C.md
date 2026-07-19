# Otimizações medidas — Fase 10C

Tag: `0.0.10c`  
Cenário de gate: `object_store.read_hotpath` (seed fixo, perfil `diagnostic`/`smoke`).

## Método

1. Registrar commit/baseline anterior (`0.0.10b` / merge da 10B).
2. Medir `modb_bench run --profile diagnostic --filter read_hotpath --seed 1`.
3. Aplicar uma mudança mínima no caminho quente.
4. Remedir com o mesmo seed/perfil e comparar via `modb_bench compare`.
5. Rodar `ctest` debug + sanitizers (correção antes de throughput).

Gates oficiais preferem Release; Debug serve para validar o runner.

## Otimizações desta entrega

### 1. `Database::get` sem decode completo do payload

- **Hipótese:** `get` + `materialize` decodificava o objeto duas vezes; `get` só
  precisa do `TypeDefinitionId` para validar o tipo bound.
- **Mudança:** `decode_object_header` + `ObjectStore::peek_type`; `get` usa o
  peek; `materialize` continua com um único `store_.get` completo.
- **Evidência:** cenário `object_store.read_hotpath` (create fora, N× get+materialize).

### 2. Fast-path `migration_for` com mapa vazio

- **Hipótese:** `materialize_decoded` alocava `std::string` e consultava
  `migrations_` em todo objeto mesmo sem migrações registradas.
- **Mudança:** retorno imediato quando `migrations_.empty()`.
- **Evidência:** mesmo cenário de leitura; custo fixo removido do caminho frio.

### 3. Índice FieldId em `ProjectionPlan::materialize`

- **Hipótese:** cada passo do plano fazia busca linear em `object.fields`.
- **Mudança:** montar índice denso `FieldId → AttributeValue*` uma vez por
  materialização (fallback linear se ids forem patológicos).
- **Evidência:** `modb.projection` + hotpath; Binding + plano cacheado permanecem
  no caminho crítico (sem interpretação dinâmica completa).

## Como reproduzir

```powershell
cmake --build --preset debug
.\build\debug\modb_bench.exe run --profile diagnostic --seed 1 --filter read_hotpath --output-dir benchmark-results
```

Comparar duas campanhas com o mesmo seed após mudanças futuras:

```powershell
.\build\debug\modb_bench.exe compare <antes.jsonl> <depois.jsonl>
```

## Dívidas ainda abertas (não nesta tag)

- `std::function` por campo no Binding (NTTP / ponteiro cru).
- Zero-copy em `to_field_values` / strings.
- `Handle::get<Member>()` materializa o objeto inteiro.
- Append incremental de coleções (O(n) hoje).
