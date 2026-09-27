// PLANO_CONCORRENCIA C2: consulta por índice sob um snapshot antigo.
//
// O índice reflete o estado corrente: mudar a chave de um objeto (ou removê-lo)
// tira a chave antiga. Uma consulta cujo snapshot é de antes da mudança ainda
// deve achar o objeto pela chave antiga -- e não achá-lo pela nova.

#include "modb/object/database.hpp"
#include "test_support.hpp"

#include <algorithm>
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
                ("modb-index-snapshot-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
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

struct User {
    std::string name;
    std::int64_t age{};
};

BindingBuilder<User> user_builder() {
    BindingBuilder<User> builder{"User"};
    builder.field<1>("name", &User::name).field<2>("age", &User::age);
    return builder;
}

template <typename Q>
std::vector<std::string> names(Q query) {
    std::vector<std::string> out;
    for (auto& item : std::move(query).stream()) {
        if (item) {
            out.push_back(item->name);
        } else {
            out.push_back("ERRO: " + item.error().message);
        }
    }
    return out;
}

Result<void> commit_update(Database& db, ObjectId id, User value) {
    auto tx = db.begin();
    if (!tx) {
        return std::unexpected(tx.error());
    }
    auto h = db.get<User>(id);
    if (!h) {
        return std::unexpected(h.error());
    }
    if (auto ok = db.update(*tx, *h, value); !ok) {
        return ok;
    }
    return tx->commit();
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
    suite.check(dbid && db->bind(user_builder()).has_value(), "anexa e faz bind");
    suite.check(db->create_index<User>(FieldId{1}).has_value(), "índice em name");
    suite.check(db->create_index<User>(FieldId{2}).has_value(), "índice em age");

    ObjectId ana, caio;
    {
        auto tx = db->begin();
        auto a = tx ? db->create(*tx, User{"ana", 30}) : Result<Handle<User>>{std::unexpected(tx.error())};
        auto c = tx ? db->create(*tx, User{"caio", 40}) : Result<Handle<User>>{std::unexpected(tx.error())};
        suite.check(a && c && tx->commit().has_value(), "cria ana e caio");
        if (a && c) {
            ana = a->id();
            caio = c->id();
        }
    }

    // Consultas montadas agora ficam com o snapshot de agora.
    auto pela_antiga = db->query<User>().equals(FieldId{1}, std::string{"ana"});
    auto pela_nova = db->query<User>().equals(FieldId{1}, std::string{"bia"});
    auto faixa = db->query<User>().between(FieldId{2}, std::int64_t{25}, std::int64_t{35});
    auto removido = db->query<User>().equals(FieldId{1}, std::string{"caio"});

    // Depois do snapshot: ana vira bia com 50 anos; caio é removido.
    suite.check(commit_update(*db, ana, User{"bia", 50}).has_value(), "renomeia ana para bia (e muda a idade)");
    {
        auto tx = db->begin();
        suite.check(tx && db->remove(*tx, caio).has_value() && tx->commit().has_value(), "remove caio");
    }

    suite.check(names(std::move(pela_antiga)) == std::vector<std::string>{"ana"},
                "o snapshot antigo acha o objeto pela chave antiga");
    suite.check(names(std::move(pela_nova)).empty(), "e não o acha pela chave nova");
    suite.check(names(std::move(faixa)) == std::vector<std::string>{"ana"},
                "faixa de idade no snapshot antigo acha a idade antiga");
    suite.check(names(std::move(removido)) == std::vector<std::string>{"caio"},
                "o snapshot antigo ainda acha o objeto removido depois");

    // Consultas novas veem o estado novo.
    suite.check(names(db->query<User>().equals(FieldId{1}, std::string{"ana"})).empty(),
                "consulta nova não acha a chave antiga");
    suite.check(names(db->query<User>().equals(FieldId{1}, std::string{"bia"})) == std::vector<std::string>{"bia"},
                "consulta nova acha a chave nova");
    suite.check(names(db->query<User>().equals(FieldId{1}, std::string{"caio"})).empty(),
                "consulta nova não acha o removido");

    // Ordem da faixa: a do índice (valor, depois id), com ou sem o caminho alternativo.
    {
        auto tx = db->begin();
        if (tx) {
            for (const auto& [n, age] : {std::pair{"d", 60}, std::pair{"e", 55}, std::pair{"f", 65}}) {
                (void)db->create(*tx, User{n, age});
            }
            suite.check(tx->commit().has_value(), "mais três usuários");
        }
    }
    auto ordenada_antes = db->query<User>().between(FieldId{2}, std::int64_t{50}, std::int64_t{70});
    const auto esperado = std::vector<std::string>{"bia", "e", "d", "f"};
    suite.check(names(db->query<User>().between(FieldId{2}, std::int64_t{50}, std::int64_t{70})) == esperado,
                "faixa em ordem de valor pelo índice");
    suite.check(commit_update(*db, ana, User{"bia", 58}).has_value(), "muda a idade de bia");
    suite.check(names(std::move(ordenada_antes)) == esperado,
                "faixa sob snapshot antigo mantém a ordem de valor (idades da época)");

    if (dbid) {
        DatabaseRegistry::instance().detach(*dbid);
    }
    return suite.finish();
}
