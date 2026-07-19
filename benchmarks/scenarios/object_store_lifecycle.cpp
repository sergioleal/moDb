#include "scenarios/object_store_lifecycle.hpp"

#include "modb/object/database.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace modb;
using namespace modb::object;

namespace modb::bench {
namespace {

struct Item {
    std::int64_t seq{};
    std::string label;
};

BindingBuilder<Item> item_binding() {
    BindingBuilder<Item> builder{"Item"};
    builder.field<1>("seq", &Item::seq).field<2>("label", &Item::label);
    return builder;
}

std::uint64_t ns_between(std::chrono::steady_clock::time_point a,
                         std::chrono::steady_clock::time_point b) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

struct AttachedDatabase {
    std::shared_ptr<Database> database;
    DatabaseId id{};
    std::filesystem::path path;

    ~AttachedDatabase() {
        if (id.value != 0) {
            DatabaseRegistry::instance().detach(id);
        }
        database.reset();
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(std::filesystem::path{path.string() + ".wal"}, ignored);
        }
    }
};

} // namespace

SampleResult run_object_store_lifecycle(const ScenarioParams& params) {
    SampleResult result;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    AttachedDatabase attached;
    attached.path = std::filesystem::path{params.work_dir} /
                    ("lifecycle-" + std::to_string(params.seed) + "-" +
                     std::to_string(params.stride) + "-" + std::to_string(unique) + ".modb");

    auto created = Database::create(attached.path);
    if (!created) {
        result.valid = false;
        result.comparable = false;
        result.error = "Database::create: " + created.error().message;
        return result;
    }
    attached.database = std::make_shared<Database>(std::move(*created));
    auto database_id = DatabaseRegistry::instance().attach(attached.database);
    if (!database_id) {
        result.valid = false;
        result.comparable = false;
        result.error = "DatabaseRegistry::attach: " + database_id.error().message;
        return result;
    }
    attached.id = *database_id;

    if (auto bound = attached.database->bind(item_binding()); !bound) {
        result.valid = false;
        result.comparable = false;
        result.error = "bind: " + bound.error().message;
        return result;
    }

    std::vector<ObjectId> ids;
    ids.reserve(params.object_count);

    auto create_tx = attached.database->begin();
    if (!create_tx) {
        result.valid = false;
        result.error = "begin(create): " + create_tx.error().message;
        return result;
    }

    const auto create_start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < params.object_count; ++i) {
        auto id = attached.database->create(
            *create_tx,
            Item{static_cast<std::int64_t>(i),
                 "item-" + std::to_string(params.seed) + "-" + std::to_string(i)});
        if (!id) {
            result.valid = false;
            result.error = "create: " + id.error().message;
            return result;
        }
        ids.push_back(id->id());
    }
    const auto create_end = std::chrono::steady_clock::now();
    if (auto committed = create_tx->commit(); !committed) {
        result.valid = false;
        result.error = "commit(create): " + committed.error().message;
        return result;
    }
    const auto create_flush_end = std::chrono::steady_clock::now();

    std::error_code size_error;
    const auto peak_bytes = std::filesystem::file_size(attached.path, size_error);

    auto delete_tx = attached.database->begin();
    if (!delete_tx) {
        result.valid = false;
        result.error = "begin(delete): " + delete_tx.error().message;
        return result;
    }

    const auto remove_at = [&](ObjectId id) -> bool {
        if (auto removed = attached.database->remove(*delete_tx, id); !removed) {
            result.valid = false;
            result.error = "remove: " + removed.error().message;
            return false;
        }
        return true;
    };

    const auto delete_start = std::chrono::steady_clock::now();
    if (params.stride <= 1) {
        for (const auto id : ids) {
            if (!remove_at(id)) {
                return result;
            }
        }
    } else {
        for (std::uint64_t offset = 0; offset < params.stride; ++offset) {
            for (std::uint64_t i = offset; i < ids.size(); i += params.stride) {
                if (!remove_at(ids[i])) {
                    return result;
                }
            }
        }
    }
    const auto delete_end = std::chrono::steady_clock::now();
    if (auto committed = delete_tx->commit(); !committed) {
        result.valid = false;
        result.error = "commit(delete): " + committed.error().message;
        return result;
    }
    const auto delete_flush_end = std::chrono::steady_clock::now();

    std::uint64_t remaining = 0;
    for (const auto id : ids) {
        if (attached.database->get<Item>(id)) {
            ++remaining;
        }
    }
    if (remaining != 0) {
        result.valid = false;
        result.comparable = false;
        result.error = std::to_string(remaining) + " objeto(s) permaneceram apos o delete";
        return result;
    }

    const auto create_ns = ns_between(create_start, create_end);
    const auto create_flush_ns = ns_between(create_end, create_flush_end);
    const auto delete_ns = ns_between(delete_start, delete_end);
    const auto delete_flush_ns = ns_between(delete_end, delete_flush_end);
    const auto elapsed_ns = create_ns + delete_ns;
    const auto operations = params.object_count * 2;

    result.metrics.operations = operations;
    result.metrics.elapsed_ns = elapsed_ns;
    result.metrics.create_ns = create_ns;
    result.metrics.create_flush_ns = create_flush_ns;
    result.metrics.delete_ns = delete_ns;
    result.metrics.delete_flush_ns = delete_flush_ns;
    result.metrics.peak_file_bytes = size_error ? 0 : peak_bytes;
    result.metrics.objects_per_second =
        elapsed_ns > 0
            ? (static_cast<double>(operations) * 1'000'000'000.0) / static_cast<double>(elapsed_ns)
            : 0.0;
    return result;
}

} // namespace modb::bench
