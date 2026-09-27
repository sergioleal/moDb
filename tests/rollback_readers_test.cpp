// PLANO_CONCORRENCIA C3: um rollback não pode mudar o que um leitor aberto vê.
//
// O rollback relia o ObjectStore do disco (resync_store_after_rollback). O
// disco só tem a versão corrente de cada objeto; as versões `previous` que um
// snapshot mais antigo ainda precisa moram em memória -- e sumiam. Vale com
// uma thread só: snapshot aberto, um commit muda um objeto, outra transação é
// desfeita, e o snapshot deixava de ver (ou via errado) o objeto.

#include "modb/object/database.hpp"
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
                ("modb-rollback-readers-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + ".wal", ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

struct Doc {
    std::string title;
    std::int64_t rev{};
};

BindingBuilder<Doc> doc_builder() {
    BindingBuilder<Doc> builder{"Doc"};
    builder.field<1>("title", &Doc::title).field<2>("rev", &Doc::rev);
    return builder;
}

} // namespace

int main() {
    TestSuite suite;
    TemporaryDatabase temp{"a"};
    auto created = Database::create(temp.path());
    if (!created) {
        suite.check(false, "cria o banco");
        return suite.finish();
    }
    auto db = std::make_shared<Database>(std::move(*created));
    auto dbid = DatabaseRegistry::instance().attach(db);
    suite.check(dbid && db->bind(doc_builder()).has_value(), "anexa e faz bind");

    // Dez objetos confirmados, rev 1.
    std::vector<ObjectId> ids;
    {
        auto tx = db->begin();
        for (int i = 0; i < 10 && tx; ++i) {
            if (auto h = db->create(*tx, Doc{"doc " + std::to_string(i), 1})) {
                ids.push_back(h->id());
            }
        }
        suite.check(tx && ids.size() == 10 && tx->commit().has_value(), "dez objetos confirmados");
    }

    auto antigo = db->snapshot();
    suite.check(antigo.has_value(), "snapshot aberto antes das mudanças");
    // A consulta fixa o próprio snapshot ao ser criada: este stream é do estado antigo.
    auto gen = db->query<Doc>().stream();

    // Commit que muda o primeiro objeto (rev 2): o snapshot antigo ainda vê rev 1.
    {
        auto tx = db->begin();
        auto h = db->get<Doc>(ids[0]);
        suite.check(tx && h && db->update(*tx, *h, Doc{"doc 0", 2}).has_value() && tx->commit().has_value(),
                    "commit muda o primeiro objeto");
    }
    auto antes = antigo ? db->get<Doc>(ids[0], *antigo) : Result<Doc>{std::unexpected(antigo.error())};
    suite.check(antes && antes->rev == 1, "antes do rollback, o snapshot antigo vê rev 1");

    // O stream do estado antigo, consumido pela metade.
    std::int64_t soma_revs = 0;
    int vistos = 0;
    {
        auto it = gen.begin();
        for (; it != gen.end() && vistos < 5; ++it, ++vistos) {
            if (*it) {
                soma_revs += (*it)->rev;
            }
        }

        // Outra transação escreve e é desfeita no meio do stream.
        {
            auto tx = db->begin();
            if (tx) {
                auto h = db->get<Doc>(ids[1]);
                if (h) {
                    (void)db->update(*tx, *h, Doc{"doc 1", 99});
                }
                (void)db->create(*tx, Doc{"descartado", 99});
                suite.check(tx->rollback().has_value(), "transação desfeita no meio do stream");
            }
        }

        for (; it != gen.end(); ++it, ++vistos) {
            if (*it) {
                soma_revs += (*it)->rev;
            }
        }
    }
    suite.check(vistos == 10 && soma_revs == 10,
                "o stream no snapshot antigo termina com os dez objetos em rev 1 (viu " + std::to_string(vistos) +
                    ", soma " + std::to_string(soma_revs) + ")");

    auto depois = antigo ? db->get<Doc>(ids[0], *antigo) : Result<Doc>{std::unexpected(antigo.error())};
    suite.check(depois && depois->rev == 1, "depois do rollback, o snapshot antigo continua vendo rev 1");
    auto atual = db->get<Doc>(ids[0]);
    suite.check(atual && db->materialize(*atual) && db->materialize(*atual)->rev == 2,
                "a leitura corrente vê rev 2");
    auto nao_desfeito = db->get<Doc>(ids[1]);
    suite.check(nao_desfeito && db->materialize(*nao_desfeito)->rev == 1, "a mudança desfeita não ficou");

    // E o banco segue escrevendo normalmente depois.
    {
        auto tx = db->begin();
        auto h = tx ? db->create(*tx, Doc{"depois", 1}) : Result<Handle<Doc>>{std::unexpected(tx.error())};
        suite.check(h && tx->commit().has_value(), "commit depois do rollback");
    }

    if (dbid) {
        DatabaseRegistry::instance().detach(*dbid);
    }
    return suite.finish();
}
