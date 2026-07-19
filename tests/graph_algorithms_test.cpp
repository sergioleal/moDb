#include "modb/graph/traversal.hpp"
#include "modb/object/binding.hpp"
#include "modb/object/collection.hpp"
#include "modb/object/database.hpp"
#include "modb/object/ref.hpp"
#include "modb/query/operators.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

using namespace modb;
using namespace modb::object;
using namespace modb::graph;
using namespace modb::query;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-12c-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
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

struct Node {
    std::string label;
    BlobId children{};
};

BindingBuilder<Node> node_builder() {
    BindingBuilder<Node> builder{"Node"};
    builder.field<1>("label", &Node::label).field<2>("children", &Node::children);
    return builder;
}

AdjacencyFn make_adjacency(Database& database, const Snapshot& snapshot) {
    return [&database, &snapshot](ObjectId from) -> Result<std::vector<ObjectId>> {
        auto node = database.get<Node>(from, snapshot);
        if (!node) {
            return std::unexpected(node.error());
        }
        if (node->children.value == 0) {
            return std::vector<ObjectId>{};
        }
        auto blobs = database.blobs();
        PersistentVector<Ref<Node>> vector{blobs, node->children};
        std::vector<ObjectId> neighbors;
        auto status = vector.for_each([&](const Ref<Node>& ref) -> Result<void> {
            neighbors.push_back(ref.target);
            return {};
        });
        if (!status) {
            return std::unexpected(status.error());
        }
        return neighbors;
    };
}

ResolveFn make_resolve(Database& database, const Snapshot& snapshot) {
    return [&database, &snapshot](ObjectId target) -> Result<bool> {
        auto node = database.get<Node>(target, snapshot);
        if (node) {
            return true;
        }
        if (node.error().code == ErrorCode::record_not_found) {
            return std::unexpected(
                Error{ErrorCode::edge_target_not_found, node.error().message});
        }
        return std::unexpected(node.error());
    };
}

Result<ObjectId> create_node(Database& database, Transaction& tx, std::string label,
                             const std::vector<ObjectId>& child_ids) {
    BlobId children{};
    if (!child_ids.empty()) {
        auto blobs = database.blobs();
        auto vector = PersistentVector<Ref<Node>>::create(blobs, tx);
        if (!vector) {
            return std::unexpected(vector.error());
        }
        for (const auto id : child_ids) {
            if (auto status = vector->push_back(tx, Ref<Node>{id}); !status) {
                return std::unexpected(status.error());
            }
        }
        children = vector->id();
    }
    auto created = database.create(tx, Node{.label = std::move(label), .children = children});
    if (!created) {
        return std::unexpected(created.error());
    }
    return created->id();
}

} // namespace

int main() {
    TestSuite suite;
    auto& registry = DatabaseRegistry::instance();
    TemporaryDatabase temp{"trav"};

    ObjectId root{};
    ObjectId a{};
    ObjectId b{};
    ObjectId c{};
    ObjectId d{};

    auto created = Database::create(temp.path());
    auto database = share_database(created);
    suite.check(database != nullptr, "create");
    if (!database) {
        return suite.finish();
    }
    auto database_id = registry.attach(database);
    suite.check(database_id.has_value() && database->bind(node_builder()).has_value(), "bind");

    {
        auto tx = database->begin();
        // Folhas primeiro (refs apontam para ids já criados).
        auto leaf_c = create_node(*database, *tx, "C", {});
        auto leaf_d = create_node(*database, *tx, "D", {});
        suite.check(leaf_c && leaf_d, "leaves");
        c = *leaf_c;
        d = *leaf_d;
        auto node_a = create_node(*database, *tx, "A", {c});
        auto node_b = create_node(*database, *tx, "B", {d});
        suite.check(node_a && node_b, "mid");
        a = *node_a;
        b = *node_b;
        auto node_root = create_node(*database, *tx, "R", {a, b});
        suite.check(node_root && tx->commit().has_value(), "root+commit");
        root = *node_root;
    }

    auto snap = database->snapshot();
    suite.check(snap.has_value(), "snapshot");
    auto adjacency = make_adjacency(*database, *snap);
    auto resolve = make_resolve(*database, *snap);

    // BFS: R, A, B, C, D
    {
        std::vector<std::string> labels;
        for (auto& visit : bfs(root, adjacency, {}, resolve)) {
            suite.check(visit.has_value(), "bfs visit ok");
            if (!visit) {
                break;
            }
            auto node = database->get<Node>(visit->id, *snap);
            suite.check(node.has_value(), "bfs materialize");
            if (node) {
                labels.push_back(node->label);
            }
        }
        suite.check(labels == std::vector<std::string>{"R", "A", "B", "C", "D"},
                    "BFS order R A B C D");
    }

    // DFS: R, A, C, B, D
    {
        std::vector<std::string> labels;
        for (auto& visit : dfs(root, adjacency, {}, resolve)) {
            suite.check(visit.has_value(), "dfs visit ok");
            if (!visit) {
                break;
            }
            auto node = database->get<Node>(visit->id, *snap);
            if (node) {
                labels.push_back(node->label);
            }
        }
        suite.check(labels == std::vector<std::string>{"R", "A", "C", "B", "D"},
                    "DFS order R A C B D");
    }

    // max_depth=1 → só R, A, B
    {
        TraversalOptions options{.max_depth = 1};
        std::vector<std::string> labels;
        for (auto& visit : bfs(root, adjacency, options, resolve)) {
            if (visit) {
                labels.push_back(database->get<Node>(visit->id, *snap)->label);
            }
        }
        suite.check(labels == std::vector<std::string>{"R", "A", "B"}, "BFS max_depth=1");
    }

    // max_vertices=3 → cede 3 e depois graph_limit_exceeded
    {
        TraversalOptions options{.max_vertices = 3};
        std::size_t ok_count = 0;
        bool saw_limit = false;
        for (auto& visit : bfs(root, adjacency, options, resolve)) {
            if (visit) {
                ++ok_count;
            } else {
                suite.check(visit.error().code == ErrorCode::graph_limit_exceeded,
                            "max_vertices → graph_limit_exceeded");
                saw_limit = true;
                break;
            }
        }
        suite.check(ok_count == 3 && saw_limit, "BFS stops after 3 vertices");
    }

    // Cancelamento interrompe sem erro.
    {
        CancellationToken token;
        TraversalOptions options{.cancel = token, .has_cancel = true};
        std::size_t count = 0;
        for (auto& visit : bfs(root, adjacency, options, resolve)) {
            suite.check(visit.has_value(), "cancel path visit ok");
            ++count;
            if (count == 2) {
                token.cancel();
            }
        }
        suite.check(count == 2, "cancel stops expansion");
    }

    // Ref órfã com fail.
    {
        ObjectId orphan_parent{};
        {
            auto tx = database->begin();
            auto orphan = create_node(*database, *tx, "OrphanParent", {ObjectId{9'999}});
            suite.check(orphan && tx->commit().has_value(), "orphan parent");
            orphan_parent = *orphan;
        }
        auto snap2 = database->snapshot();
        auto adj2 = make_adjacency(*database, *snap2);
        auto res2 = make_resolve(*database, *snap2);
        TraversalOptions options{.dangling = DanglingPolicy::fail};
        bool saw_error = false;
        for (auto& visit : bfs(orphan_parent, adj2, options, res2)) {
            if (!visit) {
                suite.check(visit.error().code == ErrorCode::edge_target_not_found,
                            "dangling fail");
                saw_error = true;
                break;
            }
        }
        suite.check(saw_error, "fail policy aborts");
    }

    // skip: cede só o pai
    {
        ObjectId orphan_parent{};
        {
            auto tx = database->begin();
            auto orphan = create_node(*database, *tx, "SkipParent", {ObjectId{9'998}});
            suite.check(orphan && tx->commit().has_value(), "skip parent");
            orphan_parent = *orphan;
        }
        auto snap2 = database->snapshot();
        auto adj2 = make_adjacency(*database, *snap2);
        auto res2 = make_resolve(*database, *snap2);
        TraversalOptions options{.dangling = DanglingPolicy::skip};
        std::size_t count = 0;
        bool failed = false;
        for (auto& visit : bfs(orphan_parent, adj2, options, res2)) {
            if (!visit) {
                failed = true;
                break;
            }
            ++count;
        }
        suite.check(!failed && count == 1, "skip policy yields only parent");
    }

    registry.detach(*database_id);
    return suite.finish();
}
