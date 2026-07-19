#include "modb/graph/edge_handle.hpp"
#include "modb/object/binding.hpp"
#include "modb/object/database.hpp"
#include "modb/object/ref.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

using namespace modb;
using namespace modb::object;
using namespace modb::graph;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-12a-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + ".wal", ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

template <typename ResultDatabase>
std::shared_ptr<Database> share_database(ResultDatabase& result) {
    if (!result) {
        return {};
    }
    return std::make_shared<Database>(std::move(*result));
}

struct Department {
    std::string name;
};

struct Address {
    std::string street;
    std::int64_t number{};
};

struct Node {
    std::int64_t tag{};
};

struct Employee {
    std::string name;
    Ref<Department> department{};
    Embedded<Address> address{};
    OwnedRef<Node> badge{};
};

BindingBuilder<Department> department_builder() {
    BindingBuilder<Department> builder{"Department"};
    builder.field<1>("name", &Department::name);
    return builder;
}

BindingBuilder<Node> node_builder() {
    BindingBuilder<Node> builder{"Node"};
    builder.field<1>("tag", &Node::tag);
    return builder;
}

Binding address_binding() {
    BindingBuilder<Address> builder{"Address"};
    builder.field<1>("street", &Address::street).field<2>("number", &Address::number);
    return std::move(*builder.build());
}

BindingBuilder<Employee> employee_builder() {
    BindingBuilder<Employee> builder{"Employee"};
    builder.field<1>("name", &Employee::name)
        .field<2>("department", &Employee::department)
        .embedded<3>("address", &Employee::address, address_binding())
        .field<4>("badge", &Employee::badge);
    return builder;
}

Result<void> bind_all(Database& database) {
    if (auto status = database.bind(department_builder()); !status) {
        return status;
    }
    if (auto status = database.bind(node_builder()); !status) {
        return status;
    }
    return database.bind(employee_builder());
}

} // namespace

int main() {
    TestSuite suite;
    auto& registry = DatabaseRegistry::instance();
    TemporaryDatabase temp{"edge"};

    ObjectId dep_id{};
    ObjectId emp_id{};
    ObjectId badge_id{};

    {
        auto created = Database::create(temp.path());
        auto database = share_database(created);
        suite.check(database != nullptr, "create database");
        if (!database) {
            return suite.finish();
        }
        auto database_id = registry.attach(database);
        suite.check(database_id.has_value(), "attach");
        suite.check(bind_all(*database).has_value(), "bind types");

        auto tx = database->begin();
        suite.check(tx.has_value(), "begin");
        auto dept = database->create(*tx, Department{"Engineering"});
        auto badge = database->create(*tx, Node{.tag = 7});
        suite.check(dept.has_value() && badge.has_value(), "create dept/badge");
        dep_id = dept->id();
        badge_id = badge->id();

        auto emp = database->create(
            *tx, Employee{.name = "Ada",
                          .department = Ref<Department>{dep_id},
                          .address = Embedded<Address>{Address{"Main", 10}},
                          .badge = OwnedRef<Node>{badge_id}});
        suite.check(emp.has_value(), "create employee");
        emp_id = emp->id();
        suite.check(tx->commit().has_value(), "commit");

        auto snap = database->snapshot();
        suite.check(snap.has_value(), "snapshot");
        auto assoc = edge(*database, *emp, FieldId{2}, &Employee::department);
        suite.check(assoc.has_value(), "association edge");
        if (assoc) {
            suite.check(assoc->source_id() == emp_id, "assoc source");
            suite.check(assoc->target_id() == dep_id, "assoc target");
            suite.check(assoc->field().value == 2, "assoc field");
            auto source = assoc->source(*snap);
            auto target = assoc->target(*snap);
            suite.check(source && source->name == "Ada", "resolve source");
            suite.check(target && target->name == "Engineering", "resolve target");
            suite.check(assoc->dangling(*snap).has_value() && !*assoc->dangling(*snap),
                        "assoc not dangling");
        }

        auto owned = edge(*database, *emp, FieldId{4}, &Employee::badge);
        suite.check(owned.has_value(), "ownership edge");
        if (owned) {
            suite.check(owned->target_id() == badge_id, "owned target");
            auto target = owned->target(*snap);
            suite.check(target && target->tag == 7, "resolve owned target");
        }

        auto embedded = edge(*database, *emp, FieldId{3}, &Employee::department);
        suite.check(!embedded && embedded.error().code == ErrorCode::invalid_edge,
                    "embedded field is invalid_edge");

        auto mismatched = edge(*database, *emp, FieldId{4}, &Employee::department);
        suite.check(!mismatched && mismatched.error().code == ErrorCode::invalid_edge,
                    "ownership FieldId with association member rejected");

        auto unknown = edge(*database, *emp, FieldId{99}, &Employee::department);
        suite.check(!unknown && unknown.error().code == ErrorCode::invalid_edge,
                    "unknown FieldId is invalid_edge");

        registry.detach(*database_id);
    }

    // Reabertura: handles novos a partir do objeto persistido.
    {
        auto opened = Database::open(temp.path());
        auto database = share_database(opened);
        suite.check(database != nullptr, "reopen");
        if (!database) {
            return suite.finish();
        }
        auto database_id = registry.attach(database);
        suite.check(bind_all(*database).has_value(), "rebind");
        auto emp = database->get<Employee>(emp_id);
        suite.check(emp.has_value(), "get employee after reopen");
        auto snap = database->snapshot();
        auto assoc = edge(*database, *emp, FieldId{2}, &Employee::department);
        suite.check(assoc.has_value(), "edge after reopen");
        if (assoc && snap) {
            auto target = assoc->target(*snap);
            suite.check(target && target->name == "Engineering", "target survives reopen");
        }
        registry.detach(*database_id);
    }

    // Ref órfã.
    {
        TemporaryDatabase orphan_temp{"orphan"};
        auto created = Database::create(orphan_temp.path());
        auto database = share_database(created);
        suite.check(database != nullptr, "create orphan db");
        if (!database) {
            return suite.finish();
        }
        auto database_id = registry.attach(database);
        suite.check(bind_all(*database).has_value(), "bind orphan db");

        auto tx = database->begin();
        const ObjectId missing{9'001};
        auto emp = database->create(*tx, Employee{.name = "Orphan",
                                                  .department = Ref<Department>{missing},
                                                  .address = Embedded<Address>{},
                                                  .badge = OwnedRef<Node>{}});
        suite.check(emp.has_value() && tx->commit().has_value(), "commit orphan ref");

        auto snap = database->snapshot();
        auto assoc = edge(*database, *emp, FieldId{2}, &Employee::department);
        suite.check(assoc.has_value(), "build edge to missing target");
        if (assoc && snap) {
            auto target = assoc->target(*snap);
            suite.check(!target && target.error().code == ErrorCode::edge_target_not_found,
                        "orphan yields edge_target_not_found");
            auto dangling = assoc->dangling(*snap);
            suite.check(dangling.has_value() && *dangling, "dangling is true");
        }
        registry.detach(*database_id);
    }

    return suite.finish();
}
