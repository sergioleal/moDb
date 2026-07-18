// Valida Sort, Top-K, Distinct, Aggregate e Merge da Fase 7D: resultados
// corretos, natureza dos operadores e pico O(k) no Top-K.
#include "modb/object/database.hpp"
#include "modb/query/operators.hpp"
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
                ("modb-agg-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
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

Result<void> seed(Database& database, std::vector<Item> items) {
    auto tx = database.begin();
    if (!tx) {
        return std::unexpected(tx.error());
    }
    for (auto& item : items) {
        auto handle = database.create(*tx, item);
        if (!handle) {
            return std::unexpected(handle.error());
        }
    }
    return tx->commit();
}

Generator<Result<int>> from_values(std::vector<int> values) {
    for (int value : values) {
        co_yield Result<int>{value};
    }
}

} // namespace

int main() {
    TestSuite suite;

    suite.check(nature_of_sort() == OperatorNature::blocking, "sort is classified as blocking");
    suite.check(nature_of_top_k() == OperatorNature::partially_blocking,
                "top_k is classified as partially_blocking");
    suite.check(nature_of_distinct() == OperatorNature::blocking,
                "distinct is classified as blocking");
    suite.check(nature_of_aggregate() == OperatorNature::blocking,
                "aggregate is classified as blocking");
    suite.check(nature_of_merge() == OperatorNature::streaming,
                "merge of sorted inputs is classified as streaming");
    suite.check(std::string{nature_name(OperatorNature::blocking)} == "blocking",
                "nature_name is stable");

    // --- sort + top_k isolados ---
    {
        std::size_t peak = 0;
        std::vector<int> sorted;
        for (auto& item : sort(from_values({5, 1, 4, 2, 3}), std::less<int>{}, &peak)) {
            if (item) {
                sorted.push_back(*item);
            }
        }
        suite.check(sorted == std::vector<int>{1, 2, 3, 4, 5}, "sort orders the full input");
        suite.check(peak == 5, "sort peak equals input size (blocking)");
    }
    {
        std::size_t peak = 0;
        std::vector<int> top;
        for (auto& item : top_k(from_values({5, 1, 9, 2, 7, 3}), 3, std::less<int>{}, &peak)) {
            if (item) {
                top.push_back(*item);
            }
        }
        suite.check(top == std::vector<int>{9, 7, 5}, "top_k yields the three largest");
        suite.check(peak <= 3, "top_k peak stays within O(k)");
    }

    // --- distinct + aggregate + merge isolados ---
    {
        auto key = [](int value) { return value % 10; };
        std::vector<int> got;
        for (auto& item : distinct(from_values({11, 21, 12, 22, 11}), key)) {
            if (item) {
                got.push_back(*item);
            }
        }
        suite.check(got == std::vector<int>{11, 12}, "distinct keeps the first key occurrence");
    }
    {
        std::int64_t sum = 0;
        for (auto& item : aggregate(from_values({1, 2, 3, 4}), std::int64_t{0},
                                    [](std::int64_t acc, int value) { return acc + value; })) {
            if (item) {
                sum = *item;
            }
        }
        suite.check(sum == 10, "aggregate folds the whole stream");
    }
    {
        std::vector<int> merged;
        for (auto& item : merge(from_values({1, 3, 5}), from_values({2, 4, 6}))) {
            if (item) {
                merged.push_back(*item);
            }
        }
        suite.check(merged == std::vector<int>{1, 2, 3, 4, 5, 6},
                    "merge combines two sorted streams");
    }

    TemporaryDatabase temp{"main"};
    auto created = Database::create(temp.path());
    auto database = share(created);
    suite.check(database != nullptr, "aggregation database is created");
    if (!database) {
        return suite.finish();
    }
    auto database_id = DatabaseRegistry::instance().attach(database);
    suite.check(database_id.has_value(), "aggregation database is attached");
    suite.check(database->bind(item_builder()).has_value(), "Item is bound");
    suite.check(seed(*database,
                     {Item{"c", 3}, Item{"a", 1}, Item{"b", 2}, Item{"a", 4}, Item{"d", 5}})
                    .has_value(),
                "dataset is seeded");

    // --- order_by na API ---
    {
        std::vector<std::int64_t> values;
        auto query = database->query<Item>().order_by(
            [](const Item& left, const Item& right) { return left.value < right.value; });
        suite.check(query.nature() == OperatorNature::blocking, "order_by plan is blocking");
        for (auto& result : std::move(query).stream()) {
            if (result) {
                values.push_back(result->value);
            }
        }
        suite.check(values == std::vector<std::int64_t>{1, 2, 3, 4, 5},
                    "query.order_by sorts by value");
    }

    // --- top_k na API com pico O(k) ---
    {
        std::size_t peak = 0;
        std::vector<std::int64_t> values;
        auto query =
            database->query<Item>()
                .top_k(2, [](const Item& left, const Item& right) { return left.value < right.value; })
                .track_peak(&peak);
        suite.check(query.nature() == OperatorNature::partially_blocking,
                    "top_k plan is partially_blocking");
        for (auto& result : std::move(query).stream()) {
            if (result) {
                values.push_back(result->value);
            }
        }
        suite.check(values == std::vector<std::int64_t>{5, 4}, "query.top_k yields the two largest");
        suite.check(peak <= 2, "query.top_k peak is O(k)");
    }

    // --- distinct_by ---
    {
        std::vector<std::string> names;
        for (auto& result :
             database->query<Item>()
                 .order_by([](const Item& left, const Item& right) { return left.name < right.name; })
                 .distinct_by([](const Item& item) { return item.name; })
                 .stream()) {
            if (result) {
                names.push_back(result->name);
            }
        }
        suite.check(names == std::vector<std::string>{"a", "b", "c", "d"},
                    "distinct_by keeps one row per name");
    }

    // --- aggregate count ---
    {
        std::size_t count = 0;
        for (auto& result : database->query<Item>().aggregate(
                 std::size_t{0}, [](std::size_t acc, const Item&) { return acc + 1; })) {
            if (result) {
                count = *result;
            }
        }
        suite.check(count == 5, "query.aggregate counts every object");
    }

    DatabaseRegistry::instance().detach(*database_id);
    return suite.finish();
}
