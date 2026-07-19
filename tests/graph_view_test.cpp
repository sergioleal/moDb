#include "modb/graph/graph_view.hpp"
#include "modb/object/binding.hpp"
#include "modb/object/collection.hpp"
#include "modb/object/database.hpp"
#include "modb/object/ref.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace modb;
using namespace modb::object;
using namespace modb::graph;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-12b-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
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

struct Project {
    std::string name;
};

struct Employee {
    std::string name;
    Ref<Project> lead{};
    BlobId projects{};
};

BindingBuilder<Project> project_builder() {
    BindingBuilder<Project> builder{"Project"};
    builder.field<1>("name", &Project::name);
    return builder;
}

BindingBuilder<Employee> employee_builder() {
    BindingBuilder<Employee> builder{"Employee"};
    builder.field<1>("name", &Employee::name)
        .field<2>("lead", &Employee::lead)
        .field<3>("projects", &Employee::projects);
    return builder;
}

Result<void> bind_all(Database& database) {
    if (auto status = database.bind(project_builder()); !status) {
        return status;
    }
    return database.bind(employee_builder());
}

} // namespace

int main() {
    TestSuite suite;
    auto& registry = DatabaseRegistry::instance();
    TemporaryDatabase temp{"view"};

    ObjectId apollo{};
    ObjectId gemini{};
    ObjectId ana{};
    ObjectId bob{};

    {
        auto created = Database::create(temp.path());
        auto database = share_database(created);
        suite.check(database != nullptr, "create database");
        if (!database) {
            return suite.finish();
        }
        auto database_id = registry.attach(database);
        suite.check(database_id.has_value() && bind_all(*database).has_value(), "attach+bind");

        auto tx = database->begin();
        suite.check(tx.has_value(), "begin");
        auto p1 = database->create(*tx, Project{"Apollo"});
        auto p2 = database->create(*tx, Project{"Gemini"});
        suite.check(p1 && p2, "create projects");
        apollo = p1->id();
        gemini = p2->id();

        auto blobs = database->blobs();
        auto projects = PersistentVector<Ref<Project>>::create(blobs, *tx);
        suite.check(projects.has_value(), "create projects vector");
        suite.check(projects->push_back(*tx, Ref<Project>{apollo}).has_value(), "push apollo");
        suite.check(projects->push_back(*tx, Ref<Project>{gemini}).has_value(), "push gemini");

        auto e1 = database->create(*tx, Employee{.name = "Ana",
                                                 .lead = Ref<Project>{apollo},
                                                 .projects = projects->id()});
        auto e2 = database->create(
            *tx, Employee{.name = "Bob", .lead = Ref<Project>{apollo}, .projects = BlobId{}});
        suite.check(e1 && e2 && tx->commit().has_value(), "seed employees");
        ana = e1->id();
        bob = e2->id();

        // Sem índice: incoming deve falhar explicitamente.
        {
            auto snap = database->snapshot();
            suite.check(snap.has_value(), "snapshot before index");
            auto view = open_graph_view(*database, *snap);
            suite.check(view.has_value(), "open graph view");
            auto apollo_h = database->get<Project>(apollo);
            auto missing = view->incoming<Employee, Project>(*apollo_h, FieldId{2},
                                                             &Employee::lead);
            suite.check(!missing && missing.error().code == ErrorCode::invalid_edge,
                        "incoming without index fails");
            suite.check(missing.error().message.find("index") != std::string::npos,
                        "error mentions index");
        }

        suite.check(database->create_index<Employee>(FieldId{2}).has_value(),
                    "index on lead Ref");

        {
            auto snap = database->snapshot();
            auto view = open_graph_view(*database, *snap);
            suite.check(view.has_value(), "open view with index");

            auto ana_h = database->get<Employee>(ana);
            auto outgoing =
                view->outgoing_collection<Employee, Project>(*ana_h, FieldId{3},
                                                             &Employee::projects);
            suite.check(outgoing.has_value() && outgoing->size() == 2,
                        "outgoing collection size");
            if (outgoing && outgoing->size() == 2) {
                suite.check((*outgoing)[0].target_id() == apollo, "order: first apollo");
                suite.check((*outgoing)[1].target_id() == gemini, "order: second gemini");
                suite.check((*outgoing)[0].source_id() == ana, "outgoing source ana");
                suite.check((*outgoing)[0].field().value == 3, "collection field id");
                auto target = (*outgoing)[0].target(*snap);
                suite.check(target && target->name == "Apollo", "resolve outgoing target");
            }

            auto bob_h = database->get<Employee>(bob);
            auto empty =
                view->outgoing_collection<Employee, Project>(*bob_h, FieldId{3},
                                                             &Employee::projects);
            suite.check(empty.has_value() && empty->empty(), "empty collection adjacency");

            auto wrong_field =
                view->outgoing_collection<Employee, Project>(*ana_h, FieldId{2},
                                                             &Employee::projects);
            suite.check(!wrong_field && wrong_field.error().code == ErrorCode::invalid_edge,
                        "Ref FieldId is not a collection blob");

            auto apollo_h = database->get<Project>(apollo);
            auto gemini_h = database->get<Project>(gemini);
            auto into_apollo =
                view->incoming<Employee, Project>(*apollo_h, FieldId{2}, &Employee::lead);
            suite.check(into_apollo.has_value() && into_apollo->size() == 2,
                        "incoming to apollo (Ana+Bob)");
            if (into_apollo) {
                std::vector<ObjectId> sources;
                for (const auto& edge : *into_apollo) {
                    sources.push_back(edge.source_id());
                    suite.check(edge.target_id() == apollo, "incoming target apollo");
                }
                suite.check(
                    (sources[0] == ana && sources[1] == bob) ||
                        (sources[0] == bob && sources[1] == ana),
                    "incoming sources are Ana and Bob");
            }

            auto into_gemini =
                view->incoming<Employee, Project>(*gemini_h, FieldId{2}, &Employee::lead);
            suite.check(into_gemini.has_value() && into_gemini->empty(),
                        "no incoming to gemini via lead");
        }

        registry.detach(*database_id);
    }

    // Reabertura: coleção e índice sobrevivem.
    {
        auto opened = Database::open(temp.path());
        auto database = share_database(opened);
        suite.check(database != nullptr, "reopen");
        if (!database) {
            return suite.finish();
        }
        auto database_id = registry.attach(database);
        suite.check(bind_all(*database).has_value(), "rebind");
        suite.check(database->has_index_for<Employee>(FieldId{2}), "index persists");

        auto snap = database->snapshot();
        auto view = open_graph_view(*database, *snap);
        auto ana_h = database->get<Employee>(ana);
        auto outgoing =
            view->outgoing_collection<Employee, Project>(*ana_h, FieldId{3}, &Employee::projects);
        suite.check(outgoing.has_value() && outgoing->size() == 2,
                    "outgoing after reopen");

        auto apollo_h = database->get<Project>(apollo);
        auto into_apollo =
            view->incoming<Employee, Project>(*apollo_h, FieldId{2}, &Employee::lead);
        suite.check(into_apollo.has_value() && into_apollo->size() == 2,
                    "incoming after reopen");
        registry.detach(*database_id);
    }

    return suite.finish();
}
