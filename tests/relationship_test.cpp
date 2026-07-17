// Importa o Database tipado, o Binding e as referências exercitadas aqui.
#include "modb/object/binding.hpp"
#include "modb/object/database.hpp"
#include "modb/object/ref.hpp"
// Importa as verificações mínimas.
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
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
                ("modb-rel-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
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

// --- Tipos de domínio do grafo ---
struct Department {
    std::string name;
};

struct Address {
    std::string street;
    std::int64_t number{};
};

// Nó de posse autorreferente: exercita cascata profunda e ciclos.
struct Node {
    std::int64_t tag{};
    OwnedRef<Node> child{};
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
    builder.field<1>("tag", &Node::tag).field<2>("child", &Node::child);
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

} // namespace

int main() {
    TestSuite suite;
    auto& registry = DatabaseRegistry::instance();
    TemporaryDatabase temp{"graph"};

    ObjectId dep_id{};
    ObjectId emp_id{};
    ObjectId badge_id{};

    // --- criação do grafo, com reabertura ---
    {
        auto created = Database::create(temp.path());
        auto database = share_database(created);
        suite.check(database != nullptr, "database is created");
        if (!database) {
            return suite.finish();
        }
        auto database_id = registry.attach(database);
        suite.check(database_id.has_value(), "database is attached");
        suite.check(database->bind(department_builder()).has_value(), "Department is bound");
        suite.check(database->bind(node_builder()).has_value(), "Node is bound");
        suite.check(database->bind(employee_builder()).has_value(), "Employee is bound");

        auto transaction = database->begin();
        suite.check(transaction.has_value(), "graph transaction begins");
        if (!transaction) {
            return suite.finish();
        }
        auto dep = database->create(*transaction, Department{"Engenharia"});
        auto badge = database->create(*transaction, Node{42, OwnedRef<Node>{}});
        suite.check(dep.has_value() && badge.has_value(), "Department and badge are stored");
        if (!dep || !badge) {
            return suite.finish();
        }
        dep_id = dep->id();
        badge_id = badge->id();

        Employee ana;
        ana.name = "Ana";
        ana.department = Ref<Department>{dep_id};
        ana.address = Embedded<Address>{Address{"Rua das Flores", 100}};
        ana.badge = OwnedRef<Node>{badge_id};
        auto emp = database->create(*transaction, ana);
        suite.check(emp.has_value(), "Employee with relationships is stored");
        if (!emp) {
            return suite.finish();
        }
        emp_id = emp->id();

        suite.check(transaction->commit().has_value(), "graph transaction committed");
        registry.detach(*database_id);
    }

    // --- reabre e verifica associação, embedded e resolução de Ref ---
    {
        auto opened = Database::open(temp.path());
        auto database = share_database(opened);
        suite.check(database != nullptr, "database reopens");
        if (!database) {
            return suite.finish();
        }
        auto database_id = registry.attach(database);
        suite.check(database_id.has_value(), "reopened database is attached");
        suite.check(database->bind(department_builder()).has_value(), "Department rebound");
        suite.check(database->bind(node_builder()).has_value(), "Node rebound");
        suite.check(database->bind(employee_builder()).has_value(), "Employee rebound");

        auto handle = database->get<Employee>(emp_id);
        suite.check(handle.has_value(), "Employee is addressable after reopening");
        if (handle) {
            auto ana = database->materialize(*handle);
            suite.check(ana.has_value() && ana->name == "Ana",
                        "Employee materializes after reopening");
            if (ana) {
                // Embedded: round-trip dentro do pai, sem identidade própria.
                suite.check(ana->address.value.street == "Rua das Flores" &&
                                ana->address.value.number == 100,
                            "embedded Address round-trips inside the parent");
                // Associação: a Ref guarda a identidade e resolve para o alvo.
                suite.check(ana->department.target == dep_id,
                            "association carries the target ObjectId");
                auto dep_handle = database->get<Department>(ana->department.target);
                suite.check(dep_handle.has_value(), "Ref resolves to the target");
                if (dep_handle) {
                    auto dep = database->materialize(*dep_handle);
                    suite.check(dep.has_value() && dep->name == "Engenharia",
                                "resolved Department has the right content");
                }
            }
        }
        suite.check(database->flush().has_value(), "reopened graph is flushed");
        registry.detach(*database_id);
    }

    // --- composição: remover Employee remove o badge (owned) em cascata,
    //     mas o Department (associação) permanece; a Ref vira pendente ---
    {
        auto opened = Database::open(temp.path());
        auto database = share_database(opened);
        if (!database) {
            return suite.finish();
        }
        auto database_id = registry.attach(database);
        suite.check(database_id.has_value(), "composition database is attached");
        suite.check(database->bind(department_builder()).has_value() &&
                        database->bind(node_builder()).has_value() &&
                        database->bind(employee_builder()).has_value(),
                    "all types rebound for composition");

        auto transaction = database->begin();
        suite.check(transaction.has_value(), "composition transaction begins");
        if (!transaction) {
            return suite.finish();
        }
        suite.check(database->remove(*transaction, emp_id).has_value(), "Employee is removed");
        suite.check_error(database->get<Node>(badge_id), ErrorCode::record_not_found,
                          "owned badge is removed in cascade");
        auto dep_handle = database->get<Department>(dep_id);
        suite.check(dep_handle.has_value(), "associated Department survives the parent removal");

        // Referência pendente detectável: remover o Department e resolver falha.
        suite.check(database->remove(*transaction, dep_id).has_value(), "Department is removed");
        suite.check_error(database->get<Department>(dep_id), ErrorCode::record_not_found,
                          "a dangling Ref target resolves to record_not_found");

        suite.check(transaction->commit().has_value(), "composition transaction committed");
        registry.detach(*database_id);
    }

    // --- cascata profunda A◆B◆C: remover A remove os três ---
    {
        TemporaryDatabase chain{"chain"};
        auto created = Database::create(chain.path());
        auto database = share_database(created);
        if (!database) {
            return suite.finish();
        }
        auto database_id = registry.attach(database);
        suite.check(database_id.has_value() && database->bind(node_builder()).has_value(),
                    "chain database ready");

        auto transaction = database->begin();
        suite.check(transaction.has_value(), "chain transaction begins");
        if (!transaction) {
            return suite.finish();
        }
        auto c = database->create(*transaction, Node{3, OwnedRef<Node>{}});
        auto b = c ? database->create(*transaction, Node{2, OwnedRef<Node>{c->id()}}) : c;
        auto a = b ? database->create(*transaction, Node{1, OwnedRef<Node>{b->id()}}) : b;
        suite.check(a.has_value() && b.has_value() && c.has_value(), "A owns B owns C");
        if (a && b && c) {
            const auto a_id = a->id();
            const auto b_id = b->id();
            const auto c_id = c->id();
            suite.check(database->remove(*transaction, a_id).has_value(), "removing A cascades");
            suite.check(transaction->commit().has_value(), "chain transaction committed");
            suite.check_error(database->get<Node>(a_id), ErrorCode::record_not_found, "A gone");
            suite.check_error(database->get<Node>(b_id), ErrorCode::record_not_found, "B gone");
            suite.check_error(database->get<Node>(c_id), ErrorCode::record_not_found, "C gone");
        }
        if (database_id) {
            registry.detach(*database_id);
        }
    }

    // --- ciclo de posse A◆B◆A: remoção detecta o ciclo e falha ---
    {
        TemporaryDatabase cyclic{"cycle"};
        auto created = Database::create(cyclic.path());
        auto database = share_database(created);
        if (!database) {
            return suite.finish();
        }
        auto database_id = registry.attach(database);
        suite.check(database_id.has_value() && database->bind(node_builder()).has_value(),
                    "cyclic database ready");

        auto transaction = database->begin();
        suite.check(transaction.has_value(), "cycle transaction begins");
        if (!transaction) {
            return suite.finish();
        }
        auto a = database->create(*transaction, Node{1, OwnedRef<Node>{}});
        auto b = a ? database->create(*transaction, Node{2, OwnedRef<Node>{a->id()}}) : a;
        suite.check(a.has_value() && b.has_value(), "A and B created");
        if (a && b) {
            // Fecha o ciclo: A passa a possuir B (e B já possui A).
            auto close = database->update(*transaction, *a, Node{1, OwnedRef<Node>{b->id()}});
            suite.check(close.has_value(), "ownership cycle is closed");
            suite.check_error(database->remove(*transaction, a->id()), ErrorCode::invalid_argument,
                              "a cyclic owned cascade fails explicitly");
            // Nada foi removido: A e B continuam legíveis.
            suite.check(database->get<Node>(a->id()).has_value() &&
                            database->get<Node>(b->id()).has_value(),
                        "no object is removed when a cycle is detected");
        }
        if (database_id) {
            registry.detach(*database_id);
        }
    }

    return suite.finish();
}
