// PLANO_CONCORRENCIA C4.2: fumaça de concorrência -- várias threads só lendo.
//
// Um banco já confirmado e nenhum escritor: 8 threads fazem get, leitura sob
// snapshot, consulta por índice e varredura com limite, e conferem os valores.
// Hoje o caminho de leitura escreve em estado compartilhado sem lock (LRU do
// BufferPool, peek_type, planos de projeção, contadores): este teste é a linha
// de base que o ThreadSanitizer acusa e que a C6 tem de deixar limpa.
//
// Uso: modb_concurrency_smoke_tests [threads] [iterações por thread]

#include "modb/object/database.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace modb;
using namespace modb::object;

namespace {

struct Item {
    std::string name;
    std::int64_t value{};
    std::int64_t bucket{};
};

BindingBuilder<Item> item_builder() {
    BindingBuilder<Item> builder{"Item"};
    builder.field<1>("name", &Item::name).field<2>("value", &Item::value).field<3>("bucket", &Item::bucket);
    return builder;
}

constexpr int k_items = 2000;
constexpr std::int64_t k_buckets = 50;

} // namespace

int main(int argc, char** argv) {
    TestSuite suite;
    const int threads = argc > 1 ? std::atoi(argv[1]) : 8;
    const int iterations = argc > 2 ? std::atoi(argv[2]) : 300;

    const auto path = std::filesystem::temp_directory_path() /
                      ("modb-concurrency-smoke-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".modb");
    auto created = Database::create(path);
    if (!created) {
        suite.check(false, "cria o banco");
        return suite.finish();
    }
    auto db = std::make_shared<Database>(std::move(*created));
    auto dbid = DatabaseRegistry::instance().attach(db);
    suite.check(dbid && db->bind(item_builder()).has_value() && db->create_index<Item>(FieldId{3}).has_value(),
                "anexa, faz bind e cria o índice em bucket");

    std::vector<ObjectId> ids;
    {
        auto tx = db->begin();
        for (int i = 0; tx && i < k_items; ++i) {
            if (auto h = db->create(*tx, Item{"item " + std::to_string(i), i, i % k_buckets})) {
                ids.push_back(h->id());
            }
        }
        suite.check(tx && ids.size() == k_items && tx->commit().has_value(), "2000 itens confirmados");
    }

    std::atomic<int> wrong{0};
    std::atomic<int> errors{0};
    std::vector<std::thread> pool;
    const auto start = std::chrono::steady_clock::now();
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            std::mt19937 rng{static_cast<std::uint32_t>(1234 + t)};
            std::uniform_int_distribution<int> pick{0, k_items - 1};
            for (int i = 0; i < iterations; ++i) {
                const int n = pick(rng);
                switch (i % 4) {
                case 0: {  // leitura corrente
                    auto h = db->get<Item>(ids[static_cast<std::size_t>(n)]);
                    auto v = h ? db->materialize(*h) : Result<Item>{std::unexpected(h.error())};
                    if (!v) {
                        ++errors;
                    } else if (v->value != n) {
                        ++wrong;
                    }
                    break;
                }
                case 1: {  // leitura sob snapshot
                    auto snap = db->snapshot();
                    auto v = snap ? db->get<Item>(ids[static_cast<std::size_t>(n)], *snap)
                                  : Result<Item>{std::unexpected(snap.error())};
                    if (!v) {
                        ++errors;
                    } else if (v->value != n) {
                        ++wrong;
                    }
                    break;
                }
                case 2: {  // consulta pelo índice: 2000/50 = 40 por bucket
                    int count = 0;
                    const auto bucket = static_cast<std::int64_t>(n % k_buckets);
                    for (auto& item : db->query<Item>().equals(FieldId{3}, bucket).stream()) {
                        if (!item) {
                            ++errors;
                            break;
                        }
                        count += item->bucket == bucket ? 1 : 0;
                    }
                    if (count != k_items / k_buckets) {
                        ++wrong;
                    }
                    break;
                }
                default: {  // varredura com limite
                    int count = 0;
                    for (auto& item : db->query<Item>().limit(25).stream()) {
                        if (!item) {
                            ++errors;
                            break;
                        }
                        ++count;
                    }
                    if (count != 25) {
                        ++wrong;
                    }
                    break;
                }
                }
            }
        });
    }
    for (auto& th : pool) {
        th.join();
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    suite.check(errors.load() == 0, "nenhum erro de leitura (" + std::to_string(errors.load()) + ")");
    suite.check(wrong.load() == 0, "nenhum valor errado (" + std::to_string(wrong.load()) + ")");
    std::cout << threads << " threads x " << iterations << " leituras em " << ms << " ms\n";

    if (dbid) {
        DatabaseRegistry::instance().detach(*dbid);
    }
    db.reset();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);
    return suite.finish();
}
