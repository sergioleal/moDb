#include "modb/object/database.hpp"
#include "modb/object/projection_plan.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <limits>
#include <string>
#include <system_error>

using namespace modb;
using namespace modb::object;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-evolution-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

struct EmployeeV1 {
    std::string name;
    double salary{};
};

struct EmployeeV2 {
    std::string name;
    double salary{};
    std::string country;
};

BindingBuilder<EmployeeV1> employee_v1() {
    BindingBuilder<EmployeeV1> builder{"Employee"};
    builder.field<1>("name", &EmployeeV1::name).field<2>("salary", &EmployeeV1::salary);
    return builder;
}

BindingBuilder<EmployeeV2> employee_v2() {
    BindingBuilder<EmployeeV2> builder{"Employee"};
    builder.field<1>("name", &EmployeeV2::name)
        .field<2>("salary", &EmployeeV2::salary)
        .field<3>("country", &EmployeeV2::country, "BR");
    return builder;
}

template <typename ResultDatabase>
std::shared_ptr<Database> share_database(ResultDatabase& result) {
    if (!result) {
        return {};
    }
    return std::make_shared<Database>(std::move(*result));
}

} // namespace

int main() {
    TestSuite suite;
    auto& registry = DatabaseRegistry::instance();

    TemporaryDatabase evolution{"mvp"};
    ObjectId ana_id{};
    ObjectId bia_id{};
    TypeDefinitionId v1_type{};
    TypeDefinitionId v2_type{};
    BaselineId v1_baseline{};

    {
        auto created = Database::create(evolution.path());
        auto database = share_database(created);
        suite.check(database != nullptr, "v1 database is created");
        if (!database) {
            return suite.finish();
        }
        auto database_id = registry.attach(database);
        suite.check(database_id.has_value(), "v1 database is attached");
        suite.check(database->bind(employee_v1()).has_value(), "v1 binding is registered");

        auto ana = database->create(EmployeeV1{"Ana", 15000.0});
        auto bia = database->create(EmployeeV1{"Bia", 12000.0});
        suite.check(ana.has_value() && bia.has_value(), "v1 objects are stored");
        if (!ana || !bia) {
            return suite.finish();
        }
        ana_id = ana->id();
        bia_id = bia->id();
        auto type = database->object_type(ana_id);
        suite.check(type.has_value(), "v1 object records its definition");
        if (type) {
            v1_type = *type;
        }
        suite.check(database->current_baseline().has_value(), "v1 baseline exists");
        if (database->current_baseline()) {
            v1_baseline = database->current_baseline()->id();
        }
        suite.check(database->flush().has_value(), "v1 database is flushed");
        registry.detach(*database_id);
    }

    {
        auto opened = Database::open(evolution.path());
        auto database = share_database(opened);
        suite.check(database != nullptr, "v2 database is reopened");
        if (!database) {
            return suite.finish();
        }
        auto database_id = registry.attach(database);
        suite.check(database_id.has_value(), "v2 database is attached");
        suite.check(database->bind(employee_v2()).has_value(),
                    "divergent binding creates a new type version");

        suite.check(database->current_baseline().has_value() &&
                        database->current_baseline()->id() != v1_baseline,
                    "schema evolution creates a new baseline");
        auto historical = database->find_baseline(v1_baseline);
        suite.check(historical.has_value() && historical->get().types().size() == 1 &&
                        historical->get().types()[0] == v1_type,
                    "the previous baseline remains loadable and immutable");

        auto ana = database->get<EmployeeV2>(ana_id);
        suite.check(ana.has_value(), "v1 object is addressed through the v2 binding");
        ProjectionPlan::reset_build_count();
        auto first = ana ? database->materialize(*ana) : Result<EmployeeV2>{
                                                       std::unexpected(Error{
                                                           ErrorCode::record_not_found, "missing"})};
        auto second = ana ? database->materialize(*ana) : first;
        suite.check(first.has_value() && first->name == "Ana" && first->salary == 15000.0 &&
                        first->country == "BR",
                    "v2 reads a v1 object with the declared default");
        suite.check(second.has_value() && ProjectionPlan::build_count() == 1,
                    "the projection plan is cached by stored TypeDefinitionId");

        auto current = database->create(EmployeeV2{"Carla", 18000.0, "PT"});
        suite.check(current.has_value(), "v2 object is stored");
        auto current_value = current ? database->materialize(*current) : first;
        auto old_value = database->get<EmployeeV2>(bia_id);
        auto old_materialized =
            old_value ? database->materialize(*old_value) : Result<EmployeeV2>{std::unexpected(
                                                          Error{ErrorCode::record_not_found,
                                                                "missing"})};
        suite.check(current_value && current_value->country == "PT" && old_materialized &&
                        old_materialized->country == "BR",
                    "v1 and v2 objects coexist in one session");

        if (ana) {
            suite.check(ana->database() == *database_id, "handle carries DatabaseId and ObjectId");
            auto salary = ana->get<&EmployeeV2::salary>();
            suite.check(salary.has_value() && *salary == 15000.0,
                        "Handle::get reads a typed member");
            auto transaction = database->begin();
            suite.check(ana->set<&EmployeeV2::salary>(transaction, 16000.0).has_value(),
                        "Handle::set rewrites through the current binding");
            auto rewritten_type = database->object_type(ana_id);
            suite.check(rewritten_type.has_value() && *rewritten_type != v1_type,
                        "Handle::set performs lazy migration to v2");
            if (rewritten_type) {
                v2_type = *rewritten_type;
            }
        }
        suite.check(database->flush().has_value(), "v2 database is flushed");
        registry.detach(*database_id);
    }

    {
        auto reopened = Database::open(evolution.path());
        auto database = share_database(reopened);
        auto database_id = database ? registry.attach(database) : Result<DatabaseId>{
                                                               std::unexpected(Error{
                                                                   ErrorCode::invalid_argument,
                                                                   "database reopen failed"})};
        suite.check(database && database_id && database->bind(employee_v2()).has_value(),
                    "evolved database reopens with the v2 binding");
        if (database && database_id) {
            auto migrated_type = database->object_type(ana_id);
            auto old = database->get<EmployeeV2>(bia_id);
            auto old_value = old ? database->materialize(*old) : Result<EmployeeV2>{
                                                                std::unexpected(Error{
                                                                    ErrorCode::record_not_found,
                                                                    "missing"})};
            suite.check(migrated_type.has_value() && *migrated_type == v2_type,
                        "lazy migration survives reopening");
            suite.check(old_value.has_value() && old_value->country == "BR",
                        "an untouched v1 object remains projectable after reopening");
            suite.check(database->find_baseline(v1_baseline).has_value(),
                        "historical baseline survives reopening after evolution");
            registry.detach(*database_id);
        }
    }

    // Mudança semântica: salary em reais vira salary_cents no mesmo FieldId.
    TemporaryDatabase semantic{"migration"};
    ObjectId salary_id{};
    TypeDefinitionId salary_v1_type{};
    {
        auto created = Database::create(semantic.path());
        auto database = share_database(created);
        auto database_id = database ? registry.attach(database) : Result<DatabaseId>{
                                                               std::unexpected(Error{
                                                                   ErrorCode::invalid_argument,
                                                                   "database creation failed"})};
        BindingBuilder<EmployeeV1> builder{"Salary"};
        builder.field<1>("name", &EmployeeV1::name).field<2>("salary", &EmployeeV1::salary);
        suite.check(database && database->bind(std::move(builder)).has_value(),
                    "semantic v1 binding is registered");
        if (!database) {
            return suite.finish();
        }
        auto object = database->create(EmployeeV1{"Dora", 12.34});
        if (object) {
            salary_id = object->id();
            auto type = database->object_type(salary_id);
            if (type) {
                salary_v1_type = *type;
            }
        }
        suite.check(object.has_value() && database->flush().has_value(),
                    "semantic v1 object is stored");
        if (database_id) {
            registry.detach(*database_id);
        }
    }

    struct SalaryV2 {
        std::string name;
        std::int64_t salary_cents{};
    };
    {
        auto opened = Database::open(semantic.path());
        auto database = share_database(opened);
        auto database_id = database ? registry.attach(database) : Result<DatabaseId>{
                                                               std::unexpected(Error{
                                                                   ErrorCode::invalid_argument,
                                                                   "database open failed"})};
        BindingBuilder<SalaryV2> builder{"Salary"};
        builder.field<1>("name", &SalaryV2::name)
            .field<2>("salary_cents", &SalaryV2::salary_cents);
        suite.check(database && database->bind(std::move(builder)).has_value(),
                    "semantic v2 binding is registered");
        if (!database) {
            return suite.finish();
        }
        auto handle = database->get<SalaryV2>(salary_id);
        suite.check(handle.has_value(), "semantic old object is found");
        if (handle) {
            auto automatic = database->materialize(*handle);
            suite.check(automatic.has_value() && automatic->salary_cents == 12,
                        "automatic numeric projection cannot infer semantic units");
            auto registered_migration = database->register_migration(
                "Salary", salary_v1_type.value,
                [](const DecodedObject& object) -> Result<FieldValues> {
                    const AttributeValue* name_value = nullptr;
                    const AttributeValue* salary_value = nullptr;
                    for (const auto& [id, value] : object.fields) {
                        if (id == FieldId{1}) {
                            name_value = &value;
                        } else if (id == FieldId{2}) {
                            salary_value = &value;
                        }
                    }
                    if (name_value == nullptr || salary_value == nullptr) {
                        return std::unexpected(
                            Error{ErrorCode::field_not_found, "migration source field is absent"});
                    }
                    const auto name = name_value->as_string();
                    const auto salary = salary_value->as_float64();
                    if (!name) {
                        return std::unexpected(name.error());
                    }
                    if (!salary) {
                        return std::unexpected(salary.error());
                    }
                    const double cents = *salary * 100.0;
                    constexpr double minimum =
                        static_cast<double>(std::numeric_limits<std::int64_t>::lowest());
                    constexpr double maximum =
                        static_cast<double>(std::numeric_limits<std::int64_t>::max());
                    if (!std::isfinite(cents) || cents < minimum || cents >= maximum) {
                        return std::unexpected(Error{ErrorCode::incompatible_projection,
                                                     "salary cents overflow"});
                    }
                    return FieldValues{
                        {FieldId{1}, AttributeValue{*name}},
                        {FieldId{2}, AttributeValue{static_cast<std::int64_t>(cents)}}};
                });
            suite.check(registered_migration.has_value(), "semantic migration is registered");
            auto migrated = database->materialize(*handle);
            suite.check(migrated.has_value() && migrated->name == "Dora" &&
                            migrated->salary_cents == 1234,
                        "registered migration handles a semantic schema change");
        }
        if (database_id) {
            registry.detach(*database_id);
        }
    }

    return suite.finish();
}
