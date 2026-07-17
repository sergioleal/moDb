#pragma once

#include "modb/error.hpp"
#include "modb/object/binding.hpp"
#include "modb/object/object_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace modb::object {

enum class ProjectionOp : std::uint8_t {
    copy,
    convert,
    use_default,
    ignore,
    resolve_reference,
};

struct ProjectionStep {
    ProjectionOp op{ProjectionOp::copy};
    FieldId source{};
    std::size_t binder_index{};
    AttributeType from{AttributeType::null};
    AttributeType to{AttributeType::null};
    std::optional<AttributeValue> default_value;
    std::optional<AttributeValue> source_default;
};

// Plano imutável que reconcilia uma definição persistida com o Binding atual.
class ProjectionPlan {
public:
    [[nodiscard]] static Result<ProjectionPlan> build(const TypeDefinition& stored,
                                                      const Binding& current);

    [[nodiscard]] Result<void> materialize(const DecodedObject& object,
                                           const Binding& current,
                                           void* destination) const;

    [[nodiscard]] std::span<const ProjectionStep> steps() const noexcept { return steps_; }

    // Instrumentação pequena e determinística usada para comprovar o cache.
    [[nodiscard]] static std::uint64_t build_count() noexcept;
    static void reset_build_count() noexcept;

private:
    explicit ProjectionPlan(std::vector<ProjectionStep> steps) : steps_{std::move(steps)} {}

    std::vector<ProjectionStep> steps_;
};

} // namespace modb::object
