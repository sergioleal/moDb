#include "modb/object/projection_plan.hpp"

#include <atomic>
#include <cmath>
#include <limits>
#include <string>

namespace modb::object {
namespace {

std::atomic<std::uint64_t> projection_build_count{0};

bool can_convert(AttributeType from, AttributeType to) noexcept {
    return (from == AttributeType::int64 && to == AttributeType::float64) ||
           (from == AttributeType::float64 && to == AttributeType::int64) ||
           (from == AttributeType::boolean && to == AttributeType::int64);
}

Result<AttributeValue> convert_value(const AttributeValue& value, AttributeType to) {
    if (value.type() == AttributeType::int64 && to == AttributeType::float64) {
        auto integer = value.as_int64();
        if (!integer) {
            return std::unexpected(integer.error());
        }
        const double converted = static_cast<double>(*integer);
        if (converted < static_cast<double>(std::numeric_limits<std::int64_t>::lowest()) ||
            converted >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) ||
            static_cast<std::int64_t>(converted) != *integer) {
            return std::unexpected(Error{ErrorCode::incompatible_projection,
                                         "int64 to float64 conversion loses precision"});
        }
        return AttributeValue{converted};
    }
    if (value.type() == AttributeType::float64 && to == AttributeType::int64) {
        auto real = value.as_float64();
        if (!real) {
            return std::unexpected(real.error());
        }
        constexpr double minimum = static_cast<double>(std::numeric_limits<std::int64_t>::lowest());
        constexpr double maximum = static_cast<double>(std::numeric_limits<std::int64_t>::max());
        if (!std::isfinite(*real) || *real < minimum || *real >= maximum) {
            return std::unexpected(
                Error{ErrorCode::incompatible_projection, "float64 to int64 conversion overflow"});
        }
        return AttributeValue{static_cast<std::int64_t>(*real)};
    }
    if (value.type() == AttributeType::boolean && to == AttributeType::int64) {
        auto boolean = value.as_bool();
        if (!boolean) {
            return std::unexpected(boolean.error());
        }
        return AttributeValue{std::int64_t{*boolean ? 1 : 0}};
    }
    return std::unexpected(
        Error{ErrorCode::incompatible_projection, "unsupported automatic field conversion"});
}

const AttributeValue* find_value(const FieldValues& fields, FieldId id) noexcept {
    for (const auto& [candidate_id, value] : fields) {
        if (candidate_id == id) {
            return &value;
        }
    }
    return nullptr;
}

} // namespace

Result<ProjectionPlan> ProjectionPlan::build(const TypeDefinition& stored,
                                             const Binding& current) {
    ++projection_build_count;
    if (stored.name() != current.type_name()) {
        return std::unexpected(
            Error{ErrorCode::type_mismatch, "projection type names do not match"});
    }
    std::vector<ProjectionStep> steps;
    steps.reserve(stored.attributes().size() + current.fields().size());

    // Campos que só existem no tipo persistido são explicitamente ignorados.
    for (const auto& source : stored.attributes()) {
        bool present = false;
        for (const auto& binder : current.fields()) {
            if (binder.id == source.id) {
                present = true;
                break;
            }
        }
        if (!present) {
            steps.push_back(ProjectionStep{.op = ProjectionOp::ignore,
                                           .source = source.id,
                                           .from = source.type,
                                           .default_value = std::nullopt,
                                           .source_default = std::nullopt});
        }
    }

    for (std::size_t index = 0; index < current.fields().size(); ++index) {
        const auto& binder = current.fields()[index];
        const auto* source = stored.find(binder.id);
        if (source == nullptr) {
            if (!binder.default_value) {
                return std::unexpected(Error{
                    ErrorCode::incompatible_projection,
                    "new required field has no default: " + binder.name});
            }
            steps.push_back(ProjectionStep{.op = ProjectionOp::use_default,
                                           .binder_index = index,
                                           .to = binder.type,
                                           .default_value = binder.default_value,
                                           .source_default = std::nullopt});
            continue;
        }
        if (source->nullable) {
            return std::unexpected(Error{
                ErrorCode::incompatible_projection,
                "nullable stored field requires a migration: " + binder.name});
        }
        if (source->type == binder.type) {
            steps.push_back(ProjectionStep{.op = source->type == AttributeType::ref
                                                    ? ProjectionOp::resolve_reference
                                                    : ProjectionOp::copy,
                                           .source = source->id,
                                           .binder_index = index,
                                           .from = source->type,
                                           .to = binder.type,
                                           .default_value = std::nullopt,
                                           .source_default = source->default_value});
            continue;
        }
        if (!can_convert(source->type, binder.type)) {
            return std::unexpected(Error{
                ErrorCode::incompatible_projection,
                "field '" + binder.name + "' changed from " +
                    std::string{attribute_type_name(source->type)} + " to " +
                    std::string{attribute_type_name(binder.type)}});
        }
        steps.push_back(ProjectionStep{.op = ProjectionOp::convert,
                                       .source = source->id,
                                       .binder_index = index,
                                       .from = source->type,
                                       .to = binder.type,
                                       .default_value = std::nullopt,
                                       .source_default = source->default_value});
    }
    return ProjectionPlan{std::move(steps)};
}

Result<void> ProjectionPlan::materialize(const DecodedObject& object,
                                         const Binding& current,
                                         void* destination) const {
    if (destination == nullptr) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "projection destination cannot be null"});
    }
    for (const auto& step : steps_) {
        if (step.op == ProjectionOp::ignore) {
            continue;
        }
        if (step.binder_index >= current.fields().size()) {
            return std::unexpected(
                Error{ErrorCode::corrupt_file, "projection binder index is out of range"});
        }
        const auto& binder = current.fields()[step.binder_index];
        const AttributeValue* value = nullptr;
        AttributeValue converted;
        if (step.op == ProjectionOp::use_default) {
            value = &*step.default_value;
        } else {
            value = find_value(object.fields, step.source);
            if (value == nullptr) {
                if (step.source_default) {
                    value = &*step.source_default;
                } else {
                    return std::unexpected(
                        Error{ErrorCode::field_not_found, "stored projection field is missing"});
                }
            }
            if (step.op == ProjectionOp::convert) {
                auto result = convert_value(*value, step.to);
                if (!result) {
                    return std::unexpected(result.error());
                }
                converted = std::move(*result);
                value = &converted;
            }
        }
        if (auto assigned = binder.store(destination, *value); !assigned) {
            return std::unexpected(assigned.error());
        }
    }
    return {};
}

std::uint64_t ProjectionPlan::build_count() noexcept {
    return projection_build_count.load();
}

void ProjectionPlan::reset_build_count() noexcept {
    projection_build_count.store(0);
}

} // namespace modb::object
