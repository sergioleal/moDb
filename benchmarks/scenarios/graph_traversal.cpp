#include "scenarios/graph_traversal.hpp"

#include "modb/graph/traversal.hpp"
#include "modb/object/collection.hpp"
#include "modb/object/database.hpp"
#include "modb/object/ref.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

using namespace modb;
using namespace modb::object;
using namespace modb::graph;

namespace modb::bench {
namespace {

struct GNode {
    std::string label;
    BlobId children{};
};

BindingBuilder<GNode> gnode_binding() {
    BindingBuilder<GNode> builder{"GNode"};
    builder.field<1>("label", &GNode::label).field<2>("children", &GNode::children);
    return builder;
}

std::uint64_t ns_between(std::chrono::steady_clock::time_point a,
                         std::chrono::steady_clock::time_point b) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

Result<ObjectId> create_tree(Database& database, Transaction& tx, std::uint32_t branching,
                             std::uint32_t depth_left, std::uint64_t& counter) {
    const std::string label = "n" + std::to_string(counter++);
    BlobId children{};
    if (depth_left > 0 && branching > 0) {
        auto blobs = database.blobs();
        auto vector = PersistentVector<Ref<GNode>>::create(blobs, tx);
        if (!vector) {
            return std::unexpected(vector.error());
        }
        for (std::uint32_t i = 0; i < branching; ++i) {
            auto child = create_tree(database, tx, branching, depth_left - 1, counter);
            if (!child) {
                return std::unexpected(child.error());
            }
            if (auto status = vector->push_back(tx, Ref<GNode>{*child}); !status) {
                return std::unexpected(status.error());
            }
        }
        children = vector->id();
    }
    auto created = database.create(tx, GNode{.label = label, .children = children});
    if (!created) {
        return std::unexpected(created.error());
    }
    return created->id();
}

AdjacencyFn make_adj(Database& database, const Snapshot& snapshot) {
    return [&database, &snapshot](ObjectId from) -> Result<std::vector<ObjectId>> {
        auto node = database.get<GNode>(from, snapshot);
        if (!node) {
            return std::unexpected(node.error());
        }
        if (node->children.value == 0) {
            return std::vector<ObjectId>{};
        }
        auto blobs = database.blobs();
        PersistentVector<Ref<GNode>> vector{blobs, node->children};
        std::vector<ObjectId> neighbors;
        auto status = vector.for_each([&](const Ref<GNode>& ref) -> Result<void> {
            neighbors.push_back(ref.target);
            return {};
        });
        if (!status) {
            return std::unexpected(status.error());
        }
        return neighbors;
    };
}

} // namespace

SampleResult run_graph_traversal(const GraphTraversalParams& params) {
    SampleResult result;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::path{params.work_dir} /
                      ("graph-trav-" + std::to_string(params.seed) + "-" +
                       std::to_string(unique) + ".modb");

    ObjectId root{};
    {
        auto created = Database::create(path);
        if (!created) {
            result.valid = false;
            result.error = "Database::create: " + created.error().message;
            return result;
        }
        auto database = std::make_shared<Database>(std::move(*created));
        auto database_id = DatabaseRegistry::instance().attach(database);
        if (!database_id) {
            result.valid = false;
            result.error = "attach: " + database_id.error().message;
            return result;
        }
        if (auto bound = database->bind(gnode_binding()); !bound) {
            DatabaseRegistry::instance().detach(*database_id);
            result.valid = false;
            result.error = "bind: " + bound.error().message;
            return result;
        }
        auto tx = database->begin();
        if (!tx) {
            DatabaseRegistry::instance().detach(*database_id);
            result.valid = false;
            result.error = "begin: " + tx.error().message;
            return result;
        }
        std::uint64_t counter = 0;
        auto built =
            create_tree(*database, *tx, params.branching, params.depth, counter);
        if (!built || !tx->commit()) {
            DatabaseRegistry::instance().detach(*database_id);
            result.valid = false;
            result.error = "seed tree failed";
            return result;
        }
        root = *built;
        result.metrics.operations = counter;
        DatabaseRegistry::instance().detach(*database_id);
    }

    // cold = reabrir o arquivo; warm = reabrir também (processo novo por sample),
    // mas o cache de páginas aquece na primeira passagem se warm pedir duas BFS.
    auto opened = Database::open(path);
    if (!opened) {
        result.valid = false;
        result.error = "Database::open: " + opened.error().message;
        return result;
    }
    auto database = std::make_shared<Database>(std::move(*opened));
    auto database_id = DatabaseRegistry::instance().attach(database);
    if (!database_id || !database->bind(gnode_binding())) {
        if (database_id) {
            DatabaseRegistry::instance().detach(*database_id);
        }
        result.valid = false;
        result.error = "reopen/bind failed";
        return result;
    }

    auto snap = database->snapshot();
    if (!snap) {
        DatabaseRegistry::instance().detach(*database_id);
        result.valid = false;
        result.error = "snapshot: " + snap.error().message;
        return result;
    }
    auto adjacency = make_adj(*database, *snap);

    if (params.cache_state == "warm") {
        for (auto& visit : bfs(root, adjacency)) {
            if (!visit) {
                DatabaseRegistry::instance().detach(*database_id);
                result.valid = false;
                result.error = "warmup bfs: " + visit.error().message;
                return result;
            }
        }
    }

    std::uint64_t peak_visited = 0;
    std::uint64_t yielded = 0;
    const auto t0 = std::chrono::steady_clock::now();
    std::unordered_set<std::uint64_t> seen;
    for (auto& visit : bfs(root, adjacency)) {
        if (!visit) {
            DatabaseRegistry::instance().detach(*database_id);
            result.valid = false;
            result.error = "bfs: " + visit.error().message;
            return result;
        }
        seen.insert(visit->id.value);
        peak_visited = (std::max)(peak_visited, static_cast<std::uint64_t>(seen.size()));
        ++yielded;
    }
    const auto t1 = std::chrono::steady_clock::now();

    result.metrics.elapsed_ns = ns_between(t0, t1);
    result.metrics.create_ns = peak_visited; // reutilizado: pico do visited-set
    result.metrics.objects_per_second =
        result.metrics.elapsed_ns == 0
            ? 0.0
            : (static_cast<double>(yielded) * 1'000'000'000.0) /
                  static_cast<double>(result.metrics.elapsed_ns);

    std::error_code ignored;
    result.metrics.peak_file_bytes =
        static_cast<std::uint64_t>(std::filesystem::file_size(path, ignored));

    DatabaseRegistry::instance().detach(*database_id);
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);
    return result;
}

} // namespace modb::bench
