#pragma once

// Importa Result e os códigos de erro.
#include "modb/error.hpp"
// Importa TypeDefinitionId.
#include "modb/object/ids.hpp"
// Importa TypeDefinition, o valor mantido pelo registro.
#include "modb/object/type_definition.hpp"

// Disponibiliza inteiros com largura fixa para o contador de ids.
#include <cstdint>
// Disponibiliza std::reference_wrapper para retornar tipos sem copiá-los.
#include <functional>
// Disponibiliza o armazenamento dos nomes usados como chave do mapa.
#include <string>
// Disponibiliza uma visão leve usada nas buscas por nome.
#include <string_view>
// Disponibiliza o mapa que relaciona nomes ao id mais recente.
#include <unordered_map>

namespace modb::object {

// Mantém, em memória, o conjunto de tipos conhecidos e atribui seus
// TypeDefinitionId.
//
// A partir da Fase 2, a alocação real de ObjectId (e portanto de
// TypeDefinitionId) passa a ser feita pelo ObjectStore persistente, seguindo
// a mesma regra monotônica a partir de first_user_object_id (ADR-002). Este
// registro simula essa regra em memória para permitir testar o modelo de
// tipos antes de existir persistência.
class TypeRegistry {
public:
    // Registra um tipo ainda não persistido, atribuindo seu TypeDefinitionId.
    // Rejeita um nome já registrado.
    [[nodiscard]] Result<TypeDefinitionId> register_type(TypeDefinition definition);
    // Procura um tipo pelo identificador.
    [[nodiscard]] Result<std::reference_wrapper<const TypeDefinition>> find(
        TypeDefinitionId id) const;
    // Procura um tipo pelo nome, retornando sua versão mais recente.
    [[nodiscard]] Result<std::reference_wrapper<const TypeDefinition>> find(
        std::string_view name) const;

private:
    // Relaciona cada id atribuído ao seu TypeDefinition completo.
    std::unordered_map<std::uint64_t, TypeDefinition> types_by_id_;
    // Relaciona cada nome ao id da sua versão mais recente.
    std::unordered_map<std::string, std::uint64_t> latest_id_by_name_;
    // Próximo id a ser atribuído; começa onde o espaço de usuário começa.
    std::uint64_t next_id_{first_user_object_id};
};

} // namespace modb::object
