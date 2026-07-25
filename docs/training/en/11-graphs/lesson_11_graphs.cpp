// Lesson 11 -- Graphs: The Reporting Chain.
// Builds on Lesson 10 (lesson_10_facades.cpp) -- opens the SAME database
// file and adds a `manager` field to Employee.
// See docs/training/en/11-graphs/11-graphs.md.

#include "modb/graph/algorithms.hpp"
#include "modb/graph/edge_handle.hpp"
#include "modb/graph/graph_view.hpp"
#include "modb/graph/traversal.hpp"
#include "modb/object/collection.hpp"
#include "modb/object/database.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

using modb::object::AttributeValue;
using modb::object::BlobId;
using modb::object::FieldId;
using modb::object::ObjectId;
using modb::object::OwnedRef;
using modb::object::Ref;

namespace {

struct Department {
    std::string name;
};

modb::object::BindingBuilder<Department> department_binding() {
    modb::object::BindingBuilder<Department> builder{"Department"};
    builder.field<1>("name", &Department::name);
    return builder;
}

struct EmergencyContact {
    std::string name;
    std::string phone;
};

modb::object::BindingBuilder<EmergencyContact> emergency_contact_binding() {
    modb::object::BindingBuilder<EmergencyContact> builder{"EmergencyContact"};
    builder.field<1>("name", &EmergencyContact::name).field<2>("phone", &EmergencyContact::phone);
    return builder;
}

struct Project {
    std::string name;
};

modb::object::BindingBuilder<Project> project_binding() {
    modb::object::BindingBuilder<Project> builder{"Project"};
    builder.field<1>("name", &Project::name);
    return builder;
}

// Lesson 5's shape plus `manager`, a Ref back to the same type -- the
// reporting chain is just another relationship, and the graph module
// walks it without needing a dedicated "graph type".
struct EmployeeV4 {
    std::string name;
    std::int64_t salary{};
    std::string country;
    Ref<Department> department{};
    OwnedRef<EmergencyContact> emergency_contact{};
    BlobId projects{};
    Ref<EmployeeV4> manager{};
};

modb::object::BindingBuilder<EmployeeV4> employee_v4_binding() {
    modb::object::BindingBuilder<EmployeeV4> builder{"Employee"};
    builder.field<1>("name", &EmployeeV4::name)
        .field<2>("salary", &EmployeeV4::salary)
        .field<3>("country", &EmployeeV4::country, "BR")
        .field<4>("department", &EmployeeV4::department, Ref<Department>{})
        .field<5>("emergency_contact", &EmployeeV4::emergency_contact, OwnedRef<EmergencyContact>{})
        .field<6>("projects", &EmployeeV4::projects, BlobId{})
        .field<7>("manager", &EmployeeV4::manager, Ref<EmployeeV4>{});
    return builder;
}

constexpr FieldId kNameField{1};
constexpr FieldId kManagerField{7};

std::filesystem::path db_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory.modb";
}

modb::Result<ObjectId> find_employee_id(modb::object::Database& database, std::string_view name) {
    auto matches =
        database.indexed_object_ids<EmployeeV4>(kNameField, AttributeValue{std::string{name}});
    if (!matches) {
        return std::unexpected(matches.error());
    }
    if (matches->empty()) {
        return std::unexpected(
            modb::Error{modb::ErrorCode::record_not_found, "no employee named " + std::string{name}});
    }
    return (*matches)[0];
}

} // namespace

int main() {
    std::cout << "Objective: walk the reporting chain with the graph module.\n";
    const auto path = db_path();

    auto opened = modb::object::Database::open(path);
    if (!opened) {
        std::cerr << opened.error().message
                  << " -- have you run Lessons 1-10 first? Expected a database at " << path.string()
                  << '\n';
        return 1;
    }
    auto database = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    // Rebinding with `manager` added is this lesson's schema evolution --
    // the fourth one this database has been through, on top of Lesson 4's
    // `country` and Lesson 5's relationship fields.
    if (!attached || !database->bind(employee_v4_binding()) || !database->bind(department_binding()) ||
        !database->bind(emergency_contact_binding()) || !database->bind(project_binding())) {
        std::cerr << "failed to bind Lesson 11 types\n";
        return 1;
    }
    if (auto created = database->create_index<EmployeeV4>(kManagerField); !created) {
        std::cerr << created.error().message << '\n';
        return 1;
    }

    auto ana_id = find_employee_id(*database, "Ana");
    auto bruno_id = find_employee_id(*database, "Bruno");
    auto carla_id = find_employee_id(*database, "Carla");
    if (!ana_id || !bruno_id || !carla_id) {
        std::cerr << "failed to look up Ana/Bruno/Carla\n";
        return 1;
    }

    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto ana_handle = database->get<EmployeeV4>(*ana_id);
        auto bruno_handle = database->get<EmployeeV4>(*bruno_id);
        if (!ana_handle || !bruno_handle) {
            std::cerr << "failed to look up Ana/Bruno\n";
            return 1;
        }
        if (auto set = ana_handle->set<&EmployeeV4::manager>(*tx, Ref<EmployeeV4>{*carla_id}); !set) {
            std::cerr << set.error().message << '\n';
            return 1;
        }
        if (auto set = bruno_handle->set<&EmployeeV4::manager>(*tx, Ref<EmployeeV4>{*carla_id});
            !set) {
            std::cerr << set.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit manager assignments\n";
            return 1;
        }
    }
    std::cout << "Ana and Bruno now report to Carla\n";

    // --- Walk the org chart downward from Carla using GraphView::incoming. ---
    // `incoming` finds every EmployeeV4 whose `manager` field points at the
    // given target -- i.e. direct reports -- which is exactly the adjacency
    // a top-down org chart needs, even though the field itself only stores
    // the upward pointer.
    {
        auto snapshot_result = database->snapshot();
        if (!snapshot_result) {
            std::cerr << snapshot_result.error().message << '\n';
            return 1;
        }
        const auto& snapshot = *snapshot_result;
        auto view = modb::graph::open_graph_view(*database, snapshot);
        if (!view) {
            std::cerr << view.error().message << '\n';
            return 1;
        }
        auto adjacency_down = [&database,
                               &view](ObjectId from) -> modb::Result<std::vector<ObjectId>> {
            auto handle = database->get<EmployeeV4>(from);
            if (!handle) {
                return std::unexpected(handle.error());
            }
            auto edges = view->incoming<EmployeeV4, EmployeeV4>(*handle, kManagerField,
                                                                 &EmployeeV4::manager);
            if (!edges) {
                return std::unexpected(edges.error());
            }
            std::vector<ObjectId> reports;
            reports.reserve(edges->size());
            for (const auto& report_edge : *edges) {
                reports.push_back(report_edge.source_id());
            }
            return reports;
        };

        std::cout << "Org chart under Carla (breadth-first)\n";
        for (auto& item : modb::graph::bfs(*carla_id, adjacency_down)) {
            if (!item) {
                std::cerr << item.error().message << '\n';
                return 1;
            }
            auto employee = database->get<EmployeeV4>(item->id, snapshot);
            if (!employee) {
                std::cerr << employee.error().message << '\n';
                return 1;
            }
            std::cout << "  depth " << item->depth << ": " << employee->name << '\n';
        }
    }

    // --- A dangling manager: create a temp employee, point another one at
    // them, then remove the manager. The reporting-up walk now has to decide
    // what to do about the vanished target. ---
    ObjectId felipe_id{};
    ObjectId gustavo_id{};
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto felipe = database->create(
            *tx, EmployeeV4{"Felipe", 16000, "BR", Ref<Department>{}, OwnedRef<EmergencyContact>{},
                            BlobId{}, Ref<EmployeeV4>{}});
        if (!felipe) {
            std::cerr << felipe.error().message << '\n';
            return 1;
        }
        felipe_id = felipe->id();
        auto gustavo = database->create(
            *tx, EmployeeV4{"Gustavo", 9000, "BR", Ref<Department>{}, OwnedRef<EmergencyContact>{},
                            BlobId{}, Ref<EmployeeV4>{felipe_id}});
        if (!gustavo) {
            std::cerr << gustavo.error().message << '\n';
            return 1;
        }
        gustavo_id = gustavo->id();
        if (!tx->commit()) {
            std::cerr << "failed to commit Felipe and Gustavo\n";
            return 1;
        }
    }
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        if (auto removed = database->remove(*tx, felipe_id); !removed) {
            std::cerr << removed.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit Felipe's removal\n";
            return 1;
        }
    }
    std::cout << "Removed Gustavo's manager Felipe (" << felipe_id.value
              << ") -- Gustavo's `manager` field now points nowhere\n";

    // Walking up from Gustavo follows the single `manager` Ref outward; the
    // target no longer resolves, so every policy has to make a choice.
    auto adjacency_up = [&database](ObjectId from) -> modb::Result<std::vector<ObjectId>> {
        auto handle = database->get<EmployeeV4>(from);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        auto manager_edge = modb::graph::edge<EmployeeV4, EmployeeV4>(*database, *handle,
                                                                      kManagerField,
                                                                      &EmployeeV4::manager);
        if (!manager_edge) {
            return std::unexpected(manager_edge.error());
        }
        if (manager_edge->target_id().value == 0) {
            return std::vector<ObjectId>{};
        }
        return std::vector<ObjectId>{manager_edge->target_id()};
    };
    auto resolve_up = [&database](ObjectId target) -> modb::Result<bool> {
        // Propagate record_not_found as an ERROR rather than swallowing it
        // into `false` -- accept_neighbor() only consults DanglingPolicy
        // when resolve() itself fails; a plain `false` return would always
        // mean "skip", regardless of which policy the caller asked for.
        auto handle = database->get<EmployeeV4>(target);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        return true;
    };

    std::cout << "Walking up from Gustavo with DanglingPolicy::fail\n";
    for (auto& item : modb::graph::bfs(gustavo_id, adjacency_up,
                                       modb::graph::TraversalOptions{
                                           .dangling = modb::graph::DanglingPolicy::fail},
                                       resolve_up)) {
        if (!item) {
            std::cout << "  stopped: " << item.error().message << '\n';
            break;
        }
        std::cout << "  visit " << item->id.value << " depth=" << item->depth << '\n';
    }

    std::cout << "Walking up from Gustavo with DanglingPolicy::skip\n";
    for (auto& item : modb::graph::bfs(gustavo_id, adjacency_up,
                                       modb::graph::TraversalOptions{
                                           .dangling = modb::graph::DanglingPolicy::skip},
                                       resolve_up)) {
        if (!item) {
            std::cerr << item.error().message << '\n';
            return 1;
        }
        std::cout << "  visit " << item->id.value << " depth=" << item->depth << '\n';
    }
    std::cout << "  (walk ended quietly -- the dangling edge was dropped, not reported)\n";

    std::cout << "Walking up from Gustavo with DanglingPolicy::yield_error\n";
    for (auto& item : modb::graph::bfs(gustavo_id, adjacency_up,
                                       modb::graph::TraversalOptions{
                                           .dangling = modb::graph::DanglingPolicy::yield_error},
                                       resolve_up)) {
        if (!item) {
            std::cout << "  error item: " << item.error().message << '\n';
            continue;
        }
        std::cout << "  visit " << item->id.value << " depth=" << item->depth << '\n';
    }
    std::cout << "  (unlike fail, yield_error would have let the walk continue into any other, "
                 "unrelated branch -- Gustavo only had the one dangling edge, so the visible "
                 "output matches fail's here)\n";

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return 0;
}
