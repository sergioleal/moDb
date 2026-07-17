#include "modb/object/projection_plan.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <limits>
#include <string>

using namespace modb;
using namespace modb::object;

namespace {

AttributeDefinition attribute(std::uint16_t id, std::string name, AttributeType type) {
    return AttributeDefinition{
        .id = FieldId{id}, .name = std::move(name), .type = type, .nullable = false};
}

struct Current {
    std::string name;
    double amount{};
    std::string country;
};

Binding current_binding(bool with_default = true) {
    BindingBuilder<Current> builder{"Current"};
    builder.field<1>("name", &Current::name).field<2>("amount", &Current::amount);
    if (with_default) {
        builder.field<3>("country", &Current::country, "BR");
    } else {
        builder.field<3>("country", &Current::country);
    }
    return std::move(*builder.build());
}

} // namespace

int main() {
    TestSuite suite;

    auto stored = TypeDefinition::create(
        "Current", {attribute(1, "name", AttributeType::string),
                    attribute(2, "amount", AttributeType::int64),
                    attribute(9, "removed", AttributeType::boolean)});
    suite.check(stored.has_value(), "stored definition is valid");
    if (!stored) {
        return suite.finish();
    }

    auto binding = current_binding();
    auto identical_type = binding.to_type_definition();
    auto identical_plan = ProjectionPlan::build(*identical_type, binding);
    bool all_copy = identical_plan.has_value();
    if (identical_plan) {
        for (const auto& step : identical_plan->steps()) {
            all_copy = all_copy && step.op == ProjectionOp::copy;
        }
    }
    suite.check(all_copy, "identical schemas produce a pure copy plan");

    auto plan = ProjectionPlan::build(*stored, binding);
    suite.check(plan.has_value(), "copy, convert, default and ignore plan builds");
    if (plan) {
        bool copy = false;
        bool convert = false;
        bool use_default = false;
        bool ignore = false;
        for (const auto& step : plan->steps()) {
            copy |= step.op == ProjectionOp::copy;
            convert |= step.op == ProjectionOp::convert;
            use_default |= step.op == ProjectionOp::use_default;
            ignore |= step.op == ProjectionOp::ignore;
        }
        suite.check(copy && convert && use_default && ignore,
                    "plan contains all expected projection operations");

        Current value;
        DecodedObject object{
            .id = ObjectId{20},
            .type = TypeDefinitionId{16},
            .fields = {{FieldId{1}, AttributeValue{"Ana"}},
                       {FieldId{2}, AttributeValue{std::int64_t{42}}},
                       {FieldId{9}, AttributeValue{true}}}};
        auto materialized = plan->materialize(object, binding, &value);
        suite.check(materialized.has_value() && value.name == "Ana" && value.amount == 42.0 &&
                        value.country == "BR",
                    "plan materializes copy, numeric conversion and default");
    }

    auto missing_default = ProjectionPlan::build(*stored, current_binding(false));
    suite.check_error(missing_default, ErrorCode::incompatible_projection,
                      "new required field without default is rejected");

    struct Integral {
        std::int64_t amount{};
    };
    BindingBuilder<Integral> integral_builder{"Integral"};
    integral_builder.field<1>("amount", &Integral::amount);
    auto integral_binding = integral_builder.build();
    auto floating = TypeDefinition::create(
        "Integral", {attribute(1, "amount", AttributeType::float64)});
    auto narrowing = ProjectionPlan::build(*floating, *integral_binding);
    Integral integral;
    DecodedObject fractional{.id = ObjectId{21},
                             .type = TypeDefinitionId{17},
                             .fields = {{FieldId{1}, AttributeValue{19.9}}}};
    suite.check(narrowing && narrowing->materialize(fractional, *integral_binding, &integral) &&
                    integral.amount == 19,
                "float64 to int64 truncates toward zero");

    DecodedObject overflow{
        .id = ObjectId{22},
        .type = TypeDefinitionId{17},
        .fields = {{FieldId{1}, AttributeValue{std::numeric_limits<double>::infinity()}}}};
    suite.check_error(narrowing->materialize(overflow, *integral_binding, &integral),
                      ErrorCode::incompatible_projection, "numeric overflow is rejected");

    auto incompatible = TypeDefinition::create(
        "Integral", {attribute(1, "amount", AttributeType::string)});
    suite.check_error(ProjectionPlan::build(*incompatible, *integral_binding),
                      ErrorCode::incompatible_projection,
                      "semantic conversion requires a registered migration");

    struct Reference {
        ObjectId manager{};
    };
    BindingBuilder<Reference> reference_builder{"Reference"};
    reference_builder.field<1>("manager", &Reference::manager);
    auto reference_binding = reference_builder.build();
    auto reference_type =
        TypeDefinition::create("Reference", {attribute(1, "manager", AttributeType::ref)});
    auto reference_plan = ProjectionPlan::build(*reference_type, *reference_binding);
    Reference reference;
    DecodedObject referenced{.id = ObjectId{23},
                             .type = TypeDefinitionId{18},
                             .fields = {{FieldId{1}, AttributeValue{ObjectId{77}}}}};
    suite.check(reference_plan && reference_plan->steps()[0].op == ProjectionOp::resolve_reference &&
                    reference_plan->materialize(referenced, *reference_binding, &reference) &&
                    reference.manager == ObjectId{77},
                "reference fields use ResolveReference");

    return suite.finish();
}
