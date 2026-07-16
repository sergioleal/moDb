#pragma once

// Disponibiliza uma visualização leve das constantes de texto.
#include <string_view>

namespace modb {

// Retorna o nome público do projeto.
[[nodiscard]] std::string_view project_name() noexcept;
// Retorna a versão atual do projeto.
[[nodiscard]] std::string_view project_version() noexcept;

} // namespace modb
