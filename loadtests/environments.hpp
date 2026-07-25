#pragma once

// Catálogo de ambientes registrados (docs/PLANO_TESTES_DE_CARGA.md §4.4).
// Carrega e valida loadtests/environments.json; resolve --environment.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace modb::loadtest {

struct EnvironmentConnection {
    std::string host;
    std::string default_user;
    std::string remote_work_dir;
    std::string binary_name;
};

struct EnvironmentEntry {
    std::string id;
    std::string label;
    std::string kind;         // "local" | "ssh"
    std::string host_class;   // rótulo de comparabilidade (§13.4)
    std::string os_hint;
    std::string notes;
    bool deprecated{false};
    std::optional<EnvironmentConnection> connection;   // presente só para kind == "ssh"
};

struct EnvironmentCatalog {
    std::vector<EnvironmentEntry> environments;

    [[nodiscard]] const EnvironmentEntry* find(std::string_view id) const;
};

struct LoadCatalogResult {
    bool ok{false};
    EnvironmentCatalog catalog;
    std::string error;
};

// Lê e valida o catálogo. Erro de schema (campo obrigatório ausente, `kind`
// desconhecido) é falha explícita -- nunca ignorado em silêncio.
[[nodiscard]] LoadCatalogResult load_environment_catalog(const std::filesystem::path& path);

struct ValidateEnvironmentResult {
    bool ok{false};
    std::string error;      // id desconhecido: mensagem inclui os ids cadastrados
    std::string warning;    // não vazio quando kind == "ssh" e o alvo é local (não bloqueia)
};

// Confere se `environment_id` existe no catálogo. `local_only` marca que o
// chamador só executa localmente (como os workloads embedded desta subfase),
// caso em que kind == "ssh" vira aviso, não erro.
[[nodiscard]] ValidateEnvironmentResult validate_environment(const EnvironmentCatalog& catalog,
                                                             std::string_view environment_id,
                                                             bool local_only);

} // namespace modb::loadtest
