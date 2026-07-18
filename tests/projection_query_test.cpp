// Valida Projection e Computed Functions da Fase 7C: composição com filter/
// limit, preguiça (upstream só avança o necessário), registro de funções e
// ProjectedRow com apenas os campos pedidos.
#include "modb/object/database.hpp"
#include "modb/query/operators.hpp"
#include "modb/query/projected_row.hpp"
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
                ("modb-proj-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
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
        auto handle = database.create(*tx, Item{"item-" + std::to_string(i), i});
        if (!handle) {
            return std::unexpected(handle.error());
        }
    }
    return tx->commit();
}

} // namespace

int main() {
    TestSuite suite;

    // --- operadores isolados: project ∘ filter ∘ limit ---
    {
        int produced = 0;
        auto source = [&produced]() -> Generator<Result<int>> {
            for (int value = 0;; ++value) {
                ++produced;
                co_yield Result<int>{value};
            }
        };
        auto doubled = project<int, int>(
            limit(filter(source(), [](int value) { return value % 2 == 0; }), 3),
            [](int value) -> Result<int> { return value * 10; });
        std::vector<int> got;
        for (auto& item : doubled) {
            if (item) {
                got.push_back(*item);
            }
        }
        suite.check(got == std::vector<int>{0, 20, 40},
                    "project composes with filter and limit");
        suite.check(produced == 5, "project pipeline stays lazy (upstream stops at limit)");
    }

    TemporaryDatabase temp{"main"};
    auto created = Database::create(temp.path());
    auto database = share(created);
    suite.check(database != nullptr, "projection database is created");
    if (!database) {
        return suite.finish();
    }
    auto database_id = DatabaseRegistry::instance().attach(database);
    suite.check(database_id.has_value(), "projection database is attached");
    suite.check(database->bind(item_builder()).has_value(), "Item is bound");
    suite.check(seed(*database, 20).has_value(), "dataset is seeded");

    suite.check(database
                    ->register_computed<Item>("double_value",
                                              [](const Item& item) -> Result<AttributeValue> {
                                                  return AttributeValue{item.value * 2};
                                              })
                    .has_value(),
                "computed function is registered");

    // --- select: só name e value ---
    {
        std::size_t rows = 0;
        bool only_selected = true;
        for (auto& result :
             database->query<Item>().select({FieldId{1}, FieldId{2}}).limit(3).stream()) {
            if (!result) {
                only_selected = false;
                break;
            }
            only_selected = only_selected && result->fields.size() == 2 &&
                            result->get("name").has_value() && result->get("value").has_value();
            ++rows;
        }
        suite.check(rows == 3 && only_selected, "select yields only requested fields");
    }

    // --- compute registrado ---
    {
        auto first = database->query<Item>().where([](const Item& item) { return item.value == 7; })
                         .compute("double_value")
                         .stream();
        bool ok = false;
        for (auto& result : first) {
            if (!result) {
                break;
            }
            auto doubled = result->get("double_value");
            ok = doubled.has_value() && doubled->as_int64().has_value() &&
                 *doubled->as_int64() == 14;
        }
        suite.check(ok, "registered compute yields the derived attribute");
    }

    // --- select + compute + filter + limit ---
    {
        std::vector<std::int64_t> doubles;
        for (auto& result : database->query<Item>()
                                .where([](const Item& item) { return item.value % 2 == 0; })
                                .select({FieldId{2}})
                                .compute("double_value")
                                .limit(4)
                                .stream()) {
            if (!result) {
                break;
            }
            auto doubled = result->get("double_value");
            if (doubled && doubled->as_int64()) {
                doubles.push_back(*doubled->as_int64());
            }
        }
        suite.check(doubles == std::vector<std::int64_t>{0, 4, 8, 12},
                    "select+compute compose with where and limit");
    }

    // --- map tipado (transformação ad-hoc) ---
    {
        std::vector<std::string> labels;
        for (auto& result :
             database->query<Item>()
                 .limit(2)
                 .map<std::string>([](const Item& item) {
                     return item.name + "=" + std::to_string(item.value);
                 })
                 .stream()) {
            if (result) {
                labels.push_back(*result);
            }
        }
        suite.check(labels == std::vector<std::string>{"item-0=0", "item-1=1"},
                    "map transforms each element lazily");
    }

    // --- compute ausente falha limpo ---
    {
        bool saw_error = false;
        for (auto& result : database->query<Item>().compute("missing").limit(1).stream()) {
            saw_error = !result && result.error().code == ErrorCode::type_not_found;
        }
        suite.check(saw_error, "unknown computed function fails with type_not_found");
    }

    DatabaseRegistry::instance().detach(*database_id);
    return suite.finish();
}
