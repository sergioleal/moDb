#include "scenarios/storage_async_io.hpp"

#include "modb/object/database.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

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

SampleResult run_storage_async_io(const StorageAsyncIoParams& params) {
    SampleResult result;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::path{params.work_dir} /
                      ("storage-async-io-" + params.wal_io + "-" + std::to_string(params.seed) +
                       "-" + std::to_string(unique) + ".modb");

    DatabaseOptions opts;
    opts.wal_io = params.wal_io == "async" ? WalIoMode::async : WalIoMode::sync;

    auto created = Database::create(path, opts);
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

    const std::uint32_t stride = params.stride == 0 ? 1 : params.stride;
    std::uint64_t committed_objects = 0;

    const auto commit_start = std::chrono::steady_clock::now();
    for (std::uint64_t t = 0; t < params.object_count; ++t) {
        auto tx = database->begin();
        if (!tx) {
            DatabaseRegistry::instance().detach(*database_id);
            result.valid = false;
            result.error = "begin: " + tx.error().message;
            return result;
        }
        for (std::uint32_t i = 0; i < stride; ++i) {
            auto id = database->create(
                *tx, Item{static_cast<std::int64_t>(t * stride + i),
                          "aio-" + std::to_string(params.seed) + "-" + std::to_string(t) + "-" +
                              std::to_string(i)});
            if (!id) {
                DatabaseRegistry::instance().detach(*database_id);
                result.valid = false;
                result.error = "create: " + id.error().message;
                return result;
            }
        }
        if (auto committed = tx->commit(); !committed) {
            DatabaseRegistry::instance().detach(*database_id);
            result.valid = false;
            result.error = "commit: " + committed.error().message;
            return result;
        }
        committed_objects += stride;
    }
    const auto commit_end = std::chrono::steady_clock::now();

    const auto elapsed_ns = ns_between(commit_start, commit_end);
    result.metrics.operations = params.object_count; // transações commitadas
    result.metrics.elapsed_ns = elapsed_ns;
    result.metrics.create_ns = elapsed_ns; // tempo total de commit (begin+writes+sync)
    result.metrics.objects_per_second =
        elapsed_ns > 0 ? (static_cast<double>(committed_objects) * 1'000'000'000.0) /
                             static_cast<double>(elapsed_ns)
                       : 0.0;

    DatabaseRegistry::instance().detach(*database_id);
    database.reset();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(std::filesystem::path{path.string() + ".wal"}, ignored);
    return result;
}

} // namespace modb::bench
