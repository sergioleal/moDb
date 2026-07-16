#pragma once

namespace modb::detail {

// Combina várias lambdas em um único objeto chamável para std::visit.
// Usado para percorrer std::variant de forma exaustiva: se uma alternativa nova
// for adicionada e não tratada, o código deixa de compilar em vez de lançar
// std::bad_variant_access em tempo de execução.
template <typename... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};

// Deduz automaticamente os tipos das lambdas passadas.
template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

} // namespace modb::detail
