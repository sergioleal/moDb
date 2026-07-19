#pragma once

// Manifesto e carga de módulos de domínio no processo (Fase 9 / ADR-012).
// Origem administrativa + hash; o cliente nunca envia binários.

#include "modb/error.hpp"
#include "modb/object/ids.hpp"
#include "modb/ops/facade_catalog.hpp"
#include "modb/ops/facade_descriptor.hpp"
#include "modb/ops/operation.hpp"
#include "modb/ops/operation_registry.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace modb::ops {

inline constexpr std::uint32_t runtime_api_version = 1;

using ModuleId = std::string;
using BinaryHash = std::string; // hex SHA-like token; no MVP usa digest estável do manifesto

struct ExportedMethod {
    std::string id{};
    OperationMode mode{OperationMode::read_write};

    friend bool operator==(const ExportedMethod&, const ExportedMethod&) = default;
};

struct ModuleManifest {
    ModuleId id{};
    std::uint32_t module_version{1};
    object::BaselineId baseline{};
    std::uint32_t api_version{runtime_api_version};
    BinaryHash hash{};
    std::vector<ExportedMethod> methods{};
    // Facades derivadas do módulo (Fase 11D). Vazio → só operações planas (Fase 9).
    std::vector<FacadeDescriptor> facades{};
    bool migration{false}; // true → baseline divergente permitida (migradora)

    friend bool operator==(const ModuleManifest&, const ModuleManifest&) = default;
};

// Calcula um hash estável do manifesto (conteúdo lógico, sem o campo hash).
[[nodiscard]] BinaryHash compute_manifest_hash(const ModuleManifest& manifest);

// Registra facades do manifesto no catálogo (métodos ⊆ exports e ∈ registry).
[[nodiscard]] Result<void> register_facades_from_manifest(const ModuleManifest& manifest,
                                                          const OperationRegistry& operations,
                                                          FacadeCatalog& catalog);

class ModuleLoader {
public:
    void admit_hash(BinaryHash hash) { allowlist_.insert(std::move(hash)); }
    [[nodiscard]] bool is_admitted(std::string_view hash) const {
        return allowlist_.contains(std::string{hash});
    }

    // Valida manifesto e registra as factories fornecidas pelo módulo.
    using Registrar = std::function<Result<void>(OperationRegistry&)>;

    [[nodiscard]] Result<void> load(const ModuleManifest& manifest,
                                    object::BaselineId database_baseline,
                                    OperationRegistry& registry, Registrar registrar);

    // Como load(), e depois registra facades do manifesto no catálogo (Fase 11D).
    [[nodiscard]] Result<void> load(const ModuleManifest& manifest,
                                    object::BaselineId database_baseline,
                                    OperationRegistry& registry, FacadeCatalog& catalog,
                                    Registrar registrar);

private:
    std::unordered_set<std::string> allowlist_;
};

} // namespace modb::ops
