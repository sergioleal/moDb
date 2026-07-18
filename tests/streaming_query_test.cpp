// Valida a consulta em streaming da Fase 7A ponta a ponta: o critério TTFR
// (`limit 1` lê ≤ 2 páginas de dados num heap com muitas páginas), varredura
// completa e filtro corretos, cancelamento cooperativo que encerra o upstream,
// e estabilidade do snapshot mantido por toda a vida do fluxo.
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

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-stream-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
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

// Insere `count` itens (value = 0..count-1) em lotes transacionais.
Result<void> seed(Database& database, int count, int batch = 2'000) {
    for (int start = 0; start < count; start += batch) {
        auto tx = database.begin();
        if (!tx) {
            return std::unexpected(tx.error());
        }
        for (int i = start; i < start + batch && i < count; ++i) {
            auto handle = database.create(*tx, Item{"item-" + std::to_string(i), i});
            if (!handle) {
                return std::unexpected(handle.error());
            }
        }
        if (auto committed = tx->commit(); !committed) {
            return std::unexpected(committed.error());
        }
    }
    return {};
}

} // namespace

int main() {
    TestSuite suite;
    TemporaryDatabase temp{"main"};

    auto created = Database::create(temp.path());
    auto database = share(created);
    suite.check(database != nullptr, "streaming database is created");
    if (!database) {
        return suite.finish();
    }
    auto database_id = DatabaseRegistry::instance().attach(database);
    suite.check(database_id.has_value(), "streaming database is attached");
    suite.check(database->bind(item_builder()).has_value(), "Item is bound");

    // Volume literal do critério de aceite 7A. A consulta com limit(1) abaixo
    // deve continuar lendo no máximo duas páginas, independentemente das
    // milhares de páginas ocupadas pelo conjunto completo.
    constexpr int total = 100'000;
    suite.check(seed(*database, total).has_value(), "the dataset is seeded");

    // --- varredura completa: enumera todos e prova que há muitas páginas ---
    std::uint64_t full_pages = 0;
    {
        database->reset_data_pages_read();
        std::size_t count = 0;
        bool all_ok = true;
        for (auto& result : database->query<Item>().stream()) {
            if (!result) {
                all_ok = false;
                break;
            }
            ++count;
        }
        full_pages = database->data_pages_read();
        suite.check(all_ok && count == total, "a full scan streams every object exactly once");
        suite.check(full_pages > 2, "the dataset spans many data pages (naive scan reads them all)");
    }

    // --- TTFR (critério 7A): limit 1 lê ≤ 2 páginas de dados ---
    {
        database->reset_data_pages_read();
        std::size_t count = 0;
        bool first_ok = false;
        for (auto& result : database->query<Item>().limit(1).stream()) {
            first_ok = result.has_value();
            ++count;
        }
        const auto pages = database->data_pages_read();
        suite.check(count == 1, "limit 1 yields exactly one result");
        suite.check(first_ok, "the single result is Ok");
        suite.check(pages <= 2,
                    "limit 1 reads at most two data pages regardless of dataset size (TTFR)");
        suite.check(pages < full_pages, "limit reads far fewer pages than a full scan");
    }

    // --- filtro (operador Predicate): só os pares ---
    {
        std::size_t evens = 0;
        bool all_even = true;
        auto is_even = [](const Item& item) { return item.value % 2 == 0; };
        for (auto& result : database->query<Item>().where(is_even).stream()) {
            if (!result) {
                all_even = false;
                break;
            }
            if (result->value % 2 != 0) {
                all_even = false;
            }
            ++evens;
        }
        suite.check(all_even && evens == static_cast<std::size_t>(total / 2),
                    "filter keeps exactly the even-valued items");
    }

    // --- filtro + limite compõem e curto-circuitam ---
    {
        database->reset_data_pages_read();
        std::vector<std::int64_t> got;
        auto is_even = [](const Item& item) { return item.value % 2 == 0; };
        for (auto& result : database->query<Item>().where(is_even).limit(3).stream()) {
            if (result) {
                got.push_back(result->value);
            }
        }
        suite.check(got == std::vector<std::int64_t>{0, 2, 4},
                    "filter then limit yields the first three even values");
        suite.check(database->data_pages_read() < full_pages,
                    "filter+limit stops the upstream before scanning everything");
    }

    // --- cancelamento cooperativo: encerra o upstream cedo ---
    {
        database->reset_data_pages_read();
        query::CancellationToken token;
        std::size_t seen = 0;
        for (auto& result : database->query<Item>().cancel_on(token).stream()) {
            (void)result;
            if (++seen == 5) {
                token.cancel();
            }
        }
        suite.check(seen >= 5 && seen < static_cast<std::size_t>(total),
                    "a cancelled stream stops well before the end");
        suite.check(database->data_pages_read() < full_pages,
                    "cancellation stops the upstream before scanning everything");
    }

    // --- snapshot estável: o fluxo ignora commits feitos após abri-lo ---
    {
        // Abre a query (fixa a época) ANTES de mutar; consome DEPOIS.
        auto query = database->query<Item>();
        // Uma alteração e uma criação após a query aberta.
        {
            auto tx = database->begin();
            suite.check(tx.has_value(), "post-snapshot transaction begins");
            if (tx) {
                auto handle = database->get<Item>(ObjectId{first_user_object_id});
                if (handle) {
                    static_cast<void>(handle->set<&Item::value>(*tx, 999999));
                }
                static_cast<void>(database->create(*tx, Item{"item-late", 424242}));
                suite.check(tx->commit().has_value(), "post-snapshot mutation commits");
            }
        }
        std::size_t count = 0;
        bool saw_late = false;
        bool saw_mutated = false;
        for (auto& result : std::move(query).stream()) {
            if (!result) {
                break;
            }
            ++count;
            if (result->name == "item-late") {
                saw_late = true;
            }
            if (result->value == 999999) {
                saw_mutated = true;
            }
        }
        suite.check(count == total, "the stream sees exactly the snapshot's object count");
        suite.check(!saw_late, "the stream never sees an object created after the snapshot");
        suite.check(!saw_mutated, "the stream sees the pre-snapshot value, not the later update");
    }

    DatabaseRegistry::instance().detach(*database_id);
    return suite.finish();
}
