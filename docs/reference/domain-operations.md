# Remote Domain Operations

> Part of the `docs/reference/` set — see [docs/README.md](../README.md) for
> the full index and [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md) for a
> narrative, tutorial-style introduction. This document is a precise
> reference: it assumes you already know C++ well and want exact behavior,
> not a learning path.

## 1. Overview

An `Operation` is business logic that runs **inside the server process**,
next to the data, callable by a string id instead of shipping a query. This
is the mechanism behind both a plain remote procedure call
(`ServerConnection::call`) and, one layer up, [facades](../FACADES.md). The
model is deliberately restrictive: the client never ships code, only
arguments; the server only runs operations whose module manifest hash is on
an explicit allowlist.

## 2. Concepts

### 2.1 The `Operation` contract

```cpp
// include/modb/ops/operation.hpp:16-42
struct OperationResult {
    std::vector<std::byte> payload{};
};

enum class OperationMode : std::uint8_t { read_only = 0, read_write = 1 };

class Operation {
public:
    virtual ~Operation() = default;
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    [[nodiscard]] virtual OperationMode mode() const noexcept { return OperationMode::read_write; }
    // Engine boundary: prefer Result. Exceptions that escape are caught by
    // the registry -> rollback + error.
    [[nodiscard]] virtual Result<OperationResult> execute(ExecutionContext& context) = 0;
};

using OperationFactory = Result<std::unique_ptr<Operation>> (*)(std::span<const std::byte> args);
```

`mode()` is metadata, not enforcement by itself — it's what
`ModuleManifest`/`ExportedMethod` and the facade layer use to reason about
which calls are read-only versus writing, ahead of actually running
anything.

### 2.2 Registering and dispatching

```cpp
// include/modb/ops/operation_registry.hpp:17-49
class OperationRegistry {
public:
    Result<void> register_factory(std::string id, OperationFactory factory,
                                  OperationMode mode = OperationMode::read_write);

    template <typename Op>
    Result<void> register_operation(std::string id) {
        return register_factory(std::move(id),
            [](std::span<const std::byte> args) { return Op::decode(args); }, Op::k_mode);
    }

    [[nodiscard]] bool contains(std::string_view id) const;
    Result<OperationResult> dispatch(std::string_view id, std::span<const std::byte> args,
                                     object::Database& database);
};
```

`register_operation<Op>` is the ergonomic path: it expects your `Op` type to
declare a static `Op::decode(args) -> Result<std::unique_ptr<Operation>>`
and a static `Op::k_mode`. `dispatch()` is the whole story in one call:
begin a transaction → construct the `Operation` via its factory → call
`execute()` → commit on success. Any error `Result` **or thrown exception**
from `execute()` rolls the transaction back.

### 2.3 `ExecutionContext` — the operation's only door into the database

```cpp
// include/modb/ops/execution_context.hpp
class ExecutionContext {
public:
    [[nodiscard]] ObjectAccess& objects() noexcept;
    [[nodiscard]] Logger& logger() noexcept;
    [[nodiscard]] object::Transaction& transaction(); // only valid in read_write mode
    [[nodiscard]] bool writable() const noexcept;
};
```

An `Operation::execute` never touches a `Database` directly — everything
goes through `context.objects()` (an `ObjectAccess`, which internally wraps
the active `Handle`/`Transaction` machinery from
[reference/handles.md](handles.md)) and `context.logger()`. This is a
deliberate narrow interface (ADR-012): the module author's surface area is
"read/write objects and log," not "arbitrary access to engine internals."

### 2.4 Admission control: `ModuleManifest` and the hash allowlist

```cpp
// include/modb/ops/module_manifest.hpp
struct ExportedMethod { std::string id{}; OperationMode mode{OperationMode::read_write}; };

struct ModuleManifest {
    ModuleId id{};
    std::uint32_t module_version{1};
    object::BaselineId baseline{};
    std::uint32_t api_version{runtime_api_version}; // = 1
    BinaryHash hash{};
    std::vector<ExportedMethod> methods{};
    std::vector<FacadeDescriptor> facades{}; // empty -> plain operations only
    bool migration{false};
};

BinaryHash compute_manifest_hash(const ModuleManifest& manifest);

class ModuleLoader {
public:
    void admit_hash(BinaryHash hash);
    [[nodiscard]] bool is_admitted(std::string_view hash) const;
    using Registrar = std::function<Result<void>(OperationRegistry&)>;
    Result<void> load(const ModuleManifest& manifest, object::BaselineId database_baseline,
                      OperationRegistry& registry, Registrar registrar);
    Result<void> load(const ModuleManifest& manifest, object::BaselineId database_baseline,
                      OperationRegistry& registry, FacadeCatalog& catalog, Registrar registrar);
};
```

The client never ships binaries or code — only `id`/`args` pairs sent to
already-loaded operations. Server-side setup computes a content hash over
the manifest (`compute_manifest_hash`), and `ModuleLoader::admit_hash` is
the explicit allowlist gate: `load()` is expected to check the manifest's
hash is admitted before invoking your `Registrar` to actually register
factories. Changing which methods/facades a module exports changes its
hash — you must update the allowlist deliberately, it doesn't happen
implicitly.

## 3. API Reference

### 3.1 `Operation` (see §2.1)

### 3.2 `OperationRegistry` (see §2.2)

### 3.3 `ExecutionContext` / `ObjectAccess` (see §2.3)

### 3.4 `ModuleManifest` / `ModuleLoader` (see §2.4)

### 3.5 Calling a registered operation remotely

`ServerConnection::call(operation_id, args)` — see
[reference/networking-protocol.md §3.2](networking-protocol.md). Arguments
are hand-rolled binary, not reflection-based (`storage::BinaryWriter`/
`BinaryReader`), matching whatever `encode_args`/`decode` your `Operation`
defines.

## 4. Semantics & Invariants

- `dispatch()` always runs begin → `execute()` → commit-or-rollback; there
  is no way to call an operation outside a transaction.
- A thrown exception from `execute()` is caught by the registry and turned
  into a rollback + error `Result` — you don't need (and shouldn't rely on)
  your own top-level `try`/`catch` inside `execute()` for this.
- `mode()`/`ExportedMethod::mode` is descriptive metadata used by the
  manifest/facade layer; it does not itself change what `execute()` is
  allowed to call — validate your own read-only assumption if you declare
  `read_only`.
- A module's manifest `hash` covers its exported surface (methods and
  facades); the allowlist check is explicit and must be updated whenever
  that surface changes.
- Arguments are opaque bytes to the registry — validation of their shape is
  entirely `Op::decode`'s responsibility (see `TransferFunds::decode`
  rejecting trailing bytes, §6).

## 5. Common Pitfalls

- **Validating business rules after mutating state instead of before.**
  `TransferFunds::execute` (§6) checks `amount_ <= 0` and
  `source_ == destination_` *before* touching either account — do the same;
  a `Result` failure rolls back the transaction, but only the transaction,
  not any manual state you kept outside it.
- **Assuming `execute()` throwing is unsafe.** It's explicitly handled
  (rollback), but prefer returning a `Result` error for expected domain
  failures (like insufficient funds) — reserve exceptions for genuinely
  exceptional situations.
- **Forgetting to update the allowlist hash after changing a module's
  exported surface.** The old hash becomes inadmissible (by design), not a
  silent pass-through.
- **Calling `context.transaction()` in a `read_only`-declared operation.**
  It's only meaningful/valid when the context is writable — check
  `context.writable()` first if your operation's mode isn't fixed.

## 6. Worked Example

Full round trip, from `examples/transfer_funds/` and
`examples/server/by_phase/phase_09/call_operation.cpp`:

```cpp
// --- The operation itself ---
class TransferFunds final : public ops::Operation {
public:
    static constexpr std::string_view k_id = "account.transfer";
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_write;

    static Result<std::vector<std::byte>> encode_args(ObjectId source, ObjectId destination,
                                                       std::int64_t amount);
    static Result<std::unique_ptr<Operation>> decode(std::span<const std::byte> args);

    Result<OperationResult> execute(ExecutionContext& context) override {
        if (amount_ <= 0) return std::unexpected(Error{ErrorCode::invalid_argument, "transfer amount must be positive"});
        if (source_.value == destination_.value) return std::unexpected(Error{ErrorCode::invalid_argument, "source and destination must differ"});

        auto& access = context.objects();
        auto source_handle = access.get<Account>(source_);
        auto dest_handle = access.get<Account>(destination_);
        auto source_account = access.materialize(*source_handle);
        auto dest_account = access.materialize(*dest_handle);

        if (source_account->balance < amount_) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "insufficient funds"});
        }
        source_account->balance -= amount_;
        dest_account->balance += amount_;
        access.update(*source_handle, *source_account);
        access.update(*dest_handle, *dest_account);

        storage::BinaryWriter payload;
        payload.write_u64(static_cast<std::uint64_t>(amount_));
        return OperationResult{.payload = std::move(payload).take()};
    }
    // ...
};

// --- Manifest + registration, server-side ---
ops::ModuleManifest transfer_funds_manifest(object::BaselineId baseline) {
    return ops::ModuleManifest{
        .id = "transfer_funds", .module_version = 1, .baseline = baseline,
        .api_version = ops::runtime_api_version,
        .methods = {ops::ExportedMethod{.id = std::string{TransferFunds::k_id}, .mode = TransferFunds::k_mode}},
    };
}
Result<void> register_transfer_funds_module(ops::OperationRegistry& registry) {
    return registry.register_operation<TransferFunds>(std::string{TransferFunds::k_id});
}

// server setup:
auto registry = std::make_shared<ops::OperationRegistry>();
ops::ModuleLoader loader;
const auto baseline = server->database().current_baseline()->id();
const auto manifest = transfer_funds_manifest(baseline);
loader.admit_hash(manifest.hash);
loader.load(manifest, baseline, *registry, register_transfer_funds_module);
server->set_operation_registry(registry);

// --- Client-side call ---
auto args = TransferFunds::encode_args(alice, bob, 40);
auto result = connection->call(TransferFunds::k_id, *args);
```

## 7. Related Documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 10
- [FACADES.md](../FACADES.md) — the versioned surface layered on top of
  operations
- [OPERACAO_MODULOS.md](../OPERACAO_MODULOS.md) — failure model of the
  module runtime
- [reference/networking-protocol.md](networking-protocol.md) — `call`,
  `set_operation_registry`
- [ADR-012](../decisions/ADR-012-runtime-de-modulos-no-processo.md)

## 8. Related Source

- `include/modb/ops/operation.hpp`, `operation_registry.hpp`,
  `execution_context.hpp`, `object_access.hpp`, `module_manifest.hpp`
- `examples/transfer_funds/transfer_funds.hpp`, `.cpp`
- `examples/server/by_phase/phase_09/call_operation.cpp`
- `tests/operation_test.cpp`, `tests/operation_server_test.cpp`
