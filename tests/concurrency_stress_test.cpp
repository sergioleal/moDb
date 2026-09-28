// PLANO_CONCORRENCIA C5: estresse com N leitores e 1 escritor, com invariantes.
//
// O escritor roda transações que criam, mudam e removem objetos; parte delas é
// desfeita de propósito depois de escrever um valor "veneno" (negativo). A cada
// commit ele anota, para a época publicada, quantos objetos existem e a soma
// dos valores. Os leitores abrem snapshots, varrem tudo e conferem:
//
//   - o estado visto é exatamente um estado confirmado (contagem e soma iguais
//     às anotadas para a época do snapshot);
//   - nenhum valor veneno aparece (nada de transação desfeita);
//   - nenhuma leitura falha.
//
// Modos:
//   serial      um mutex global em volta de cada transação e de cada leitura.
//               Valida o próprio teste; tem de passar hoje, em qualquer preset.
//   concurrent  sem lock nenhum. É o alvo das C6-C8: até lá o motor não é
//               seguro entre threads, e este modo só roda no preset tsan.
//
// Uso: modb_concurrency_stress_tests <serial|concurrent> [segundos] [leitores] [semente]

#include "modb/object/database.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace modb;
using namespace modb::object;

namespace {

struct Doc {
    std::int64_t value{};
    std::string pad;
};

BindingBuilder<Doc> doc_builder() {
    BindingBuilder<Doc> builder{"Doc"};
    builder.field<1>("value", &Doc::value).field<2>("pad", &Doc::pad);
    return builder;
}

constexpr std::int64_t k_poison = -1;

struct State {
    std::int64_t count{0};
    std::int64_t sum{0};
};

// Estados confirmados, por época publicada. Escrito pelo escritor, lido pelos leitores.
class Ledger {
public:
    void record(std::uint64_t epoch, State s) {
        const std::scoped_lock lock{mu_};
        states_[epoch] = s;
    }
    // Espera o escritor anotar a época (ele anota logo depois do commit voltar).
    std::optional<State> wait_for(std::uint64_t epoch, const std::atomic<bool>& writer_done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                const std::scoped_lock lock{mu_};
                if (const auto it = states_.find(epoch); it != states_.end()) {
                    return it->second;
                }
            }
            if (writer_done.load()) {
                const std::scoped_lock lock{mu_};
                const auto it = states_.find(epoch);
                return it == states_.end() ? std::nullopt : std::optional{it->second};
            }
            std::this_thread::yield();
        }
        return std::nullopt;
    }

private:
    std::mutex mu_;
    std::map<std::uint64_t, State> states_;
};

} // namespace

int main(int argc, char** argv) {
    TestSuite suite;
    const std::string_view mode = argc > 1 ? argv[1] : "serial";
    const bool serial = mode == "serial";
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 2;
    const int readers = argc > 3 ? std::atoi(argv[3]) : 4;
    const std::uint32_t seed = argc > 4 ? static_cast<std::uint32_t>(std::atoi(argv[4])) : 20260927u;
    std::cout << "modo " << mode << ", " << seconds << " s, " << readers << " leitores, semente " << seed << '\n';

    const auto path = std::filesystem::temp_directory_path() /
                      ("modb-concurrency-stress-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".modb");
    auto created = Database::create(path);
    if (!created) {
        suite.check(false, "cria o banco");
        return suite.finish();
    }
    auto db = std::make_shared<Database>(std::move(*created));
    auto dbid = DatabaseRegistry::instance().attach(db);
    suite.check(dbid && db->bind(doc_builder()).has_value(), "anexa e faz bind");

    // Estado inicial: 200 objetos confirmados.
    std::vector<std::pair<ObjectId, std::int64_t>> live;  // modelo do escritor
    State model;
    {
        auto tx = db->begin();
        for (int i = 0; tx && i < 200; ++i) {
            if (auto h = db->create(*tx, Doc{i, std::string(64, 'x')})) {
                live.emplace_back(h->id(), i);
                model.count += 1;
                model.sum += i;
            }
        }
        suite.check(tx && tx->commit().has_value(), "estado inicial confirmado");
    }
    Ledger ledger;
    ledger.record(db->epoch(), model);

    std::mutex engine;  // só no modo serial
    std::atomic<bool> writer_done{false};
    std::atomic<std::uint64_t> commits{0}, rollbacks{0}, conflicts{0}, reads{0};
    std::atomic<int> read_errors{0}, poison_seen{0}, mismatches{0}, missing_epochs{0};
    const auto stop_at = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

    // Onde cada thread está, para o vigia contar se o teste travar.
    // 0 fora, 1 esperando begin, 2 na transação, 3 commit, 4 rollback,
    // 11 snapshot, 12 scan, 13 esperando a época no ledger.
    std::atomic<int> writer_phase{0};
    std::vector<std::atomic<int>> reader_phase(static_cast<std::size_t>(readers));
    std::atomic<bool> all_done{false};
    std::thread watchdog{[&] {
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(seconds + 90);
        while (!all_done.load()) {
            if (std::chrono::steady_clock::now() > limit) {
                std::cerr << "VIGIA: travado. escritor fase " << writer_phase.load() << ", commits "
                          << commits.load() << ", leituras " << reads.load() << ", writer_done "
                          << writer_done.load() << "; leitores:";
                for (auto& ph : reader_phase) {
                    std::cerr << ' ' << ph.load();
                }
                std::cerr << std::endl;
                std::abort();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }};

    std::thread writer{[&] {
        std::mt19937 rng{seed};
        std::uniform_int_distribution<int> percent{0, 99};
        while (std::chrono::steady_clock::now() < stop_at) {
            std::unique_lock<std::mutex> lock{engine, std::defer_lock};
            if (serial) {
                lock.lock();
            }
            writer_phase.store(1);
            auto tx = db->begin();
            writer_phase.store(2);
            if (!tx) {
                continue;
            }
            const bool abort = percent(rng) < 20;
            auto next_live = live;
            State next = model;
            bool ok = true;
            const int ops = 1 + percent(rng) % 5;
            for (int k = 0; k < ops && ok; ++k) {
                const int op = percent(rng);
                if (op < 40 || next_live.size() < 20) {  // cria
                    const std::int64_t v = abort ? k_poison : percent(rng) + 1;
                    auto h = db->create(*tx, Doc{v, std::string(64, 'c')});
                    ok = h.has_value();
                    if (ok) {
                        next_live.emplace_back(h->id(), v);
                        next.count += 1;
                        next.sum += v;
                    }
                } else if (op < 80) {  // muda
                    auto& [id, old] = next_live[static_cast<std::size_t>(percent(rng)) % next_live.size()];
                    const std::int64_t v = abort ? k_poison : percent(rng) + 1;
                    auto h = db->get<Doc>(id);
                    ok = h && db->update(*tx, *h, Doc{v, std::string(64, 'u')}).has_value();
                    if (ok) {
                        next.sum += v - old;
                        old = v;
                    }
                } else {  // remove
                    const auto i = static_cast<std::size_t>(percent(rng)) % next_live.size();
                    ok = db->remove(*tx, next_live[i].first).has_value();
                    if (ok) {
                        next.count -= 1;
                        next.sum -= next_live[i].second;
                        next_live.erase(next_live.begin() + static_cast<std::ptrdiff_t>(i));
                    }
                }
            }
            if (!ok) {
                writer_phase.store(4);
                // Conflito com um snapshot aberto (a versão anterior ainda é
                // necessária): desiste da transação, o modelo não muda.
                (void)tx->rollback();
                ++conflicts;
                continue;
            }
            if (abort) {
                (void)tx->rollback();
                ++rollbacks;
                continue;
            }
            writer_phase.store(3);
            if (!tx->commit()) {
                ++conflicts;
                continue;
            }
            live = std::move(next_live);
            model = next;
            ledger.record(db->epoch(), model);
            ++commits;
        }
        writer_phase.store(0);
        writer_done.store(true);
    }};

    std::vector<std::thread> pool;
    for (int r = 0; r < readers; ++r) {
        pool.emplace_back([&, r] {
            auto& phase = reader_phase[static_cast<std::size_t>(r)];
            while (!writer_done.load()) {
                std::unique_lock<std::mutex> lock{engine, std::defer_lock};
                if (serial) {
                    lock.lock();
                }
                phase.store(11);
                auto snap = db->snapshot();
                if (!snap) {
                    ++read_errors;
                    continue;
                }
                State seen;
                bool poison = false;
                phase.store(12);
                auto scanned = db->scan<Doc>(*snap, [&](const Doc& d) -> Result<void> {
                    seen.count += 1;
                    seen.sum += d.value;
                    poison = poison || d.value == k_poison;
                    return {};
                });
                const auto epoch = snap->epoch();
                snap = std::unexpected(Error{ErrorCode::invalid_argument, "fechado"});  // solta o snapshot
                if (lock.owns_lock()) {
                    lock.unlock();
                }
                ++reads;
                if (!scanned) {
                    ++read_errors;
                    continue;
                }
                if (poison) {
                    ++poison_seen;
                }
                phase.store(13);
                auto expected = ledger.wait_for(epoch, writer_done);
                phase.store(0);
                if (!expected) {
                    ++missing_epochs;
                } else if (expected->count != seen.count || expected->sum != seen.sum) {
                    ++mismatches;
                }
            }
        });
    }
    writer.join();
    for (auto& t : pool) {
        t.join();
    }
    all_done.store(true);
    watchdog.join();

    std::cout << commits.load() << " commits, " << rollbacks.load() << " rollbacks, " << conflicts.load()
              << " conflitos, " << reads.load() << " leituras de snapshot\n";
    suite.check(commits.load() > 0 && reads.load() > 0, "houve escrita e leitura");
    suite.check(read_errors.load() == 0, "nenhuma leitura falhou (" + std::to_string(read_errors.load()) + ")");
    suite.check(poison_seen.load() == 0,
                "nenhum leitor viu valor de transação desfeita (" + std::to_string(poison_seen.load()) + ")");
    suite.check(missing_epochs.load() == 0,
                "todo snapshot caiu numa época confirmada (" + std::to_string(missing_epochs.load()) + " fora)");
    suite.check(mismatches.load() == 0,
                "cada snapshot viu exatamente o estado da sua época (" + std::to_string(mismatches.load()) +
                    " diferentes)");

    if (dbid) {
        DatabaseRegistry::instance().detach(*dbid);
    }
    db.reset();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);
    return suite.finish();
}
