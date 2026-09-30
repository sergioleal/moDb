// Campo `ops::Value` num objeto persistido (PLANO_PROPOSTAS_REGISTRY R14): um
// membro com `bytes_codec` entra no binding como `bytes`, sobrevive à reabertura
// do banco e volta igual. O objeto inteiro precisa caber numa página do heap
// (8 KB com o MODB_PAGE_SIZE padrão): um Value maior falha com
// `record_too_large` e não deixa nada gravado.

#include "modb/object/database.hpp"
#include "modb/ops/value.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

using namespace modb;
using modb::ops::Value;
using modb::ops::ValueList;

namespace {

struct Agente {
    std::string nome;
    Value tags;       // lista de textos
    Value esquema;    // JSON livre (mapa aninhado)
};

object::BindingBuilder<Agente> agente_binding() {
    object::BindingBuilder<Agente> b{"Agente"};
    b.field<1>("nome", &Agente::nome).field<2>("tags", &Agente::tags).field<3>("esquema", &Agente::esquema);
    return b;
}

void apagar(const std::filesystem::path& db) {
    std::error_code ignored;
    std::filesystem::remove(db, ignored);
    std::filesystem::remove(db.string() + ".wal", ignored);
}

// O banco aberto, registrado no DatabaseRegistry enquanto existir (begin exige).
struct Aberto {
    std::shared_ptr<object::Database> db;
    object::DatabaseId attached;
    ~Aberto() { object::DatabaseRegistry::instance().detach(attached); }
};

Result<std::unique_ptr<Aberto>> abrir(const std::filesystem::path& path, bool criar) {
    auto opened = criar ? object::Database::create(path) : object::Database::open(path);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    auto database = std::make_shared<object::Database>(std::move(*opened));
    auto attached = object::DatabaseRegistry::instance().attach(database);
    if (!attached) {
        return std::unexpected(attached.error());
    }
    std::unique_ptr<Aberto> aberto{new Aberto{database, *attached}};
    if (auto bound = database->bind(agente_binding()); !bound) {
        return std::unexpected(bound.error());
    }
    return aberto;
}

} // namespace

int main() {
    TestSuite suite;
    static_assert(object::has_bytes_codec<Value>, "ops::Value tem bytes_codec");
    static_assert(object::attribute_type_of<Value>() == object::AttributeType::bytes, "ops::Value é guardado como bytes");

    const auto db = std::filesystem::temp_directory_path() /
                    ("modb-stored-value-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     ".modb");
    apagar(db);

    const Value tags = ValueList{Value{std::string{"clima"}}, Value{std::string{"ação"}}, Value{std::string{"mcp"}}};
    const Value esquema = Value::object({
        {"type", std::string{"object"}},
        {"required", ValueList{Value{std::string{"cidade"}}}},
        {"properties", Value::object({{"cidade", Value::object({{"type", std::string{"string"}}, {"max", std::int64_t{80}}})}})},
        {"aberto", true},
        {"peso", 0.5},
        {"vazio", Value{}},
    });

    object::ObjectId id{};
    {
        auto database = abrir(db, true);
        suite.check(database.has_value(), "cria o banco e faz o bind com campos Value");
        if (database) {
            auto tx = (*database)->db->begin();
            auto criado = tx ? (*database)->db->create(*tx, Agente{"previsão", tags, esquema})
                             : Result<object::Handle<Agente>>{std::unexpected(tx.error())};
            suite.check(criado.has_value() && tx->commit().has_value(), "grava um objeto com campos Value");
            if (criado) {
                id = criado->id();
            }

            // Um valor médio cabe na página.
            auto tx_medio = (*database)->db->begin();
            auto medio = tx_medio ? (*database)->db->create(*tx_medio, Agente{"médio", Value{std::string(4096, 'm')}, Value{}})
                                  : Result<object::Handle<Agente>>{std::unexpected(tx_medio.error())};
            suite.check(medio.has_value() && tx_medio->commit().has_value(), "um Value de 4 KB cabe no objeto");

            // Maior que a página: falha na escrita, e a transação desfeita não grava nada.
            auto tx2 = (*database)->db->begin();
            auto grande = tx2 ? (*database)->db->create(*tx2, Agente{"grande", Value{std::string(64 * 1024, 'x')}, Value{}})
                              : Result<object::Handle<Agente>>{std::unexpected(tx2.error())};
            suite.check(!grande && grande.error().code == ErrorCode::record_too_large,
                        "um objeto maior que a página é recusado com record_too_large");
            if (tx2) {
                (void)tx2->rollback();
            }
        }
    }

    {
        auto database = abrir(db, false);
        suite.check(database.has_value(), "reabre o banco");
        if (database && id.value != 0) {
            auto handle = (*database)->db->get<Agente>(id);
            auto lido = handle ? (*database)->db->materialize(*handle) : Result<Agente>{std::unexpected(handle.error())};
            suite.check(lido.has_value(), "lê o objeto depois de reabrir");
            if (lido) {
                suite.check(lido->nome == "previsão", "o campo de texto volta igual");
                suite.check(lido->tags == tags, "a lista volta igual (com acento)");
                suite.check(lido->esquema == esquema, "o mapa aninhado volta igual (tipos e null preservados)");
            }
            std::size_t grandes = 0;
            for (auto& row : (*database)->db->query<Agente>().stream()) {
                grandes += row && row->nome == "grande" ? 1 : 0;
            }
            suite.check(grandes == 0, "o objeto recusado não ficou gravado");
        }
    }

    apagar(db);
    return suite.finish();
}
