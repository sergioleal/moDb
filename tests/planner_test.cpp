// Valida o planner da Fase 7E: escolha Index Scan vs Scan+Predicate, pushdown
// de Limit, selecao automatica de Top-K (order_by+limit), nature()/
// first_result_cost() e benchmarks reproduziveis de TTFR, pico O(k) e ganho
// de indice.
#include "modb/object/database.hpp"
#include "modb/query/planner.hpp"
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
using namespace modb::query;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-plan-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        auto wal = path_;
        wal += ".wal";
        std::filesystem::remove(wal, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

struct Item {
    std::string name;
    std::int64_t value{};
    friend bool operator==(const Item&, const Item&) = default;
};

BindingBuilder<Item> item_builder() {
    BindingBuilder<Item> builder{"Item"};
    builder.field<1>("name", &Item::name).field<2>("value", &Item::value);
    return builder;
}

std::shared_ptr<Database> share(Result<Database>& result) {
    if (!result) {
        return {};
    }
    return std::make_shared<Database>(std::move(*result));
}

Result<void> seed(Database& database, int count) {
    auto tx = database.begin();
    if (!tx) {
        return std::unexpected(tx.error());
    }
    for (int i = 0; i < count; ++i) {
        auto handle = database.create(*tx, Item{"n" + std::to_string(i), i});
        if (!handle) {
            return std::unexpected(handle.error());
        }
    }
    return tx->commit();
}

} // namespace

int main() {
    TestSuite suite;

    {
        QueryIntent intent;
        intent.limit = 1;
        auto plan = plan_query(intent);
        suite.check(plan.access == AccessMethod::table_scan &&
                        plan.nature == OperatorNature::streaming && plan.limit_pushed &&
                        plan.first_result_cost == 1,
                    "plain limit plans a streaming table_scan with pushdown");
    }
    {
        QueryIntent intent;
        intent.index_requested = true;
        intent.index_available = true;
        intent.index_field = 2;
        intent.limit = 1;
        auto plan = plan_query(intent);
        suite.check(plan.access == AccessMethod::index_scan && plan.limit_pushed &&
                        plan.first_result_cost == 1,
                    "indexed equality + limit plans index_scan with pushdown");
    }
    {
        QueryIntent intent;
        intent.index_requested = true;
        intent.index_available = false;
        intent.index_field = 2;
        auto plan = plan_query(intent);
        suite.check(plan.access == AccessMethod::table_scan && plan.index_requested &&
                        !plan.index_available,
                    "missing index falls back to table_scan");
    }
    {
        QueryIntent intent;
        intent.has_order = true;
        intent.limit = 3;
        auto plan = plan_query(intent);
        suite.check(plan.uses_top_k && !plan.uses_sort &&
                        plan.nature == OperatorNature::partially_blocking &&
                        plan.first_result_cost == QueryPlan::kFullInput && !plan.limit_pushed,
                    "order_by+limit selects Top-K instead of full sort");
    }
    {
        QueryIntent intent;
        intent.has_order = true;
        auto plan = plan_query(intent);
        suite.check(plan.uses_sort && plan.nature == OperatorNature::blocking &&
                        plan.first_result_cost == QueryPlan::kFullInput,
                    "order_by alone is a blocking sort");
    }

    TemporaryDatabase temp{"main"};
    auto created = Database::create(temp.path());
    auto database = share(created);
    suite.check(database != nullptr, "planner database is created");
    if (!database) {
        return suite.finish();
    }
    auto database_id = DatabaseRegistry::instance().attach(database);
    suite.check(database_id.has_value(), "planner database is attached");
    suite.check(database->bind(item_builder()).has_value(), "Item is bound");
    constexpr int kRows = 5'000;
    suite.check(seed(*database, kRows).has_value(), "dataset is seeded");

    {
        auto before = database->query<Item>().equals(FieldId{2}, std::int64_t{42});
        suite.check(before.plan().access == AccessMethod::table_scan,
                    "equals without index is table_scan");
        suite.check(database->create_index<Item>(FieldId{2}).has_value(), "index on value created");
        auto after = database->query<Item>().equals(FieldId{2}, std::int64_t{42});
        const auto planned = after.plan();
        suite.check(planned.access == AccessMethod::index_scan && planned.index_available,
                    "equals with index plans index_scan");
        suite.check(planned.nature == OperatorNature::streaming && after.first_result_cost() == 1,
                    "index equality is streaming with first_result_cost=1");
        std::size_t hits = 0;
        for (auto& result : std::move(after).stream()) {
            if (result && result->value == 42) {
                ++hits;
            }
        }
        suite.check(hits == 1, "planned index_scan returns the equality hit");
    }

    {
        database->reset_data_pages_read();
        auto query = database->query<Item>().limit(1);
        suite.check(query.plan().limit_pushed && query.first_result_cost() == 1,
                    "limit-only plan pushes to the source");
        std::size_t got = 0;
        for (auto& result : std::move(query).stream()) {
            if (result) {
                ++got;
            }
        }
        suite.check(got == 1 && database->data_pages_read() <= 2,
                    "TTFR: limit 1 reads <= 2 data pages on a large collection");
    }

    {
        std::size_t peak = 0;
        std::vector<std::int64_t> values;
        auto query =
            database->query<Item>()
                .order_by([](const Item& a, const Item& b) { return a.value < b.value; })
                .limit(3)
                .track_peak(&peak);
        suite.check(query.plan().uses_top_k &&
                        query.nature() == OperatorNature::partially_blocking,
                    "order_by+limit selects Top-K in the live plan");
        for (auto& result : std::move(query).stream()) {
            if (result) {
                values.push_back(result->value);
            }
        }
        suite.check(values == std::vector<std::int64_t>{0, 1, 2},
                    "order_by+limit yields the three smallest values");
        suite.check(peak <= 3, "selected Top-K keeps peak O(k)");
    }

    {
        std::uint64_t full_pages = 0;
        database->reset_data_pages_read();
        for (auto& result : database->query<Item>().stream()) {
            (void)result;
        }
        full_pages = database->data_pages_read();
        suite.check(full_pages > 2, "full scan reads many data pages");

        database->reset_data_pages_read();
        std::size_t hits = 0;
        for (auto& result :
             database->query<Item>().equals(FieldId{2}, std::int64_t{100}).stream()) {
            if (result) {
                ++hits;
            }
        }
        const auto index_pages = database->data_pages_read();
        suite.check(hits == 1 && index_pages < full_pages,
                    "index scan reads fewer data pages than a full scan");
    }

    DatabaseRegistry::instance().detach(*database_id);
    return suite.finish();
}