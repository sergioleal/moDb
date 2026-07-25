#pragma once

// Struct C++ + Binding do dataset `user_v1` (docs/PLANO_TESTES_DE_CARGA.md
// §7) contra o motor de verdade. Compartilhado entre `target_embedded.cpp`
// (cria/lê direto num `Database` local) e `target_client.cpp`/
// `loadtest_facade.cpp` (Subfase G: mesmo tipo, dos dois lados de um
// `Server`/`Client` em loopback) -- as duas execuções precisam bindar
// EXATAMENTE o mesmo `Binding` para que os hashes lógicos sejam comparáveis.

#include "dataset_user.hpp"

#include "modb/object/database.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace modb::loadtest {

struct User {
    std::int64_t id{};
    std::string login;
    std::string email;
    std::string display_name;
    std::int64_t created_at{};
    std::int32_t status{};
    std::vector<std::byte> filler;
};

[[nodiscard]] object::BindingBuilder<User> user_binding();

[[nodiscard]] User to_engine_user(const GeneratedUser& g);
[[nodiscard]] GeneratedUser from_engine_user(const User& u);

} // namespace modb::loadtest
