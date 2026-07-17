# Protocolo de Implementação por Fase — ODB++ (moDb)

> Este documento detalha cada fase do [PLANO_ODB.md](PLANO_ODB.md) no nível de
> execução: design binário, assinaturas de API, passo a passo de implementação
> e especificação dos testes automatizados. O objetivo é que qualquer pessoa
> com C++ intermediário consiga implementar uma fase lendo apenas este
> documento e o código existente.

---

# Protocolo geral (vale para todas as fases)

## Convenções obrigatórias

1. **Erros**: nenhuma exceção no motor. Toda função que pode falhar retorna
   `Result<T>` (`std::expected<T, modb::Error>`). Novos códigos entram no enum
   `ErrorCode` em [error.hpp](../include/modb/error.hpp).
2. **Tipos fortes**: todo identificador é um struct com um único campo
   `value` e `operator==` default (padrão de `PageId`). Nunca usar inteiro cru.
3. **Formato em disco**: little-endian via `store_le`/`load_le`
   ([endian.hpp](../include/modb/storage/endian.hpp)). Proibido `memcpy` de
   structs, padding, ou dependência de ABI. Todo dado lido do arquivo é
   validado antes do uso (limites, tags, tamanhos).
4. **Estilo**: comentários em português explicando o porquê, seguindo a
   densidade do código atual. `.clang-format` do repositório.
5. **Namespaces**: modelo lógico em `modb`, armazenamento em `modb::storage`,
   modelo de objetos em `modb::object` (novo), rede em `modb::net` (fase 8).

## Como adicionar um teste (protocolo padrão)

1. Criar `tests/<area>_test.cpp` com um `main` que instancia `TestSuite`
   ([test_support.hpp](../tests/test_support.hpp)), usa `check`/`check_error`
   e retorna `suite.finish()`.
2. Usar `TemporaryDatabase` para todo arquivo temporário — um teste nunca
   depende de resíduos de outro.
3. Registrar no `CMakeLists.txt` seguindo o bloco dos testes existentes:
   `add_executable(modb_<area>_tests tests/<area>_test.cpp)` +
   `target_link_libraries(... modb::modb)` + bloco MinGW estático +
   `add_test(NAME modb.<area> COMMAND modb_<area>_tests)`.
4. A suíte inteira (`ctest`) deve estar verde antes de qualquer commit.

## Definição de pronto por fase

- [ ] Todas as tarefas do protocolo implementadas.
- [ ] Todos os testes especificados escritos e passando.
- [ ] Suíte completa verde (`ctest`) nos presets `debug` e `sanitizers`.
- [ ] Build limpo com `-DMODB_WARNINGS_AS_ERRORS=ON`.
- [ ] Critério de aceite da fase demonstrado por teste automatizado.
- [ ] Checkbox correspondente marcado no [PLANO_ODB.md](PLANO_ODB.md).

---

# Fase 0 — Decisões e fundações

## Objetivo

Fixar todas as decisões que afetam o formato de arquivo. Esta fase produz
documentos, não código — mas as decisões abaixo já estão tomadas e devem
apenas ser registradas como ADRs em `docs/decisions/` (uma ADR por item).

## Decisões fixadas (conteúdo das ADRs)

### ADR-001 — Identidade

| Id | Tipo | Largura | Regra |
|---|---|---|---|
| `ObjectId` | persistente | u64 | monotônico, nunca reutilizado; `0` = inválido/nulo |
| `TypeDefinitionId` | persistente | u64 | **é o `ObjectId`** do objeto TypeDefinition (catálogo é objeto) |
| `BaselineId` | persistente | u64 | é o `ObjectId` do objeto Baseline |
| `FieldId` | persistente | u16 | único dentro do tipo; nunca reutilizado entre versões |
| `BlobId` | persistente | u64 | `PageId` da primeira página do blob; `0` = ausente |
| `DatabaseId` | runtime | u32 | atribuído pelo `DatabaseRegistry`; nunca persistido |

### ADR-002 — ObjectIds reservados (bootstrap do catálogo)

ObjectIds `1..15` são reservados para meta-objetos. Os três primeiros são
**meta-tipos compilados no motor** (nunca lidos do disco):

| ObjectId | Meta-tipo |
|---|---|
| 1 | `TypeDefinition` (o tipo que descreve tipos) |
| 2 | `AttributeDefinition` |
| 3 | `Baseline` |

Objetos de catálogo gravados em disco usam esses ids como
`TypeDefinitionId`. Isso resolve o bootstrap: o motor sabe decodificar uma
TypeDefinition sem precisar ler uma TypeDefinition. O primeiro `ObjectId` de
usuário é `16`.

### ADR-003 — Tipos de atributo e encoding de valores

Tags de tipo (u8) e encoding do valor no payload:

| Tag | Tipo | Encoding do valor |
|---|---|---|
| 0 | `null` | (sem bytes) |
| 1 | `boolean` | u8 (0/1; outro valor = `invalid_encoding`) |
| 2 | `int64` | u64 LE (complemento de dois via `std::bit_cast`) |
| 3 | `float64` | u64 LE (bits IEEE-754 via `std::bit_cast`) |
| 4 | `string` | u32 comprimento + bytes UTF-8 |
| 5 | `bytes` | u32 comprimento + bytes crus |
| 6 | `ref` | u64 `ObjectId` |
| 7 | `blob` | u64 `BlobId` |
| 8 | `embedded` | u32 comprimento + payload aninhado (formato da Fase 2) |

Conversões permitidas no `ProjectionPlan::Convert` (Fase 3):
`int64↔float64` (float→int trunca; overflow = erro), `boolean→int64`.
Qualquer outra conversão exige migração registrada.

### ADR-004 — Página raiz do banco (`DBRT`)

O campo `catalog_root` do superbloco passa a apontar para uma página `DBRT`
(criada na primeira escrita OO), layout:

| Offset | Campo | Tipo |
|---|---|---|
| 0 | magic `"DBRT"` | 4 bytes |
| 4 | versão | u16 (=1) |
| 6 | flags | u16 (=0) |
| 8 | `identity_dir` (PageId da IDMD raiz) | u64 |
| 16 | `catalog_heap_root` (THRP do heap de catálogo) | u64 |
| 24 | `data_heap_root` (THRP do heap de dados) | u64 |
| 32 | `next_object_id` | u64 |
| 40 | `current_baseline` (ObjectId) | u64 |
| 48.. | reservado (zeros) | — |

`0` em qualquer campo de página/id significa "ainda não existe". No MVP há
**um** heap de dados para todos os objetos (segmentação por tipo é pós-MVP).

### ADR-005 — Mapa de identidade (ObjectId → localização física)

Estrutura direta indexada por `ObjectId` (possível porque ids são monotônicos
e não reutilizados). Duas camadas de páginas:

**IDMD (diretório)**: header 16 bytes
(`"IDMD"` | versão u16 | reservado u16 | `next_dir` u64) seguido de até
`(page_size-16)/8` PageIds de páginas IDMP. Diretórios encadeiam por
`next_dir` quando lotam.

**IDMP (entradas)**: header 16 bytes (`"IDMP"` | versão u16 | reservado)
seguido de entradas de 16 bytes:

| Offset | Campo | Tipo |
|---|---|---|
| 0 | `page` (PageId do registro) | u64 |
| 8 | `slot` | u16 |
| 10 | `generation` | u16 |
| 12 | flags (bit0 = alocado, bit1 = removido) | u32 |

Endereçamento: `entradas_por_pagina = (page_size-16)/16`;
`indice_global = object_id`; página IDMP = `indice_global / entradas_por_pagina`
(resolvida via diretório); entrada = `indice_global % entradas_por_pagina`.
Lookup é O(1) com no máximo 2 leituras de página (diretório + entradas).

### ADR-006 — Destino do código relacional

`Catalog`, `Table`, `Schema`, `Row` e comandos relacionais da CLI são
removidos na Fase 2, quando o caminho vertical OO os substitui. `Value` e
`DataType` são absorvidos por `AttributeValue`/`AttributeType`. O storage
(`NativeFile`, `PageFile`, `SlottedPage`, `TableHeap`, `endian`,
`BinaryReader/Writer`) permanece intacto.

### ADR-007 — Limites do MVP OO

| Limite | Valor |
|---|---|
| atributos por tipo | 256 |
| identificadores (nomes) | ≤ 63 bytes UTF-8 |
| payload de objeto | deve caber numa página (blobs para o resto) |
| string/bytes inline | ≤ capacidade do payload; acima disso, usar blob |

## Tarefas

1. Escrever as 7 ADRs acima em `docs/decisions/ADR-00X-*.md` (uma por
   decisão, formato: contexto → decisão → consequências).
2. Adicionar termos OO ao `GLOSSARIO.md` (ObjectId, Handle, Binding,
   ProjectionPlan, Baseline, TypeDefinition, BlobStore, migração preguiçosa).
3. Adicionar aviso de superseded no topo de `PLANO_DE_DESENVOLVIMENTO.md`,
   `ESCOPO_MVP.md` e `README.md` apontando para `PLANO_ODB.md`.

## Testes automatizados

Nenhum código novo. A suíte existente permanece verde (regressão zero).

## Critério de conclusão

Todas as ADRs revisadas; qualquer implementador da Fase 1/2 responde, só com
os documentos: "qual a largura de ObjectId? como acho o registro de um objeto?
como o catálogo se decodifica a si mesmo?".

---

# Fase 1 — Modelo de objetos e catálogo em memória

## Artefatos novos

```text
include/modb/object/ids.hpp            (ObjectId, FieldId, BlobId, ...)
include/modb/object/attribute_value.hpp
include/modb/object/type_definition.hpp
include/modb/object/baseline.hpp
include/modb/object/type_registry.hpp
src/object/attribute_value.cpp
src/object/type_definition.cpp
src/object/type_registry.cpp
tests/object_model_test.cpp
```

## Design

### ids.hpp

```cpp
namespace modb::object {
struct ObjectId  { std::uint64_t value{}; friend bool operator==(ObjectId, ObjectId) = default; };
struct FieldId   { std::uint16_t value{}; friend bool operator==(FieldId, FieldId) = default; };
struct BlobId    { std::uint64_t value{}; friend bool operator==(BlobId, BlobId) = default; };
struct DatabaseId{ std::uint32_t value{}; friend bool operator==(DatabaseId, DatabaseId) = default; };
using TypeDefinitionId = ObjectId; // ADR-001: o catálogo é objeto.
using BaselineId       = ObjectId;
inline constexpr ObjectId meta_type_definition{1};
inline constexpr ObjectId meta_attribute_definition{2};
inline constexpr ObjectId meta_baseline{3};
inline constexpr std::uint64_t first_user_object_id = 16;
}
```

### AttributeValue

```cpp
enum class AttributeType : std::uint8_t {
    null = 0, boolean = 1, int64 = 2, float64 = 3,
    string = 4, bytes = 5, ref = 6, blob = 7, embedded = 8,
};

class AttributeValue {
    using Storage = std::variant<std::monostate, bool, std::int64_t, double,
                                 std::string, std::vector<std::byte>,
                                 ObjectId, BlobId>;
    // API espelhada em Value atual: construtores por tipo, type() -> AttributeType,
    // accessors tipados as_bool()/as_int64()/... retornando Result<T>.
    // std::visit exaustivo com Overloaded (nunca cadeias de get_if).
};
```

`embedded` não tem storage próprio nesta fase (entra na Fase 4); a tag existe
desde já para o formato não mudar.

### AttributeDefinition / TypeDefinition / Baseline

```cpp
struct AttributeDefinition {
    FieldId id; std::string name; AttributeType type;
    bool nullable{}; std::optional<AttributeValue> default_value;
    bool is_collection{}; bool is_embedded{}; bool is_owned{};
};

class TypeDefinition { // imutável após criação
public:
    static Result<TypeDefinition> create(std::string name,
                                         std::vector<AttributeDefinition> attrs);
    // Valida: nome ≤ 63 bytes; ≤ 256 atributos; FieldIds únicos e != 0;
    //         nomes de atributo únicos; default compatível com o tipo/nullable.
    TypeDefinitionId id() const;           // atribuído na persistência (Fase 2);
    const std::string& name() const;       // em memória, id{0} = ainda não persistido.
    std::span<const AttributeDefinition> attributes() const;
    const AttributeDefinition* find(FieldId) const;   // nullptr se ausente
    const AttributeDefinition* find(std::string_view) const;
};

class Baseline { // imutável: snapshot estrutural completo
public:
    static Result<Baseline> create(std::vector<TypeDefinitionId> types);
    BaselineId id() const;
    std::span<const TypeDefinitionId> types() const;
};
```

### TypeRegistry

```cpp
class TypeRegistry {
public:
    Result<TypeDefinitionId> register_type(TypeDefinition def); // rejeita nome duplicado na baseline corrente
    Result<std::reference_wrapper<const TypeDefinition>> find(TypeDefinitionId) const;
    Result<std::reference_wrapper<const TypeDefinition>> find(std::string_view name) const; // mais recente
};
```

### Validação de payload lógico

```cpp
using FieldValues = std::vector<std::pair<FieldId, AttributeValue>>;
Result<void> validate_object(const TypeDefinition&, const FieldValues&);
// Regras: todo FieldId existe no tipo; sem FieldId duplicado; tipo do valor
// compatível com o atributo (null exige nullable); atributo sem valor e sem
// default e não-nullable → erro null_constraint_violation.
```

Novos `ErrorCode`: `duplicate_field`, `field_not_found`, `duplicate_type`,
`type_not_found`, `invalid_object_id`.

## Passo a passo

1. `ids.hpp` (sem dependências).
2. `AttributeValue` — portar a estrutura de `Value` (variant + visit) para o
   conjunto novo de tipos; accessors tipados.
3. `AttributeDefinition` + validações locais.
4. `TypeDefinition::create` com toda a validação; imutabilidade por membros
   `const`-acessíveis apenas.
5. `Baseline`, `TypeRegistry`.
6. `validate_object`.
7. Testes.

## Testes automatizados — `tests/object_model_test.cpp` (CTest `modb.object_model`)

| Caso | Verificação |
|---|---|
| tipos de valor | cada `AttributeType` constrói, reporta `type()` correto e faz round-trip pelo accessor |
| accessor errado | `as_int64()` sobre string → erro `type_mismatch` |
| tipo válido | `TypeDefinition::create` aceita definição bem formada |
| FieldId duplicado | → `duplicate_field` |
| FieldId zero | → `invalid_argument` |
| nome duplicado de atributo | → `duplicate_column` (reusado) |
| >256 atributos | → `too_many_columns` (reusado) |
| nome de tipo >63 bytes | → `invalid_identifier` |
| default incompatível | default string em atributo int64 → `type_mismatch` |
| registry duplicado | registrar dois tipos "Employee" → `duplicate_type` |
| registry find | por id e por nome, incluindo `type_not_found` |
| validate ok | payload completo compatível → sucesso |
| validate null em não-nullable | → `null_constraint_violation` |
| validate campo inexistente | → `field_not_found` |
| validate ausente com default | → sucesso (default cobre) |

## Critério de conclusão

Todos os casos acima verdes; `TypeDefinition`/`Baseline` sem nenhum método
mutador público.

---

# Fase 2 — Codec genérico e ObjectStore persistente

## Artefatos novos

```text
include/modb/object/object_codec.hpp      src/object/object_codec.cpp
include/modb/object/identity_map.hpp      src/object/identity_map.cpp
include/modb/object/database_root.hpp     src/object/database_root.cpp
include/modb/object/object_store.hpp      src/object/object_store.cpp
include/modb/object/catalog_store.hpp     src/object/catalog_store.cpp
tests/object_codec_test.cpp
tests/identity_map_test.cpp
tests/object_store_test.cpp
tests/catalog_persistence_test.cpp
```

Removidos ao final: `catalog.hpp/cpp`, `table.hpp/cpp`, `schema.*`, `row.hpp`,
comandos relacionais da CLI, `catalog_test.cpp`, `model_test.cpp` (substituído
por `object_model_test.cpp`).

## Design

### Formato do registro de objeto (dentro de um slot do TableHeap)

```text
| object_id u64 | type_definition_id u64 | payload |
```

### Formato do payload

```text
| versão u8 (=1) | field_count u16 |
| field_id u16 | tag u8 | valor (ADR-003) |   × field_count
```

Regras de decodificação defensiva (o arquivo é entrada não confiável):
`field_count ≤ 256` **antes** de qualquer `reserve`; comprimentos de
string/bytes validados contra `remaining()` antes de ler; tag desconhecida →
`invalid_encoding`; bytes sobrando após o último campo → `trailing_data`;
`field_id` duplicado → `invalid_encoding`.

### object_codec.hpp

```cpp
// Codec genérico: não conhece classes C++, apenas TypeDefinition (ADR do doc §21).
Result<std::vector<std::byte>> encode_object(const TypeDefinition&, ObjectId,
                                             const FieldValues&);
struct DecodedObject { ObjectId id; TypeDefinitionId type; FieldValues fields; };
Result<DecodedObject> decode_object(std::span<const std::byte> record);
// decode NÃO exige a TypeDefinition (formato autodescritivo por tags);
// a validação semântica contra o tipo é feita por quem chama.
```

### identity_map.hpp

```cpp
struct ObjectLocation { storage::RecordId record; };
class IdentityMap {
public:
    static Result<IdentityMap> create(storage::PageFile&);          // aloca IDMD raiz
    static Result<IdentityMap> open(storage::PageFile&, PageId dir);
    PageId directory_root() const;
    Result<void> bind(ObjectId, storage::RecordId);   // aloca IDMP sob demanda
    Result<ObjectLocation> find(ObjectId) const;      // ausente/removido → record_not_found
    Result<void> rebind(ObjectId, storage::RecordId); // objeto mudou de página
    Result<void> erase(ObjectId);                     // marca tombstone (bit1)
};
```

### database_root.hpp

Lê/escreve a página `DBRT` (ADR-004). API: `DatabaseRoot::create(PageFile&)`,
`open(PageFile&, PageId)`, getters, e `Result<void> update(...)` que regrava a
página. O `PageFile::set_catalog_root` existente aponta o superbloco para ela.

### object_store.hpp

```cpp
class ObjectStore {
public:
    // Abre/cria toda a hierarquia: DBRT -> IdentityMap + TableHeaps.
    static Result<ObjectStore> create(storage::PageFile&);
    static Result<ObjectStore> open(storage::PageFile&);

    Result<ObjectId> create_object(const TypeDefinition&, FieldValues);
    Result<DecodedObject> get(ObjectId) const;
    Result<void> update(ObjectId, const TypeDefinition&, FieldValues);
    Result<void> remove(ObjectId);
    // Varredura sequencial (base do streaming futuro).
    Result<void> scan(std::function<Result<void>(const DecodedObject&)>) const;
};
```

`create_object`: valida → aloca `ObjectId` (`next_object_id++` no DBRT, gravado
**antes** do registro; crash entre os dois desperdiça um id, nunca duplica) →
`encode_object` → `TableHeap::insert` → `IdentityMap::bind`.

### catalog_store.hpp

Persiste `TypeDefinition`/`Baseline` como objetos no heap de catálogo usando o
próprio codec com os meta-tipos (ADR-002):

- `TypeDefinition` como objeto: campo 1 = `name` (string), campo 2 =
  `attributes` (bytes com o sub-encoding: `count u16` e por atributo:
  `field_id u16 | name: u16 len + bytes | tag u8 | flags u8 | has_default u8 |
  [default no encoding ADR-003]`; flags: bit0 nullable, bit1 collection,
  bit2 embedded, bit3 owned).
- `Baseline` como objeto: campo 1 = `type_ids` (bytes: `count u16` + u64 ids).

API: `save_type`, `save_baseline`, `load_all` (varre o heap de catálogo e
reconstrói o `TypeRegistry` + baseline corrente na abertura).

## Passo a passo

1. Codec (`encode_object`/`decode_object`) + testes de round-trip — sem I/O.
2. `DatabaseRoot` (criação, abertura, atualização, validação de magic/versão).
3. `IdentityMap` (bind/find/rebind/erase, crescimento de IDMP e encadeamento
   de IDMD).
4. `ObjectStore` amarrando tudo; alocação de ObjectId.
5. `CatalogStore` + reconstrução na abertura.
6. CLI: `modb type define <db> <nome> <campo:tipo[:null]>...`, `modb type list <db>`,
   `modb object create/get/remove <db> ...` (parsing fora da biblioteca).
7. Remover o modelo relacional e seus testes; adaptar `database_check` para
   reconhecer páginas DBRT/IDMD/IDMP.
8. Testes de integração e reabertura.

## Testes automatizados

**`tests/object_codec_test.cpp`** (`modb.object_codec`)

| Caso | Verificação |
|---|---|
| round-trip por tipo | um objeto com todos os 8 tipos de valor codifica e decodifica idêntico |
| round-trip com null e default | campos ausentes/null preservados |
| field_count mentiroso | payload declara 1000 campos com 3 bytes → `unexpected_end_of_input`, **sem** alocação gigante |
| tag desconhecida | tag 99 → `invalid_encoding` |
| comprimento de string além do buffer | → `unexpected_end_of_input` |
| trailing data | bytes extras → `trailing_data` |
| field_id duplicado no payload | → `invalid_encoding` |

**`tests/identity_map_test.cpp`** (`modb.identity_map`)

| Caso | Verificação |
|---|---|
| bind/find | localização idêntica após bind |
| find inexistente | → `record_not_found` |
| erase | find após erase → `record_not_found`; rebind após erase → erro |
| crescimento | 10 000 binds (força múltiplas IDMP e 2+ IDMD encadeadas); todos os finds corretos |
| reabertura | fechar PageFile, reabrir, todos os finds corretos |

**`tests/object_store_test.cpp`** (`modb.object_store`)

| Caso | Verificação |
|---|---|
| create/get | objeto recuperado idêntico, ObjectId ≥ 16 |
| ids monotônicos | N criações → ids estritamente crescentes |
| update | conteúdo novo visível; identidade inalterada mesmo se mudou de página |
| remove | get → `record_not_found`; id nunca é reutilizado por criação posterior |
| scan | enumera exatamente os vivos |
| **integração (critério da fase)** | 500 objetos (payloads variados, multi-página), fechar instância, `ObjectStore::open`, verificar os 500 por get e por scan |

**`tests/catalog_persistence_test.cpp`** (`modb.catalog_persistence`)

| Caso | Verificação |
|---|---|
| save/load de tipo | TypeDefinition com todos os recursos (defaults, flags) sobrevive à reabertura |
| baseline | baseline corrente restaurada |
| versão de tipo | segunda gravação do mesmo nome cria nova definição/baseline; busca por nome retorna a versão ativa |
| arquivo v. relacional antigo | abrir um arquivo sem DBRT → erro claro, sem crash |

## Critério de conclusão

Critério de aceite da fase (teste de integração acima) verde; modelo
relacional removido; `database_check` reconhece as páginas novas.

---

# Fase 3 — Binding, Handle e ProjectionPlan

## Artefatos novos

```text
include/modb/object/binding.hpp           src/object/binding.cpp
include/modb/object/projection_plan.hpp   src/object/projection_plan.cpp
include/modb/object/database.hpp          src/object/database.cpp
include/modb/object/handle.hpp
tests/binding_test.cpp
tests/projection_test.cpp
tests/schema_evolution_test.cpp
```

## Design

### Binding

Liga `FieldId` → membro C++ da versão atual (doc §22). Só há um binding por
tipo por aplicação.

```cpp
template <typename T>
class BindingBuilder {
public:
    explicit BindingBuilder(std::string type_name);
    template <std::uint16_t Id, typename M>
    BindingBuilder& field(std::string name, M T::* member);
    Result<Binding<T>> build(); // valida: ids únicos, nomes únicos, ≥1 campo
};
```

Internamente cada campo vira um `FieldBinder`:

```cpp
struct FieldBinder {
    FieldId id; std::string name; AttributeType type;
    void (*store)(void* object, const AttributeValue&); // escreve no membro
    AttributeValue (*load)(const void* object);         // lê do membro
};
```

Mapeamento de tipo C++ → `AttributeType` por trait
(`bool→boolean`, inteiros→int64, `double→float64`, `std::string→string`,
`std::vector<std::byte>→bytes`; `Ref<T>`/`BlobId` entram na Fase 4). Tipo não
mapeável = `static_assert` com mensagem clara.

O `Binding<T>` gera sua `TypeDefinition` canônica (`to_type_definition()`),
usada para comparação com o catálogo.

### ProjectionPlan (doc §23–24)

```cpp
enum class ProjectionOp : std::uint8_t { copy, convert, use_default, ignore, resolve_reference };
struct ProjectionStep { ProjectionOp op; FieldId source; std::size_t binder_index;
                        AttributeType from, to; };
class ProjectionPlan {
public:
    // Compara a TypeDefinition persistida com o binding atual e gera os passos.
    static Result<ProjectionPlan> build(const TypeDefinition& stored, const BindingBase& current);
    Result<void> materialize(const DecodedObject&, void* destination) const;
};
```

Regras de construção: campo presente nos dois com o mesmo tipo → `copy`; tipos
diferentes com conversão permitida (ADR-003) → `convert`; permitidos → nada;
não permitido → erro `incompatible_projection` (novo ErrorCode) **a menos**
que exista migração registrada; campo só no binding → `use_default` (do
binding/definição; sem default e não-nullable → erro); campo só no persistido
→ `ignore`; tags `ref` → `resolve_reference`.

Cache: `unordered_map<TypeDefinitionId, ProjectionPlan>` por `Database`
(binding é fixo por processo, então a chave é só o tipo persistido).

### Database / DatabaseRegistry / Handle

```cpp
class Database {
public:
    static Result<Database> open(const std::filesystem::path&);  // cria se não existe? não: create/open separados como PageFile
    static Result<Database> create(const std::filesystem::path&);

    template <typename T> Result<void> bind(BindingBuilder<T>);
    // Na vinculação: procura TypeDefinition mais recente com o mesmo nome;
    //  - inexistente  -> persiste a canônica do binding (novo tipo, nova baseline)
    //  - idêntica     -> adota o id existente
    //  - divergente   -> persiste NOVA TypeDefinition + nova Baseline (evolução);
    //                    a antiga permanece intocada (doc §19).

    template <typename T> Result<Handle<T>> create(const T& value);
    template <typename T> Result<Handle<T>> get(ObjectId);
    template <typename T> Result<T> materialize(Handle<T>) const;
    Result<void> remove(ObjectId);

    Result<void> register_migration(
        std::string type_name, std::uint64_t from_type_id,
        std::function<Result<FieldValues>(const DecodedObject&)>);
};

class DatabaseRegistry { // doc §5
public:
    Result<DatabaseId> attach(std::shared_ptr<Database>);
    Result<std::shared_ptr<Database>> find(DatabaseId) const;
    void detach(DatabaseId);
};

template <typename T>
class Handle { // apenas identidade (doc §6)
public:
    DatabaseId database() const; ObjectId id() const;
    template <auto Member> auto get() const
        -> Result<member_type_t<decltype(Member)>>;      // materializa 1 campo
    template <auto Member, typename V>
    Result<void> set(Transaction&, V&& value);           // regrava com a definição atual
};
```

`Transaction` nesta fase é um placeholder (`Database::begin()` retorna um
objeto vazio; commit implícito por operação). A assinatura já existe para a
Fase 5 não quebrar a API. Escrita via `set` implementa a **migração
preguiçosa** (doc §28): o objeto é regravado com a TypeDefinition atual.

## Passo a passo

1. Traits de tipo + `FieldBinder` + `BindingBuilder::build`.
2. `to_type_definition()` e comparação estrutural (nome+id+tipo+flags).
3. Registro do binding no `Database` com os três desfechos (novo/igual/evolução).
4. `ProjectionPlan::build` + `materialize` + cache.
5. `Database::create/get/materialize/remove`, `Handle::get/set`.
6. `register_migration` (consultado pelo `build` do plano quando a projeção
   automática é impossível).
7. Testes.

## Testes automatizados

**`tests/binding_test.cpp`** (`modb.binding`)

| Caso | Verificação |
|---|---|
| binding válido | gera TypeDefinition canônica esperada (ids, nomes, tags) |
| FieldId duplicado | `build()` → `duplicate_field` |
| ida e volta | `T → payload → T` idêntico via binding + codec |
| tipo novo | primeiro `bind` persiste tipo e baseline |
| tipo idêntico | segundo processo (reabertura) com mesmo binding **não** cria nova definição |

**`tests/projection_test.cpp`** (`modb.projection`)

| Caso | Verificação |
|---|---|
| plano copy puro | tipos idênticos → todos os passos `copy` |
| convert | int64→float64 e float64→int64 (trunca); overflow → erro |
| default | campo novo recebe default; sem default não-nullable → erro na construção do plano |
| ignore | campo removido some sem erro |
| cache | segunda materialização do mesmo TypeDefinitionId não reconstrói o plano (verificar por contador interno exposto para teste) |

**`tests/schema_evolution_test.cpp`** (`modb.schema_evolution`) — **critério do MVP OO**

| Caso | Verificação |
|---|---|
| cenário v1→v2 | grava `Employee{name,salary}` com binding v1; fecha; reabre com binding v2 (`+country`, default "BR"); lê o objeto antigo com `country=="BR"` — sem migração manual |
| migração preguiçosa | após `set` no objeto antigo, o registro passa a usar a TypeDefinition v2 (verificar `type_definition_id` no registro) e o objeto v1 original permanece legível |
| coexistência | objetos v1 e v2 lidos na mesma sessão, ambos corretos |
| migração semântica | `salary`→`salary_cents` via `register_migration` (multiplica por 100); sem o registro, a projeção falha com `incompatible_projection` |
| baselines imutáveis | evolução cria nova baseline; a anterior continua carregável |

## Critério de conclusão

Cenário v1→v2 verde de ponta a ponta. Este teste é o **critério de aceite do
MVP OO** inteiro.

---

# Fase 4 — Relacionamentos, coleções e BlobStore

## Artefatos novos

```text
include/modb/object/ref.hpp
include/modb/object/blob_store.hpp        src/object/blob_store.cpp
include/modb/object/persistent_vector.hpp src/object/persistent_vector.cpp
include/modb/object/persistent_set.hpp    (idem map)
tests/relationship_test.cpp
tests/blob_store_test.cpp
tests/collection_test.cpp
```

## Design

### Referências

```cpp
template <typename T> struct Ref      { ObjectId target{}; };            // associação (◇)
template <typename T> struct OwnedRef { ObjectId target{}; };            // composição (◆): cascata
template <typename T> struct Embedded { T value{}; };                    // sem identidade
```

- No binding, `Ref<T>`/`OwnedRef<T>` mapeiam para tag `ref` (a distinção
  associação/composição vive na `AttributeDefinition.is_owned`).
- `Embedded<T>` exige que `T` também tenha binding registrado; serializa como
  tag `embedded` (payload aninhado completo).
- Política de remoção (registrar como ADR-008): remover objeto referenciado
  por `Ref` é **permitido**; a resolução posterior falha com
  `record_not_found` (referência pendente detectável). `OwnedRef` remove o
  filho em cascata (profundidade-primeiro, ciclos detectados por conjunto de
  visitados → `invalid_argument`).

### BlobStore

Página `BLBP`: header 24 bytes
(`"BLBP"` | versão u16 | reservado u16 | `next` u64 | `payload_length` u32 |
reservado u32) + até `page_size-24` bytes de dados. `BlobId` = PageId da
primeira página (ADR-001).

```cpp
class BlobStore {
public:
    explicit BlobStore(storage::PageFile&);
    Result<BlobId> create(std::span<const std::byte>);           // fatia em páginas encadeadas
    Result<std::vector<std::byte>> read(BlobId) const;           // valida cadeia e comprimentos
    Result<void> read_chunks(BlobId,                             // leitura em streaming
        std::function<Result<void>(std::span<const std::byte>)>) const;
    Result<BlobId> rewrite(BlobId, std::span<const std::byte>);  // reusa/estende/apara a cadeia
    Result<void> remove(BlobId);                                 // zera e devolve as páginas à cadeia livre (pós-MVP: free list; MVP: apenas marca)
};
```

Validações: magic/versão por página; ciclo na cadeia → `page_chain_cycle`;
`payload_length > page_size-24` → `corrupt_page`.

### PersistentVector

Objeto pai guarda só o `BlobId` (doc §12). Encoding do blob:
`| count u32 | elemento... |` com cada elemento no encoding ADR-003 (tag
incluída). MVP: `push_back` = rewrite do blob (correto primeiro; otimização de
append entra na Fase 10 com medição). API:

```cpp
template <typename T> class PersistentVector {
public:
    Result<std::size_t> size() const;
    Result<T> at(std::size_t) const;
    Result<void> push_back(Transaction&, const T&);
    Result<void> for_each(std::function<Result<void>(const T&)>) const; // sem materializar tudo
};
```

`PersistentSet<T>`/`PersistentMap<K,V>`: mesmo blob, elementos mantidos
ordenados pela codificação canônica; busca binária na leitura. Documentar o
custo O(n) de escrita como limitação de MVP.

## Passo a passo

1. `BlobStore` completo + testes (independe do resto).
2. Tags `ref`/`embedded` no codec e nos traits do binding.
3. Resolução `resolve_reference` no ProjectionPlan (`ObjectId` → `Handle<T>`).
4. Cascata de `OwnedRef` no `Database::remove`.
5. `PersistentVector`, depois `Set`/`Map`.
6. `database_check`: validar cadeias BLBP e refs órfãs (aviso, não erro).

## Testes automatizados

**`tests/blob_store_test.cpp`** (`modb.blob_store`)

| Caso | Verificação |
|---|---|
| round-trip pequeno | blob < 1 página |
| round-trip grande | blob de 100 KiB (≥ 25 páginas), byte a byte |
| read_chunks | concatenação dos chunks == conteúdo |
| rewrite maior/menor | cadeia estende e apara corretamente; reabertura ok |
| ciclo na cadeia | corromper `next` para página anterior → `page_chain_cycle` |
| comprimento corrompido | `payload_length` inválido → `corrupt_page` |

**`tests/relationship_test.cpp`** (`modb.relationship`)

| Caso | Verificação |
|---|---|
| associação | `Employee.department: Ref<Department>` resolve para o objeto certo após reabertura |
| referência pendente | remover Department; resolver → `record_not_found` (sem crash) |
| embedded | `Embedded<Address>` round-trip dentro do pai; sem ObjectId próprio |
| composição | remover Employee remove Address own-ref em cascata |
| cascata profunda | A◆B◆C: remover A remove os três |
| ciclo em owned | A◆B◆A → remoção detecta ciclo e falha explicitamente |

**`tests/collection_test.cpp`** (`modb.collection`)

| Caso | Verificação |
|---|---|
| vector básico | push_back/at/size/for_each com 10 000 elementos (multi-página), reabertura |
| vector de refs | `PersistentVector<Ref<Project>>` resolve elementos |
| set | inserção com duplicatas → deduplicado e ordenado |
| map | put/get/remove e reabertura |
| **grafo do critério** | Employee→Department (Ref), Employee◆Address (Owned), Employee.projects (vector de refs): grava, reabre, verifica tudo, remove Employee e confere cascata + projects intactos (Project não é owned) |

## Critério de conclusão

Teste "grafo do critério" verde após reabertura.

---

# Fase 5 — Transações, WAL e recuperação

## Artefatos novos

```text
include/modb/tx/transaction.hpp        src/tx/transaction.cpp
include/modb/tx/wal.hpp                src/tx/wal.cpp
include/modb/tx/recovery.hpp           src/tx/recovery.cpp
include/modb/storage/page_cache.hpp    src/storage/page_cache.cpp
tests/wal_test.cpp
tests/recovery_test.cpp
tests/failpoint_test.cpp
```

## Design

### Modelo: WAL redo-only com after-images de página

Simples, correto e suficiente para single-writer: durante a transação, toda
página modificada é escrita **somente** no WAL (after-image completa da
página); as páginas de dados não são tocadas. No commit: registro `commit` →
`sync` do WAL → aplicação das páginas no arquivo de dados → (checkpoint
eventual). Rollback = descartar as páginas pendentes (nada foi aplicado).

### Arquivo WAL: `<db>.wal`

Header (32 bytes): `"MOWL"` | versão u16 | reservado | `page_size` u32 |
reservado. Registros:

```text
| lsn u64 | tx_id u64 | tipo u8 | page_id u64 | length u32 | payload | crc32 u32 |
```

Tipos: `1=begin`, `2=page_image` (payload = página completa), `3=commit`,
`4=checkpoint`. CRC32 cobre do `lsn` ao fim do payload; registro com CRC
inválido ou truncado marca o **fim lógico** do WAL (tudo depois é descartado).

### PageCache (embrião do BufferPool)

```cpp
class PageCache { // capacidade fixa, sem eviction no MVP (falha se exceder — limite documentado)
public:
    Result<Page*> fetch(PageId);          // lê do arquivo se ausente
    void mark_dirty(PageId);
    std::span<const PageId> dirty() const;
    Result<void> apply_to(storage::PageFile&); // escreve sujas + flush
    void discard();                        // rollback
};
```

### Transaction

```cpp
class Transaction {
public:
    TxId id() const;
    Result<void> commit();   // WAL(images+commit) -> wal.sync() -> aplica páginas -> flush
    Result<void> rollback(); // discard
    ~Transaction();          // não-committed -> rollback automático
};
// Database::begin() -> Result<Transaction>; um único escritor por vez
// (segunda begin com escrita pendente -> ErrorCode::transaction_active).
```

Toda a API de escrita (`create`, `set`, `remove`, `push_back`) passa a exigir
`Transaction&` — o placeholder da Fase 3 vira real. Escritas fora de transação
→ `transaction_required` (novo ErrorCode).

### Recuperação (na abertura do banco)

1. Se `<db>.wal` não existe ou é vazio → nada a fazer.
2. Varrer registros válidos até o fim lógico; agrupar `page_image` por tx.
3. Para cada tx **com** `commit`: reaplicar as after-images (idempotente).
4. `flush()` do arquivo de dados; remover o WAL; pronto. Se a remoção falhar,
   reportar o erro e preservar o WAL para redo idempotente na próxima abertura.

### Failpoints (infraestrutura de teste)

Decorator de teste sobre `NativeFile` que falha/interrompe após N operações:

```cpp
class FailpointFile { // tests/failpoint_file.hpp — só na árvore de testes
    // encaminha para NativeFile; após fail_after_writes_, toda escrita
    // retorna io_error simulando o processo morto no meio.
};
```

O WAL recebe o arquivo por interface (template ou std::function de fábrica)
para o teste injetar o failpoint.

## Passo a passo

1. `PageCache` (sem WAL) + testes unitários.
2. Formato WAL: append, CRC, leitura sequencial com fim lógico.
3. `Transaction::commit`/`rollback` sobre o cache + WAL.
4. Recuperação na abertura + truncamento.
5. Exigir `Transaction&` em toda escrita (compila-quebra guiada).
6. Failpoints e a matriz de testes de falha.

## Testes automatizados

**`tests/wal_test.cpp`** (`modb.wal`)

| Caso | Verificação |
|---|---|
| round-trip de registros | begin/image/commit lidos de volta idênticos |
| CRC corrompido | flip de 1 byte → registro rejeitado, fim lógico correto |
| WAL truncado no meio de um registro | leitura para no último registro íntegro |
| cabeçalho WAL ilegível | abertura falha com `wal_corrupt` e preserva o arquivo |

**`tests/recovery_test.cpp`** (`modb.recovery`)

| Caso | Verificação |
|---|---|
| commit aplicado | tx commitada, processo "morre" antes de aplicar páginas (só WAL existe) → reabertura aplica; objeto visível |
| tx sem commit | images sem commit no WAL → reabertura ignora; objeto ausente |
| idempotência | recuperar duas vezes o mesmo WAL → estado idêntico |
| rollback explícito | alterações somem; objetos pré-existentes intactos |
| destrutor | Transaction sai de escopo sem commit → rollback |

**`tests/failpoint_test.cpp`** (`modb.failpoint`) — matriz de morte simulada

| Ponto de falha | Estado esperado após reabertura |
|---|---|
| antes do registro commit no WAL | transação ausente por completo |
| depois do commit no WAL, antes de aplicar páginas | transação **presente** por completo (redo) |
| no meio da aplicação das páginas | presente por completo (reaplicação idempotente) |
| durante o checkpoint/truncamento | presente; WAL residual tratado |

Cada linha: montar cenário com `FailpointFile`, "matar", reabrir com o arquivo
real, verificar TUDO-ou-NADA por `get`/`scan`.

## Critério de conclusão

Matriz de failpoints 100% verde: nenhuma linha exibe transação parcial.

---

# Fase 6 — Snapshots e MVCC

A implementação é dividida em 6A–6D. Cada incremento deve terminar verde e
utilizável antes do início do seguinte; não se considera a Fase 6 parcialmente
concluída apenas porque o formato novo foi gravado.

## Fase 6A — Épocas e formato versionado

O ADR-009 deve fixar o modelo inicial **single-writer / multi-reader por
época**, o limite de uma versão anterior por objeto, os conflitos esperados e
o comportamento de versões após reabertura.

- `epoch` global u64 no DBRT, incrementado a cada commit.
- A entrada do IdentityMap ganha uma segunda localização: layout novo de 48
  bytes por entrada — `{ current: (page,slot,gen,flags), current_epoch u64,
  previous: (page,slot,gen,flags), previous_epoch u64 }` (IDMP versão 2; migração:
  regravar o mapa na primeira abertura pós-upgrade).

### Critério de conclusão 6A

Banco v1 migra para IDMP v2 sem alterar objetos, banco v2 reabre sem nova
migração e a época global permanece monotônica através de commits e reabertura.

## Fase 6B — Snapshot e leituras consistentes

- Escrita (update/remove) move `current`→`previous` antes de gravar a nova
  versão; o registro antigo no heap **não** é apagado enquanto houver snapshot
  com `epoch < current_epoch`.
- `Snapshot{ epoch }`: leituras escolhem `current` se
  `current_epoch ≤ snapshot.epoch`, senão `previous` (se também não servir →
  objeto não existia no snapshot).

```cpp
class Snapshot { public: std::uint64_t epoch() const; /* RAII: libera no destrutor */ };
Result<Snapshot> Database::snapshot() const;
Result<DecodedObject> ObjectStore::get(ObjectId, const Snapshot&) const;
Result<void> ObjectStore::scan(const Snapshot&, ...) const;
```

Criações posteriores não aparecem no snapshot; atualizações e remoções
posteriores continuam expondo a versão anterior. Leituras sem `Snapshot`
continuam usando `current`.

Como há somente uma posição `previous`, uma segunda alteração do mesmo objeto
enquanto essa versão estiver visível retorna `snapshot_conflict`; essa proteção
faz parte da primeira entrega pública de snapshots.

### Critério de conclusão 6B

`get` e `scan`, com commits intercalados de forma determinística, devolvem
exatamente o estado lógico da época capturada, e nenhuma sequência aceita
sobrescreve uma versão ainda visível.

## Fase 6C — Retenção, GC e concorrência

- GC: ao fechar o último snapshot antigo (contagem por época em memória),
  registros `previous` obsoletos são apagados do heap e a entrada compactada.
- Um único lock de escritor serializa commits. Abertura/fechamento de snapshots
  e GC sincronizam o registro de épocas sem bloquear a duração completa das
  leituras.

Limitação documentada: **uma** versão anterior por objeto; um escritor que
modifica o mesmo objeto duas vezes enquanto um snapshot antigo está aberto
recebe `snapshot_conflict` (novo ErrorCode) — restrição aceitável para
single-writer, removida somente quando o MVCC completo for planejado.

### Critério de conclusão 6C

Nenhuma versão ainda visível é descartada; ao fechar o último snapshot
dependente, versões obsoletas são recuperadas; uma segunda alteração
incompatível retorna `snapshot_conflict` sem escrita parcial.

## Fase 6D — Integração e recuperação

### Testes automatizados — `tests/snapshot_test.cpp` (`modb.snapshot`)

| Caso | Verificação |
|---|---|
| leitura estável | snapshot; update no objeto; get via snapshot → valor antigo; get corrente → novo |
| remoção invisível | remover objeto após snapshot → snapshot ainda o vê |
| criação invisível | objeto criado após o snapshot não aparece no scan do snapshot |
| scan consistente | scan longo intercalado com commits (interleaving manual) enumera exatamente o estado da época |
| conflito | 2ª modificação do mesmo objeto com snapshot antigo aberto → `snapshot_conflict` |
| GC | fechar o snapshot libera as versões antigas (scan físico não as encontra) |
| reabertura | snapshot não sobrevive ao processo; abertura limpa `previous` órfãos |

Além desses casos, a matriz deve cobrir migração IDMP v1→v2, época após
reabertura, recovery de commits com versões e falhas nos limites entre
publicação de `current`, retenção de `previous` e atualização da época.

### Critério de conclusão 6D

A matriz automatizada passa sem leitura mista, versão perdida, vazamento
persistente ou corrupção após recovery.

## Critério de conclusão da Fase 6

Scan sob snapshot produz estado idêntico ao da época, com commits concorrentes
intercalados no mesmo processo.

---

# Fase 7 — Índices e consultas em streaming (embedded)

## Artefatos novos

```text
include/modb/index/btree.hpp             src/index/btree.cpp
include/modb/query/generator.hpp         (coroutine Generator<T>)
include/modb/query/operators.hpp         src/query/operators.cpp
include/modb/query/planner.hpp           src/query/planner.cpp
tests/btree_test.cpp
tests/generator_test.cpp
tests/streaming_query_test.cpp
tests/planner_test.cpp
```

## Design

### B+ tree

Páginas `BTIN` (interna) e `BTLF` (folha), header 32 bytes
(magic | versão u16 | nível u16 | key_count u16 | reservado | `next_leaf` u64
nas folhas). Chave = encoding canônico do `AttributeValue` (ordenável byte a
byte: tag + big-endian para inteiros com bias de sinal, IEEE-754 com flip de
sinal, strings cruas) + `ObjectId` como desempate (chaves duplicadas
permitidas). Valor = `ObjectId`. Operações: `insert`, `remove`,
`find(key)`, `range(lo, hi)` como cursor; split de folha e interna com
propagação até a raiz; raiz persistida no objeto `IndexDefinition` do
catálogo (campo `root_page`).

### Generator (coroutines C++20)

```cpp
template <typename T>
class Generator { // promise_type mínimo: co_yield T; sem exceções cruzando a fronteira —
public:           // erros fluem como Generator<Result<T>>.
    struct iterator; iterator begin(); std::default_sentinel_t end();
};
```

Sem dependência de `std::generator` (C++23) — implementação própria de ~80
linhas, testada isoladamente. Validar compilação nos três toolchains.

### Operadores

Todos consomem e produzem `Generator<Result<DecodedObject>>` (ou linhas
projetadas), avaliação totalmente preguiçosa:

```cpp
Generator<Result<DecodedObject>> scan(const ObjectStore&, const Snapshot&, TypeDefinitionId);
Generator<Result<DecodedObject>> index_scan(const BTree&, Range, const ObjectStore&, const Snapshot&);
auto filter(Gen, Predicate) -> Gen;      // streaming
auto project(Gen, FieldList) -> Gen;     // streaming
auto limit(Gen, n) -> Gen;               // streaming; PARA o upstream após n (curto-circuito)
auto compute(Gen, ComputedFn) -> Gen;    // streaming (funções registradas)
auto sort(Gen, key) -> Gen;              // BLOQUEANTE: materializa (documentado)
auto top_k(Gen, k, key) -> Gen;          // parcialmente bloqueante: heap de k
auto aggregate(Gen, spec) -> Gen;        // bloqueante
auto distinct(Gen, key) -> Gen;          // bloqueante
```

### Planner

Entrada: descrição da consulta
`{ type, predicate?, projection?, order_by?, limit? }`. Regras (determinísticas,
sem custo estimado por estatística no MVP):

1. Predicado de igualdade/faixa sobre atributo indexado → `index_scan`;
   senão `scan` + `filter`.
2. `order_by` sobre atributo indexado → índice já entrega ordenado (streaming);
   senão `sort` (bloqueante).
3. `limit` presente → empurrado para o mais fundo possível; com `top_k` quando
   há `order_by` não indexado.
4. O plano expõe `nature()` ∈ {streaming, partially_blocking, blocking} e
   `first_result_cost()` (nº estimado de objetos lidos até o 1º resultado) —
   base da métrica TTFR.

Cancelamento: `CancellationToken` (flag atômica) checado pelos operadores a
cada elemento; cancelado → o generator termina limpo (destruição de coroutines
em cadeia).

## Testes automatizados

**`tests/btree_test.cpp`** (`modb.btree`)

| Caso | Verificação |
|---|---|
| inserção ordenada e aleatória | 50 000 chaves; busca de todas |
| invariantes estruturais | após cada 1 000 inserções: profundidade uniforme, ordenação interna, fill mínimo |
| duplicatas | mesma chave, ObjectIds distintos, range devolve todos |
| range | limites inclusivos/exclusivos, faixa vazia |
| remoção | busca falha após remoção; invariantes mantidas |
| reabertura | árvore íntegra e completa após fechar/abrir |
| ordem de tipos mistos | encoding canônico ordena int64/float64/string corretamente por tipo |

**`tests/generator_test.cpp`** (`modb.generator`)

| Caso | Verificação |
|---|---|
| preguiça | generator de contador infinito + `limit(5)` → upstream avança exatamente 5 vezes (contador prova) |
| composição | filter∘project∘limit corretos |
| destruição precoce | abandonar o generator no meio não vaza (ASan/valgrind no preset sanitizers) |

**`tests/streaming_query_test.cpp`** (`modb.streaming_query`)

| Caso | Verificação |
|---|---|
| **TTFR (critério)** | 100 000 objetos; consulta com `limit 1`; instrumentar o ObjectStore com contador de páginas lidas: deve ler ≤ 2 páginas de dados — prova que nada foi materializado |
| memória O(1) | operador `filter+project` sobre 100 000 objetos: nenhum operador guarda mais que K elementos (contadores internos de pico expostos para teste) |
| snapshot no fluxo | consulta longa + commits intercalados → resultados do snapshot |
| cancelamento | cancelar após N resultados → upstream para (contador), sem vazamento |
| operador bloqueante | `sort` global correto; `top_k` usa só k de memória (contador de pico) |

**`tests/planner_test.cpp`** (`modb.planner`)

| Caso | Verificação |
|---|---|
| escolha de índice | predicado indexado → plano com index_scan |
| fallback | não indexado → scan+filter |
| natureza | planos classificados corretamente (streaming/parcial/bloqueante) |
| limit pushdown | limit chega ao operador mais fundo |

## Critério de conclusão

Teste TTFR verde: primeiro resultado com ≤ 2 páginas lidas em 100 k objetos;
buscas por chave comprovadamente via índice.

---

# Fase 8 — Servidor, protocolo binário e backpressure

## Artefatos novos

```text
include/modb/net/native_socket.hpp     src/net/native_socket.cpp   (espelha NativeFile: Win32/POSIX isolados)
include/modb/net/protocol.hpp          src/net/protocol.cpp
include/modb/net/server.hpp            src/net/server.cpp
include/modb/net/client.hpp            src/net/client.cpp
tests/protocol_test.cpp
tests/server_streaming_test.cpp
```

[ADR-010](decisions/ADR-010-protocolo-binario-proximo-do-armazenamento.md):
rede via sockets nativos próprios (`NativeSocket`, mesmo padrão do
`NativeFile`) e protocolo próximo do armazenamento lógico — sem dependência
externa no MVP; asio reavaliado se a complexidade crescer. ADR-011: modelo de concorrência — 1 thread de aceitação
+ 1 thread por conexão no MVP (instância dedicada a uma aplicação; poucas
conexões); o motor continua com um escritor; o `DatabaseRegistry` ganha o lock
que o escopo single-thread dispensava (revisar `ScratchPagePool` etc. sob a
nova premissa — os pontos exatos ficam listados na ADR).

## Protocolo (frames sobre TCP)

Frame físico: `| length u32 | type u8 | payload |` (length cobre type+payload;
máx 16 MiB — frame maior → erro de protocolo). Mensagens:

| Tipo | Nome | Payload |
|---|---|---|
| 1 | `Hello` | versão do protocolo u16; nome do banco (string); codecs de compressão aceitos |
| 2 | `HelloOk` | versão u16; BaselineId u64; codecs e limites selecionados |
| 3 | `Query` | query_id u32 + descrição serializada da consulta |
| 4 | `StreamBegin` | query_id u32 |
| 5 | `ObjectFrame` | query_id u32 + diretório de slots + `ObjectEnvelope`s |
| 6 | `StreamEnd` | query_id u32 + total u64 |
| 7 | `StreamError` | query_id u32 + ErrorCode u16 + mensagem (string) |
| 8 | `Cancel` | query_id u32 |
| 9 | `OpCall` | op_id (string) + argumentos serializados (Fase 9) |
| 10 | `OpResult` | sucesso u8 + payload ou erro |

`ObjectEnvelope`, independente de página física:

```text
| object_id u64 | type_definition_id u64 | payload_length u32 | payload |
```

O payload reutiliza o codec lógico da ADR-003. Não contém `PageId`, `SlotId`,
`RecordId`, generation, offset de arquivo ou header de página.

`ObjectFrame` usa um diretório inspirado em slotted pages:

```text
| query_id u32 | record_count u32 | compression u8 | reservado[3] |
| uncompressed_size u32 | encoded_size u32                         |
| record_count × { offset u32, length u32 }                        |
| encoded_data[encoded_size]                                       |
```

Offsets são relativos ao início da área de dados descomprimida. Intervalos devem
estar dentro de `uncompressed_size`, não podem se sobrepor e preservam a ordem
de produção. O diretório de slots não é comprimido. Um frame com um único slot
é válido. Não existem lotes lógicos: cada objeto é
elegível para envio assim que produzido; o serializer pode apenas coalescer
objetos já disponíveis, sem aguardar quantidade ou tamanho mínimo
(doc streaming §Framing e ADR-010).

`compression=none` é obrigatório e implica
`encoded_size == uncompressed_size`. Outros codecs são anunciados pelo cliente
no `Hello` e selecionados no `HelloOk`. A primeira implementação não mantém
estado nem dicionário entre frames. O servidor só tenta comprimir acima de um
limiar configurável e volta a `none` se não houver redução material. O receptor
valida tamanhos, razão máxima de expansão e codec negociado antes de alocar ou
descomprimir.

### Backpressure

O laço de envio escreve um `ObjectFrame` limitado no socket **bloqueante**;
quando o cliente não consome, `send` bloqueia → o generator não avança → scan suspenso.
A propagação é natural porque o pipeline é preguiçoso (Fase 7) — não há fila
intermediária além de 1 objeto em trânsito. Teste comprova.

### Cliente

```cpp
class Client {
public:
    static Result<Client> connect(std::string_view host, std::uint16_t port);
    Result<ObjectStream> query(QueryDescription);
    // ObjectStream::next() -> Result<std::optional<DecodedObject>>; nullopt = StreamEnd.
    Result<void> cancel(QueryId);
};
```

## Testes automatizados

**`tests/protocol_test.cpp`** (`modb.protocol`) — sem rede: codificação de
frames em buffers

| Caso | Verificação |
|---|---|
| round-trip de cada mensagem | encode→decode idêntico |
| frame truncado / length mentiroso / >16 MiB | erros específicos, sem alocação gigante |
| compressão negociada | frame compressível → round-trip; mesmo conteúdo lógico que `none` |
| frame pequeno/incompressível | enviado como `none`, sem expansão inútil |
| compressão inválida | codec não negociado, stream truncado, tamanho ou razão de expansão inválidos → `protocol_error` sem alocação excessiva |
| lixo | bytes aleatórios → erro de protocolo, nunca crash (base p/ fuzzing F10) |

**`tests/server_streaming_test.cpp`** (`modb.server_streaming`) — servidor em
thread + cliente no mesmo processo de teste, porta efêmera de loopback

| Caso | Verificação |
|---|---|
| fluxo completo | 10 000 objetos: Begin → `ObjectFrame`(s) → End; conteúdo íntegro e ordem preservada |
| diretório inválido | offset fora da área de dados, sobreposição ou envelope truncado → `protocol_error` |
| independência física | nenhum frame contém `PageId`/`SlotId`/`RecordId`; realocação física não altera bytes lógicos do objeto |
| **backpressure (critério)** | cliente consome 1 obj/50 ms com janela TCP pequena: instrumentar o servidor com contador de objetos produzidos − enviados ≤ pequena constante (produção acompanha consumo; sem acúmulo) |
| cancelamento | Cancel no meio → produção para (contador), conexão utilizável para nova consulta |
| desconexão abrupta | fechar socket no meio do fluxo → servidor libera cursor/snapshot sem vazar (contadores de recursos) |
| erro no meio do fluxo | falha injetada após N objetos → cliente recebe exatamente N objetos + `StreamError` |
| duas consultas concorrentes | interleaving de dois query_ids na mesma conexão, ambos íntegros |

## Critério de conclusão

Teste de backpressure verde: com cliente lento, o servidor comprova produção
casada ao consumo, sem crescimento de memória.

---

# Fase 9 — Runtime de módulos de domínio

## Artefatos novos

```text
include/modb/ops/operation.hpp
include/modb/ops/execution_context.hpp
include/modb/ops/operation_registry.hpp   src/ops/operation_registry.cpp
include/modb/ops/module_manifest.hpp      src/ops/module_manifest.cpp
include/modb/ops/module_loader.hpp        src/ops/module_loader.cpp
examples/transfer_funds/                   (módulo exemplo completo)
tests/operation_test.cpp
tests/operation_server_test.cpp
```

## Design

```cpp
class ExecutionContext { // ÚNICA porta de entrada (doc codigo-local §ExecutionContext)
public:
    Transaction& transaction();
    ObjectAccess& objects();   // fachada: get/create/remove/query — nada de páginas/WAL/índices
    Logger& logger();
};

struct OperationResult { std::vector<std::byte> payload; }; // codec ADR-003

class Operation {
public:
    virtual ~Operation() = default;
    // Result em vez de exceção na fronteira do motor; exceções que escapem
    // do código de domínio são capturadas pelo runtime -> rollback + OpResult de erro.
    virtual Result<OperationResult> execute(ExecutionContext&) = 0;
};

class OperationRegistry {
public:
    template <typename Op>
    Result<void> register_operation(std::string id);   // id ex.: "account.transfer"
    Result<OperationResult> dispatch(std::string_view id,
                                     std::span<const std::byte> args,
                                     Database&);        // begin -> execute -> commit/rollback
};

struct ModuleManifest { ModuleId id; std::uint32_t module_version;
                        BaselineId baseline; std::uint32_t api_version;
                        BinaryHash hash; std::vector<ExportedMethod> methods; };
// Na carga: api_version == corrente do motor; baseline compatível com a do
// banco (igual, ou marcada como migradora), hash admitido e exports válidos
// — senão ErrorCode::incompatible_module.
```

Contrato transacional do `dispatch` (doc codigo-local §Commit): sucesso →
commit; `Result` de erro **ou** exceção capturada → rollback. Conforme a
[ADR-012](decisions/ADR-012-runtime-de-modulos-no-processo.md), módulos no
primeiro runtime são carregados **dentro do processo** a partir de uma origem
confiável configurada pelo operador. O `ModuleLoader` valida id, versão da API,
baseline, exports e hash antes do registro; o cliente nunca envia binários nem
escolhe caminhos. Sandbox, workers isolados e atualização a quente ficam para
avaliação posterior. Migrações usam a mesma infraestrutura: uma
`MigrationOperation` registrada e despachada como qualquer operação.

Consultas permanecem internas ao motor. `ObjectAccess::query<T>()` pode usar
índices, planejamento e streaming, mas não existe interface SQL pública. Métodos
`read_only` recebem `Snapshot`; métodos `read_write`, `Transaction`. Argumentos
e resultados sempre usam o codec versionado, sem ponteiros ou dependência do
layout C++ em memória, preservando uma futura fronteira para workers.

Protocolo: mensagens `OpCall`/`OpResult` da Fase 8;
`client.call<TransferFunds>(args...)` serializa argumentos pelo codec.

### Exemplo obrigatório: `TransferFunds`

Reproduz o exemplo do documento: `Account{owner, balance}`;
`TransferFunds{source, destination, amount}` → valida saldo → debita → credita.
Saldo insuficiente → erro → rollback comprovado.

## Testes automatizados

**`tests/operation_test.cpp`** (`modb.operation`) — sem rede

| Caso | Verificação |
|---|---|
| dispatch feliz | TransferFunds move o saldo; committed (visível em nova leitura) |
| erro de domínio | saldo insuficiente → OpResult de erro; **nenhum** saldo alterado |
| exceção do módulo | operação que lança `std::runtime_error` → rollback + erro; motor segue utilizável |
| id desconhecido | dispatch("nao.existe") → `operation_not_found` |
| manifesto incompatível | api_version divergente → `incompatible_module` na carga |
| origem/hash não admitido | loader rejeita antes de registrar ou executar qualquer método |
| migração como operação | MigrationOperation converte objetos v1→v2 dentro de uma transação; falha no meio → nada migrado |

**`tests/operation_server_test.cpp`** (`modb.operation_server`)

| Caso | Verificação |
|---|---|
| **critério da fase** | via `client.call<TransferFunds>` pela rede: transferência atômica; saldo insuficiente → erro no cliente e saldos intactos |
| crash + recovery | derrubar o servidor (kill do processo de teste filho) após commit no WAL; subir de novo; transferência presente — documenta o ciclo supervisor → restart → WAL recovery |

## Critério de conclusão

`TransferFunds` atômico de ponta a ponta pela rede, com rollback comprovado e
recuperação pós-crash.

---

# Fase 10 — Desempenho e estabilização

## Frentes e protocolo

1. **Benchmarks** (`benchmarks/`, alvo CMake separado, nunca no ctest): seguir
   integralmente [PLANO_BENCHMARKS.md](PLANO_BENCHMARKS.md). O runner cobre
   todas as camadas, preserva amostras brutas e gera um único JSONL autocontido
   por campanha: `modb-benchmark-YYYYMMDDTHHMMSS.mmmZ-<commit>-<host>.jsonl`.
   TTFR, throughput, p99, CPU, memória, I/O, espaço, rede e correção são métricas
   de primeira classe; datasets, seeds, ambiente e configuração acompanham os
   resultados.
2. **BufferPool completo**: evoluir `PageCache` — capacidade configurável,
   LRU, pin/unpin, write-back integrado ao WAL, métricas (hits/misses/
   evictions). Testes: banco 10× maior que o cache mantém corretude
   (`modb.buffer_pool`); benchmark antes/depois.
3. **Fuzzing** (preset `fuzz`, clang/libFuzzer): alvos para `decode_object`,
   decodificação de TypeDefinition, cadeia de blob, frames do protocolo e
   registros do WAL. Corpus mínimo em `tests/fuzz/corpus/`. Meta: 1 h de fuzz
   por alvo sem crash/OOM/UB antes do release.
4. **Otimizações medidas**: só com profiling antes/depois registrado
   (ex.: append real no PersistentVector, batch de escrita no WAL,
   `insertion_capacity` incremental). Nenhuma otimização sem número.
5. **Documentação final**: reescrever `README.md` (exemplo OO completo),
   `FORMATO_DE_ARQUIVO.md` (todas as páginas: DBRT/IDMD/IDMP/BLBP/BTIN/BTLF/
   WAL), documentação da API pública, guia de operação (supervisor, backup =
   cópia de `<db>` + `<db>.wal` quiescente, restauração, `modb db check`).
6. **Política de compatibilidade**: versionamento do formato (major no
   superbloco, minor no DBRT) e do protocolo (no Hello); regra: minor = só
   aditivo; major = recusa com mensagem clara.

## Critério de conclusão

Benchmarks reproduzíveis comparáveis entre execuções; fuzzing limpo; banco
maior que o cache correto; documentação completa; suíte inteira verde nos
três presets.

---

# Apêndice A — Mapa de ErrorCodes novos por fase

| Fase | ErrorCode |
|---|---|
| 1 | `duplicate_field`, `field_not_found`, `duplicate_type`, `type_not_found`, `invalid_object_id` |
| 3 | `incompatible_projection`, `binding_mismatch` |
| 5 | `transaction_required`, `transaction_active`, `wal_corrupt` |
| 6 | `snapshot_conflict` |
| 8 | `protocol_error`, `frame_too_large`, `connection_closed` |
| 9 | `operation_not_found`, `incompatible_module` |

# Apêndice B — Mapa de páginas do formato

| Magic | Página | Fase |
|---|---|---|
| (superbloco `MODB`) | metadados do arquivo | existente |
| `THRP` | raiz de TableHeap | existente |
| `SLPG` | dados slotted (registros) | existente |
| `DBRT` | raiz do banco OO | 2 |
| `IDMD` | diretório do mapa de identidade | 2 |
| `IDMP` | entradas do mapa de identidade | 2 (v2 na fase 6) |
| `BLBP` | página de blob | 4 |
| `BTIN`/`BTLF` | B+ tree interna/folha | 7 |
| `MOWL` | header do arquivo WAL | 5 |

# Apêndice C — Ordem de leitura para um novo implementador

1. [arquitetura.md](../arquitetura.md), [codigo-local.md](../codigo-local.md),
   [streaming.md](../streaming.md) — a visão.
2. [PLANO_ODB.md](PLANO_ODB.md) — as fases e por quê nesta ordem.
3. Este documento, na fase que for implementar.
4. Código existente citado pela fase (começar por `error.hpp`, `page.hpp`,
   `slotted_page.hpp`, `table_heap.hpp`).
