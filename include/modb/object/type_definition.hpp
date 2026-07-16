#pragma once

// Importa AttributeType e AttributeValue, usados pelos atributos e defaults.
#include "modb/object/attribute_value.hpp"
// Importa Result e os códigos de erro de validação.
#include "modb/error.hpp"
// Importa FieldId e TypeDefinitionId.
#include "modb/object/ids.hpp"

// Disponibiliza std::optional para o valor default, que pode não existir.
#include <optional>
// Disponibiliza uma visão sem cópia dos atributos.
#include <span>
// Disponibiliza o armazenamento dos nomes.
#include <string>
// Disponibiliza uma visão leve usada nas buscas por nome.
#include <string_view>
// Disponibiliza std::pair usado em FieldValues.
#include <utility>
// Disponibiliza o contêiner que possui os atributos e os pares de FieldValues.
#include <vector>

namespace modb::object {

// Declarada aqui para que TypeDefinition possa conceder acesso ao construtor
// que atribui o id (ver Fase 2: a atribuição real vem do ObjectStore).
class TypeRegistry;

// Descreve um único atributo de um tipo.
struct AttributeDefinition {
    // Identifica o atributo de forma estável dentro do tipo.
    FieldId id;
    // Nome usado para localizar o atributo.
    std::string name;
    // Tipo de valor que o atributo aceita.
    AttributeType type{AttributeType::null};
    // Informa se o atributo aceita o valor ausente (tag null).
    bool nullable{true};
    // Valor usado quando um objeto mais antigo não possui este atributo
    // (ProjectionPlan::Default, Fase 3).
    std::optional<AttributeValue> default_value;
    // Informa se o atributo é uma coleção (PersistentVector/Set/Map, Fase 4).
    bool is_collection{false};
    // Informa se o atributo é um objeto embutido sem identidade própria
    // (Embedded<T>, Fase 4).
    bool is_embedded{false};
    // Informa se a referência é de composição: remover o pai remove o filho
    // em cascata (OwnedRef<T>, Fase 4).
    bool is_owned{false};

    // Permite comparar definições em testes.
    friend bool operator==(const AttributeDefinition&, const AttributeDefinition&) = default;
};

// Descreve um tipo de objeto de forma imutável (ADR-006, arquitetura.md §17).
//
// Diferente do Schema relacional, um TypeDefinition não exige ao menos um
// atributo: um tipo sem atributos é degenerado mas válido (marcador puro).
class TypeDefinition {
public:
    // Cria um TypeDefinition validado e ainda não registrado (id() == {0}).
    // A atribuição do id acontece no registro (ver TypeRegistry::register_type;
    // a partir da Fase 2 o id vem da alocação persistente de ObjectId).
    [[nodiscard]] static Result<TypeDefinition> create(std::string name,
                                                       std::vector<AttributeDefinition> attributes);

    // Retorna o identificador do tipo, ou {0} se ainda não foi registrado.
    [[nodiscard]] TypeDefinitionId id() const noexcept { return id_; }
    // Retorna o nome do tipo.
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    // Expõe os atributos somente para leitura e sem cópia.
    [[nodiscard]] std::span<const AttributeDefinition> attributes() const noexcept {
        return attributes_;
    }
    // Procura um atributo pelo FieldId; nullptr quando ausente.
    [[nodiscard]] const AttributeDefinition* find(FieldId id) const noexcept;
    // Procura um atributo pelo nome; nullptr quando ausente.
    [[nodiscard]] const AttributeDefinition* find(std::string_view name) const noexcept;

    // Permite comparar definições em testes.
    friend bool operator==(const TypeDefinition&, const TypeDefinition&) = default;

private:
    // Somente o registro que atribui identidade pode estampar um id real —
    // hoje o TypeRegistry em memória; a partir da Fase 2, o ObjectStore.
    friend class TypeRegistry;

    // Constrói a versão ainda não registrada (id {0}); usado por create().
    explicit TypeDefinition(std::string name, std::vector<AttributeDefinition> attributes)
        : name_{std::move(name)}, attributes_{std::move(attributes)} {}
    // Estampa um id sobre uma definição já validada, sem revalidar nada.
    TypeDefinition(TypeDefinitionId id, TypeDefinition unassigned) noexcept
        : id_{id},
          name_{std::move(unassigned.name_)},
          attributes_{std::move(unassigned.attributes_)} {}

    // {0} até o tipo ser registrado.
    TypeDefinitionId id_{};
    // Nome estável do tipo.
    std::string name_;
    // Atributos na ordem declarada.
    std::vector<AttributeDefinition> attributes_;
};

// Lista de pares (FieldId, valor) que representa um objeto lógico antes de
// ser codificado — a entrada de validate_object e, na Fase 2, do codec.
using FieldValues = std::vector<std::pair<FieldId, AttributeValue>>;

// Confere se um conjunto de campos respeita a TypeDefinition: todo FieldId
// existe no tipo, sem duplicatas, tipos compatíveis, e nenhum atributo fica
// sem valor a menos que seja nullable ou possua default.
[[nodiscard]] Result<void> validate_object(const TypeDefinition& type, const FieldValues& fields);

} // namespace modb::object
