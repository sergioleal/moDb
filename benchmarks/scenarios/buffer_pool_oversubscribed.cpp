#include "scenarios/buffer_pool_oversubscribed.hpp"

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

} // namespace

SampleResult run_buffer_pool_oversubscribed(const BufferPoolParams& params) {
    SampleResult result;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::path{params.work_dir} /
                      ("buffer-pool-" + std::to_string(params.seed) + "-" +
                       std::to_string(unique) + ".modb");

    auto created = Database::create(path, params.cache_pages);
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
    if (auto bound = database->bind(item_binding()); !bound) {
        DatabaseRegistry::instance().detach(*database_id);
        result.valid = false;
        result.error = "bind: " + bound.error().message;
        return result;
    }

    std::vector<ObjectId> ids;
    ids.reserve(params.object_count);

    auto tx = database->begin();
    if (!tx) {
        DatabaseRegistry::instance().detach(*database_id);
        result.valid = false;
        result.error = "begin: " + tx.error().message;
        return result;
    }

    const auto create_start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < params.object_count; ++i) {
        auto id = database->create(
            *tx, Item{static_cast<std::int64_t>(i),
                      "bp-" + std::to_string(params.seed) + "-" + std::to_string(i)});
        if (!id) {
            DatabaseRegistry::instance().detach(*database_id);
            result.valid = false;
            result.error = "create: " + id.error().message;
            return result;
        }
        ids.push_back(id->id());
    }
    if (auto committed = tx->commit(); !committed) {
        DatabaseRegistry::instance().detach(*database_id);
        result.valid = false;
        result.error = "commit: " + committed.error().message;
        return result;
    }
    const auto create_end = std::chrono::steady_clock::now();

    const auto read_start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < ids.size(); ++i) {
        auto handle = database->get<Item>(ids[i]);
        if (!handle) {
            DatabaseRegistry::instance().detach(*database_id);
            result.valid = false;
            result.error = "get: " + handle.error().message;
            return result;
        }
        auto got = database->materialize(*handle);
        if (!got || got->seq != static_cast<std::int64_t>(i)) {
            DatabaseRegistry::instance().detach(*database_id);
            result.valid = false;
            result.comparable = false;
            result.error = "validacao falhou no objeto " + std::to_string(i);
            return result;
        }
    }
    const auto read_end = std::chrono::steady_clock::now();

    const auto create_ns = ns_between(create_start, create_end);
    const auto read_ns = ns_between(read_start, read_end);
    const auto elapsed_ns = create_ns + read_ns;
    const auto operations = params.object_count * 2;

    result.metrics.operations = operations;
    result.metrics.elapsed_ns = elapsed_ns;
    result.metrics.create_ns = create_ns;
    result.metrics.delete_ns = read_ns; // reusa campo: tempo de read-back
    result.metrics.objects_per_second =
        elapsed_ns > 0
            ? (static_cast<double>(operations) * 1'000'000'000.0) / static_cast<double>(elapsed_ns)
            : 0.0;

    DatabaseRegistry::instance().detach(*database_id);
    database.reset();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(std::filesystem::path{path.string() + ".wal"}, ignored);
    return result;
}

} // namespace modb::bench
