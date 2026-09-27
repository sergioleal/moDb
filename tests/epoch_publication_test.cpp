// PLANO_CONCORRENCIA C1: um snapshot só vê transações publicadas.
//
// O commit avança a época no começo (ela vai na imagem do DBRT, dentro do
// WAL), mas uma transação só pode ficar visível depois de durável e aplicada.
// Um snapshot aberto nesse intervalo -- aqui, depois de um commit parado num
// failpoint -- não pode enxergar a transação.

#include "modb/object/database.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

using namespace modb;
using namespace modb::object;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-epoch-pub-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
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

struct Aberto {
    std::shared_ptr<Database> db;
    DatabaseId id{};
    ~Aberto() {
        if (id.value != 0) {
            DatabaseRegistry::instance().detach(id);
        }
    }
};

bool abrir(Aberto& a, const std::filesystem::path& path) {
    auto created = Database::create(path);
    if (!created) {
        return false;
    }
    a.db = std::make_shared<Database>(std::move(*created));
    auto id = DatabaseRegistry::instance().attach(a.db);
    if (!id) {
        return false;
    }
    a.id = *id;
    return a.db->bind(doc_builder()).has_value();
}

} // namespace

int main() {
    TestSuite suite;

    for (const auto phase : {CommitPhase::stop_after_images, CommitPhase::stop_after_commit_record}) {
        const std::string nome = phase == CommitPhase::stop_after_images ? "imagens no WAL, sem commit"
                                                                         : "commit no WAL, sem aplicar";
        TemporaryDatabase temp{phase == CommitPhase::stop_after_images ? "images" : "record"};
        Aberto a;
        if (!abrir(a, temp.path())) {
            suite.check(false, "abre o banco");
            return suite.finish();
        }
        auto& db = *a.db;

        ObjectId antigo;
        {
            auto tx = db.begin();
            auto h = tx ? db.create(*tx, Doc{"antigo", 1}) : Result<Handle<Doc>>{std::unexpected(tx.error())};
            suite.check(h && tx->commit().has_value(), "[" + nome + "] commit normal de um objeto");
            if (h) {
                antigo = h->id();
            }
        }
        const auto publicada = db.epoch();

        ObjectId novo;
        {
            auto tx = db.begin();
            if (!tx) {
                suite.check(false, "begin");
                return suite.finish();
            }
            auto h = db.create(*tx, Doc{"novo", 1});
            auto velho = db.get<Doc>(antigo);
            suite.check(h && velho && db.update(*tx, *velho, Doc{"antigo", 2}).has_value(),
                        "[" + nome + "] a transação cria um objeto e muda outro");
            if (h) {
                novo = h->id();
            }
            suite.check(tx->commit(phase).has_value(), "[" + nome + "] o commit para no failpoint");

            // O que um leitor que chega agora enxerga.
            suite.check(db.epoch() == publicada, "[" + nome + "] a época publicada não avançou");
            auto snap = db.snapshot();
            suite.check(snap.has_value(), "[" + nome + "] abre snapshot no meio do commit");
            if (snap) {
                suite.check(snap->epoch() == publicada, "[" + nome + "] o snapshot fica na época publicada");
                auto visto_novo = db.get<Doc>(novo, *snap);
                suite.check(!visto_novo, "[" + nome + "] o snapshot não vê o objeto criado pela transação");
                auto visto_antigo = db.get<Doc>(antigo, *snap);
                suite.check(visto_antigo && visto_antigo->rev == 1,
                            "[" + nome + "] o snapshot vê a versão confirmada do objeto mudado");
            }
        }
    }

    // Commit completo: aí sim a época publicada avança e um snapshot novo vê.
    {
        TemporaryDatabase temp{"full"};
        Aberto a;
        if (!abrir(a, temp.path())) {
            suite.check(false, "abre o banco (full)");
            return suite.finish();
        }
        auto& db = *a.db;
        const auto antes = db.epoch();
        auto snap_antes = db.snapshot();
        ObjectId id;
        {
            auto tx = db.begin();
            auto h = tx ? db.create(*tx, Doc{"x", 1}) : Result<Handle<Doc>>{std::unexpected(tx.error())};
            suite.check(h && tx->commit().has_value(), "commit completo");
            if (h) {
                id = h->id();
            }
        }
        suite.check(db.epoch() == antes + 1, "commit completo publica a época seguinte");
        auto snap = db.snapshot();
        suite.check(snap && db.get<Doc>(id, *snap).has_value(), "snapshot depois do commit vê o objeto");
        suite.check(snap_antes && !db.get<Doc>(id, *snap_antes), "snapshot de antes continua sem ver");
        // Rollback não publica nada.
        {
            auto tx = db.begin();
            if (tx) {
                (void)db.create(*tx, Doc{"descartado", 1});
                (void)tx->rollback();
            }
        }
        suite.check(db.epoch() == antes + 1, "rollback não mexe na época publicada");
    }

    return suite.finish();
}
