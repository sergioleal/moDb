// Importa as coleções persistentes, o Database tipado e as referências.
#include "modb/object/collection.hpp"
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
#include <vector>

using namespace modb;
using namespace modb::object;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-coll-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
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

struct Project {
    std::string name;
};

struct Address {
    std::string street;
};

struct Employee {
    std::string name;
    Ref<Project> lead{};        // associação
    OwnedRef<Address> office{}; // composição (◆)
    BlobId projects{};          // PersistentVector<Ref<Project>>
};

BindingBuilder<Project> project_builder() {
    BindingBuilder<Project> builder{"Project"};
    builder.field<1>("name", &Project::name);
    return builder;
}

BindingBuilder<Address> address_builder() {
    BindingBuilder<Address> builder{"Address"};
    builder.field<1>("street", &Address::street);
    return builder;
}

BindingBuilder<Employee> employee_builder() {
    BindingBuilder<Employee> builder{"Employee"};
    builder.field<1>("name", &Employee::name)
        .field<2>("lead", &Employee::lead)
        .field<3>("office", &Employee::office)
        .field<4>("projects", &Employee::projects);
    return builder;
}

} // namespace

int main() {
    TestSuite suite;
    auto& registry = DatabaseRegistry::instance();

    // --- vector básico: 10 000 elementos multi-página, reabertura ---
    {
        TemporaryDatabase temp{"vector"};
        BlobId vector_id{};
        constexpr int total = 10000;
        {
            auto created = Database::create(temp.path());
            auto database = share_database(created);
            suite.check(database != nullptr, "vector database created");
            if (!database) {
                return suite.finish();
            }
            auto database_id = registry.attach(database);
            auto transaction = database->begin();
            auto blobs = database->blobs();
            auto vector = PersistentVector<std::int64_t>::create(blobs);
            suite.check(vector.has_value(), "vector is created");
            if (!vector) {
                return suite.finish();
            }
            for (int i = 0; i < total; ++i) {
                if (auto pushed = vector->push_back(transaction, static_cast<std::int64_t>(i * 2));
                    !pushed) {
                    suite.check(false, "push_back failed");
                    break;
                }
            }
            auto size = vector->size();
            suite.check(size.has_value() && *size == static_cast<std::size_t>(total),
                        "vector holds all pushed elements");
            auto middle = vector->at(5000);
            suite.check(middle.has_value() && *middle == 10000, "at() reads the right element");
            vector_id = vector->id();
            suite.check(database->flush().has_value(), "vector flushed");
            if (database_id) {
                registry.detach(*database_id);
            }
        }
        auto reopened = Database::open(temp.path());
        auto database = share_database(reopened);
        suite.check(database != nullptr, "vector database reopened");
        if (database) {
            auto blobs = database->blobs();
            PersistentVector<std::int64_t> vector{blobs, vector_id};
            auto size = vector.size();
            suite.check(size.has_value() && *size == static_cast<std::size_t>(total),
                        "vector survives reopening");
            std::int64_t sum = 0;
            std::size_t seen = 0;
            auto walked = vector.for_each([&](const std::int64_t& value) -> Result<void> {
                sum += value;
                ++seen;
                return {};
            });
            suite.check(walked.has_value() && seen == static_cast<std::size_t>(total) &&
                            sum == static_cast<std::int64_t>(total) * (total - 1),
                        "for_each visits every element after reopening");
        }
    }

    // --- set: duplicatas removidas e ordenação por codificação ---
    {
        TemporaryDatabase temp{"set"};
        auto created = Database::create(temp.path());
        auto database = share_database(created);
        if (!database) {
            return suite.finish();
        }
        auto database_id = registry.attach(database);
        auto transaction = database->begin();
        auto blobs = database->blobs();
        auto set = PersistentSet<std::int64_t>::create(blobs);
        suite.check(set.has_value(), "set is created");
        if (set) {
            for (std::int64_t value : {5, 3, 5, 1, 3, 9, 1}) {
                suite.check(set->insert(transaction, value).has_value(), "set insert succeeds");
            }
            auto size = set->size();
            suite.check(size.has_value() && *size == 4, "set deduplicates to 4 unique elements");
            auto has3 = set->contains(3);
            auto has7 = set->contains(7);
            suite.check(has3.has_value() && *has3, "set contains an inserted element");
            suite.check(has7.has_value() && !*has7, "set rejects an absent element");
        }
        if (database_id) {
            registry.detach(*database_id);
        }
    }

    // --- map: put/get/remove e reabertura ---
    {
        TemporaryDatabase temp{"map"};
        BlobId map_id{};
        {
            auto created = Database::create(temp.path());
            auto database = share_database(created);
            if (!database) {
                return suite.finish();
            }
            auto database_id = registry.attach(database);
            auto transaction = database->begin();
            auto blobs = database->blobs();
            auto map = PersistentMap<std::string, std::int64_t>::create(blobs);
            suite.check(map.has_value(), "map is created");
            if (!map) {
                return suite.finish();
            }
            suite.check(map->put(transaction, "ana", 10).has_value(), "map put ana");
            suite.check(map->put(transaction, "bia", 20).has_value(), "map put bia");
            suite.check(map->put(transaction, "ana", 15).has_value(), "map replace ana");
            auto ana = map->get("ana");
            suite.check(ana.has_value() && ana->has_value() && **ana == 15, "map get replaced value");
            auto removed = map->remove(transaction, "bia");
            suite.check(removed.has_value() && *removed, "map remove existing key");
            auto missing = map->get("bia");
            suite.check(missing.has_value() && !missing->has_value(), "removed key is absent");
            map_id = map->id();
            suite.check(database->flush().has_value(), "map flushed");
            if (database_id) {
                registry.detach(*database_id);
            }
        }
        auto reopened = Database::open(temp.path());
        auto database = share_database(reopened);
        suite.check(database != nullptr, "map database reopened");
        if (database) {
            auto blobs = database->blobs();
            PersistentMap<std::string, std::int64_t> map{blobs, map_id};
            auto ana = map.get("ana");
            auto size = map.size();
            suite.check(ana.has_value() && ana->has_value() && **ana == 15 && size.has_value() &&
                            *size == 1,
                        "map survives reopening with the right state");
        }
    }

    // --- grafo do critério: Ref + OwnedRef + PersistentVector<Ref> ---
    {
        TemporaryDatabase temp{"graph"};
        ObjectId emp_id{};
        ObjectId lead_id{};
        ObjectId office_id{};
        std::vector<ObjectId> project_ids;
        BlobId projects_blob{};

        {
            auto created = Database::create(temp.path());
            auto database = share_database(created);
            suite.check(database != nullptr, "graph database created");
            if (!database) {
                return suite.finish();
            }
            auto database_id = registry.attach(database);
            suite.check(database_id.has_value(), "graph database attached");
            suite.check(database->bind(project_builder()).has_value() &&
                            database->bind(address_builder()).has_value() &&
                            database->bind(employee_builder()).has_value(),
                        "graph types bound");

            auto lead = database->create(Project{"Apollo"});
            auto p2 = database->create(Project{"Gemini"});
            auto office = database->create(Address{"Rua A"});
            suite.check(lead && p2 && office, "projects and office created");
            if (!lead || !p2 || !office) {
                return suite.finish();
            }
            lead_id = lead->id();
            office_id = office->id();
            project_ids = {lead_id, p2->id()};

            // Monta a coleção de refs de projetos e guarda o BlobId no Employee.
            auto transaction = database->begin();
            auto blobs = database->blobs();
            auto projects = PersistentVector<Ref<Project>>::create(blobs);
            suite.check(projects.has_value(), "projects vector created");
            if (!projects) {
                return suite.finish();
            }
            for (auto id : project_ids) {
                suite.check(projects->push_back(transaction, Ref<Project>{id}).has_value(),
                            "project ref appended");
            }
            projects_blob = projects->id();

            Employee ana;
            ana.name = "Ana";
            ana.lead = Ref<Project>{lead_id};
            ana.office = OwnedRef<Address>{office_id};
            ana.projects = projects_blob;
            auto emp = database->create(ana);
            suite.check(emp.has_value(), "employee with graph created");
            if (!emp) {
                return suite.finish();
            }
            emp_id = emp->id();
            suite.check(database->flush().has_value(), "graph flushed");
            registry.detach(*database_id);
        }

        // Reabre, verifica tudo, remove Employee, confere cascata + projects.
        {
            auto opened = Database::open(temp.path());
            auto database = share_database(opened);
            suite.check(database != nullptr, "graph database reopened");
            if (!database) {
                return suite.finish();
            }
            auto database_id = registry.attach(database);
            suite.check(database_id.has_value() &&
                            database->bind(project_builder()).has_value() &&
                            database->bind(address_builder()).has_value() &&
                            database->bind(employee_builder()).has_value(),
                        "graph types rebound");

            auto handle = database->get<Employee>(emp_id);
            suite.check(handle.has_value(), "employee addressable after reopening");
            Employee ana;
            if (handle) {
                auto materialized = database->materialize(*handle);
                suite.check(materialized.has_value(), "employee materializes");
                if (materialized) {
                    ana = *materialized;
                }
            }
            suite.check(ana.lead.target == lead_id, "association preserved");
            suite.check(ana.office.target == office_id, "owned ref preserved");
            suite.check(ana.projects == projects_blob, "projects blob id preserved");

            // A coleção de refs resolve para os projetos certos.
            auto blobs = database->blobs();
            PersistentVector<Ref<Project>> projects{blobs, ana.projects};
            std::vector<ObjectId> resolved;
            auto walked = projects.for_each([&](const Ref<Project>& ref) -> Result<void> {
                resolved.push_back(ref.target);
                return {};
            });
            suite.check(walked.has_value() && resolved == project_ids,
                        "projects vector resolves to the right elements");

            // Remove Employee: o office (owned) some em cascata; os Projects
            // (associação/coleção) permanecem intactos.
            suite.check(database->remove(emp_id).has_value(), "employee removed");
            suite.check_error(database->get<Address>(office_id), ErrorCode::record_not_found,
                              "owned office removed in cascade");
            for (auto id : project_ids) {
                suite.check(database->get<Project>(id).has_value(),
                            "project survives (not owned)");
            }
            registry.detach(*database_id);
        }
    }

    return suite.finish();
}
