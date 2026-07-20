#pragma once

// Disponibiliza inteiros com largura fixa para os identificadores.
#include <array>
#include <cstdint>

namespace modb::object {

// Identifica um objeto permanentemente; nunca muda mesmo que o objeto mude de
// página. Monotônico e nunca reutilizado (ADR-001).
struct ObjectId {
    std::uint64_t value{};

    // Permite comparar identificadores diretamente.
    friend bool operator==(ObjectId, ObjectId) = default;
};

// Identifica um atributo dentro de um tipo; único por tipo, nunca reutilizado
// entre versões do mesmo tipo (ADR-001).
struct FieldId {
    std::uint16_t value{};

    friend bool operator==(FieldId, FieldId) = default;
};

// Identifica a primeira página de um blob encadeado (Fase 4); 0 = ausente.
struct BlobId {
    std::uint64_t value{};

    friend bool operator==(BlobId, BlobId) = default;
};

// Identifica um banco aberto dentro do processo. Atribuído em tempo de
// execução pelo DatabaseRegistry (Fase 3); nunca é persistido (ADR-001).
struct DatabaseId {
    std::uint32_t value{};

    friend bool operator==(DatabaseId, DatabaseId) = default;
};

// Identidade persistente do arquivo de banco (Fase 14 / ADR-016). Gerada uma
// vez na criação e gravada no DBRT; distinta de DatabaseId (runtime).
struct DatabaseUuid {
    std::array<std::uint8_t, 16> bytes{};

    [[nodiscard]] bool is_nil() const noexcept {
        for (const auto b : bytes) {
            if (b != 0) {
                return false;
            }
        }
        return true;
    }

    friend bool operator==(const DatabaseUuid&, const DatabaseUuid&) = default;
};

// Linha do tempo do banco; muda em restore/recriação divergente (Fase 14).
struct TimelineId {
    std::uint64_t value{1};

    friend bool operator==(TimelineId, TimelineId) = default;
};

// O catálogo também é composto por objetos: a identidade de uma
// TypeDefinition/Baseline é o próprio ObjectId do objeto que a representa
// (ADR-001, ADR-002).
using TypeDefinitionId = ObjectId;
using BaselineId = ObjectId;

// ObjectId zero nunca identifica um objeto real; usado como marcador de
// "ainda não atribuído" (ver TypeDefinition::id()/Baseline::id() na Fase 1,
// e alocação persistente de ObjectId na Fase 2).
inline constexpr ObjectId invalid_object_id{0};

// Os ObjectIds de 1 a 15 são reservados para meta-objetos do catálogo
// (ADR-002). Os três primeiros são meta-tipos compilados no motor, nunca
// lidos do disco: resolvem o bootstrap da decodificação do próprio catálogo.
inline constexpr ObjectId meta_type_definition{1};
inline constexpr ObjectId meta_attribute_definition{2};
inline constexpr ObjectId meta_baseline{3};

// Primeiro ObjectId disponível para tipos e objetos definidos pela aplicação.
inline constexpr std::uint64_t first_user_object_id = 16;

} // namespace modb::object
