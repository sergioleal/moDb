#pragma once

// Perfis de campanha (docs/PLANO_TESTES_DE_CARGA.md §6.2): conjuntos
// pré-definidos de casos, ponto de partida da composição de seletores (§6.1).
// Um perfil pode incluir workloads sem dispatch implementado ainda -- isso é
// esperado (`list-cases` continua funcionando; `run` reporta cada caso assim
// como "unimplemented", não trava a campanha inteira).

#include "matrix.hpp"

#include <optional>
#include <string>
#include <vector>

namespace modb::loadtest {

struct Profile {
    std::string name;
    std::vector<Case> cases;
};

[[nodiscard]] const std::vector<std::string>& list_profile_names();
[[nodiscard]] std::optional<Profile> find_profile(std::string_view name);

} // namespace modb::loadtest
