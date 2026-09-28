// PLANO_CONCORRENCIA C10: o contrato de threads do Database.
//
//   - várias threads escrevendo: um escritor por vez, `begin` espera a vez; um
//     leia-modifique-grave dentro da transação nunca perde atualização;
//   - na mesma thread, uma segunda transação é `transaction_active`;
//   - leitores concorrentes veem o contador só crescer, e cada snapshot vê um
//     valor confirmado;
//   - DDL (create_index) espera a transação de outra thread terminar;
//   - uma thread pode abrir transação no meio de um stream que ela consome.

#include "modb/object/database.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace modb;
using namespace modb::object;

namespace {

struct Counter {
    std::string name;
    std::int64_t value{};
};

BindingBuilder<Counter> counter_builder() {
    BindingBuilder<Counter> builder{"Counter"};
    builder.field<1>("name", &Counter::name).field<2>("value", &Counter::value);
    return builder;
}

} // namespace

int main() {
    TestSuite suite;
    const auto path = std::filesystem::temp_directory_path() /
                      ("modb-concurrent-writers-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".modb");
    auto created = Database::create(path);
    if (!created) {
        suite.check(false, "cria o banco");
        return suite.finish();
    }
    auto db = std::make_shared<Database>(std::move(*created));
    auto dbid = DatabaseRegistry::instance().attach(db);
    suite.check(dbid && db->bind(counter_builder()).has_value(), "anexa e faz bind");

    ObjectId counter;
    {
        auto tx = db->begin();
        auto h = tx ? db->create(*tx, Counter{"c", 0}) : Result<Handle<Counter>>{std::unexpected(tx.error())};
        suite.check(h && tx->commit().has_value(), "cria o contador");
        if (h) {
            counter = h->id();
        }
    }

    // --- mesma thread: segunda transação é recusada, não trava ---
    {
        auto first = db->begin();
        auto second = db->begin();
        suite.check(first && !second && second.error().code == ErrorCode::transaction_active,
                    "segunda transação na mesma thread é transaction_active");
        if (first) {
            (void)first->rollback();
        }
    }

    // --- 4 escritores x 150 incrementos, com leitores conferindo ---
    constexpr int k_writers = 4;
    constexpr int k_increments = 150;
    std::atomic<int> write_errors{0};
    std::atomic<bool> writing{true};
    std::atomic<int> went_back{0}, read_errors{0};
    std::atomic<std::uint64_t> reads{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            std::int64_t last = 0;
            while (writing.load()) {
                // O padrão recomendado (ADR-027): lock de leitura antes do
                // snapshot, pelo tempo da leitura. Um snapshot aberto enquanto
                // a thread espera o lock atrás de escritores fica mais velho que
                // o último commit e faz as escritas do objeto conflitarem.
                const auto read_lock = db->read_guard();
                auto snap = db->snapshot();
                auto v = snap ? db->get<Counter>(counter, *snap) : Result<Counter>{std::unexpected(snap.error())};
                if (!v) {
                    ++read_errors;
                    continue;
                }
                if (v->value < last) {
                    ++went_back;
                }
                last = v->value;
                ++reads;
            }
        });
    }
    std::vector<std::thread> writers;
    for (int w = 0; w < k_writers; ++w) {
        writers.emplace_back([&] {
            for (int i = 0; i < k_increments; ++i) {
                // `transact` espera a vez (begin) e repete em snapshot_conflict:
                // os leitores seguram snapshots da versão anterior o tempo todo.
                auto done = db->transact([&](Transaction& tx) -> Result<void> {
                    auto h = db->get<Counter>(counter);
                    if (!h) {
                        return std::unexpected(h.error());
                    }
                    auto v = db->materialize(*h);
                    if (!v) {
                        return std::unexpected(v.error());
                    }
                    return db->update(tx, *h, Counter{"c", v->value + 1});
                });
                if (!done) {
                    ++write_errors;
                    std::cerr << "  incremento: " << done.error().message << '\n';
                }
            }
        });
    }
    for (auto& t : writers) {
        t.join();
    }
    writing.store(false);
    for (auto& t : readers) {
        t.join();
    }
    auto final_value = db->get<Counter>(counter);
    auto fv = final_value ? db->materialize(*final_value) : Result<Counter>{std::unexpected(final_value.error())};
    suite.check(write_errors.load() == 0, "nenhum incremento falhou (" + std::to_string(write_errors.load()) + ")");
    suite.check(fv && fv->value == k_writers * k_increments,
                "nenhuma atualização perdida: " + std::to_string(fv ? fv->value : -1) + " de " +
                    std::to_string(k_writers * k_increments));
    suite.check(read_errors.load() == 0 && reads.load() > 0, "leitores leram durante as escritas");
    suite.check(went_back.load() == 0, "nenhum leitor viu o contador voltar");

    // --- DDL espera a transação de outra thread ---
    {
        std::atomic<bool> index_done{false};
        auto tx = db->begin();
        std::thread ddl{[&] {
            (void)db->create_index<Counter>(FieldId{1});
            index_done.store(true);
        }};
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        suite.check(!index_done.load(), "create_index espera a transação aberta em outra thread");
        if (tx) {
            (void)tx->commit();
        }
        ddl.join();
        suite.check(index_done.load() && db->has_index_for<Counter>(FieldId{1}),
                    "e roda depois que ela termina");
    }

    // --- transação no meio de um stream consumido pela mesma thread ---
    {
        int seen = 0;
        bool wrote = false;
        for (auto& item : db->query<Counter>().stream()) {
            if (!item) {
                break;
            }
            ++seen;
            auto tx = db->begin();  // o stream não segura o lock entre itens
            if (tx) {
                auto created = db->create(*tx, Counter{"extra", 1});
                wrote = created && tx->commit().has_value();
            }
        }
        suite.check(seen == 1 && wrote, "abre transação no meio de um stream sem travar");
    }

    if (dbid) {
        DatabaseRegistry::instance().detach(*dbid);
    }
    db.reset();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);
    return suite.finish();
}
