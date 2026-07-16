// Importa as declarações das funções de versão.
#include "modb/version.hpp"

namespace modb {

// Retorna uma referência leve para o nome fixo do projeto.
std::string_view project_name() noexcept { return "moDb"; }

// Retorna uma referência leve para a versão atual.
std::string_view project_version() noexcept { return "0.1.0"; }

} // namespace modb
