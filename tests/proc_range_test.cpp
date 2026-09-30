// Faixa e prefixo pelo índice nas procs (PLANO_PROPOSTAS_REGISTRY R4):
// `Context::range` e `Context::prefix`, numa proc de leitura (snapshot da
// chamada) e numa de escrita (a transação, com o que a própria chamada gravou).

#include "modb/app/server_connection.hpp"
#include "modb/server/host.hpp"
#include "modb/server/module.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace modb;
using modb::server::Context;
using modb::server::Mode;

namespace {

struct Termo {
    std::string texto;
    std::int64_t n{};
    std::string livre;  // sem índice
};

constexpr object::FieldId k_texto{1};
constexpr object::FieldId k_n{2};
constexpr object::FieldId k_livre{3};

object::BindingBuilder<Termo> termo_binding() {
    object::BindingBuilder<Termo> b{"Termo"};
    b.field<1>("texto", &Termo::texto).field<2>("n", &Termo::n).field<3>("livre", &Termo::livre);
    return b;
}

// Os textos dos ids, em ordem, para comparar sem depender da ordem do índice.
Result<ops::Value> textos(Context& c, const Result<std::vector<object::ObjectId>>& ids) {
    if (!ids) {
        return std::unexpected(ids.error());
    }
    std::vector<std::string> out;
    for (const auto id : *ids) {
        auto t = c.read<Termo>(id);
        if (!t) {
            return std::unexpected(t.error());
        }
        out.push_back(t->texto);
    }
    std::ranges::sort(out);
    ops::ValueList list;
    for (auto& s : out) {
        list.emplace_back(std::move(s));
    }
    return ops::Value{std::move(list)};
}

server::Module termos_module() {
    return server::ModuleBuilder{"termos"}
        .type(termo_binding())
        .index<Termo>(k_texto)
        .index<Termo>(k_n)
        .proc("termos.criar", Mode::read_write, "{texto, n}",
              [](Context& c, const ops::Args& a) -> Result<ops::Value> {
                  auto texto = a.text("texto");
                  auto n = a.integer_or("n", 0);
                  if (!texto || !n) {
                      return std::unexpected(!texto ? texto.error() : n.error());
                  }
                  auto id = c.create(Termo{*texto, *n, *texto});
                  if (!id) {
                      return std::unexpected(id.error());
                  }
                  return ops::Value::object({{"id", *id}});
              })
        .proc("termos.prefixo", Mode::read_only, "{prefixo}",
              [](Context& c, const ops::Args& a) -> Result<ops::Value> {
                  auto p = a.text("prefixo");
                  if (!p) {
                      return std::unexpected(p.error());
                  }
                  return textos(c, c.prefix<Termo>(k_texto, *p));
              })
        .proc("termos.faixa", Mode::read_only, "{de, ate}",
              [](Context& c, const ops::Args& a) -> Result<ops::Value> {
                  auto de = a.integer("de");
                  auto ate = a.integer("ate");
                  if (!de || !ate) {
                      return std::unexpected(!de ? de.error() : ate.error());
                  }
                  return textos(c, c.range<Termo>(k_n, object::AttributeValue{*de}, object::AttributeValue{*ate}));
              })
        .proc("termos.criar_e_prefixo", Mode::read_write, "{texto, prefixo}: cria e procura na mesma chamada",
              [](Context& c, const ops::Args& a) -> Result<ops::Value> {
                  auto texto = a.text("texto");
                  auto p = a.text("prefixo");
                  if (!texto || !p) {
                      return std::unexpected(!texto ? texto.error() : p.error());
                  }
                  if (auto id = c.create(Termo{*texto, 0, *texto}); !id) {
                      return std::unexpected(id.error());
                  }
                  return textos(c, c.prefix<Termo>(k_texto, *p));
              })
        .proc("termos.sem_indice", Mode::read_only, "prefixo num campo sem índice",
              [](Context& c, const ops::Args&) -> Result<ops::Value> {
                  return textos(c, c.prefix<Termo>(k_livre, "a"));
              })
        .proc("termos.sem_indice_escrita", Mode::read_write, "prefixo num campo sem índice, numa escrita",
              [](Context& c, const ops::Args&) -> Result<ops::Value> {
                  return textos(c, c.prefix<Termo>(k_livre, "a"));
              })
        .build();
}

Result<app::ServerConnection> conectar(std::uint16_t porta, const std::filesystem::path& db) {
    Error ultimo{ErrorCode::connection_closed, "sem tentativa"};
    for (int i = 0; i < 100; ++i) {
        auto conn = app::ServerConnection::connect(
            app::ConnectionOptions{.host = "127.0.0.1", .port = porta, .database_name = db.filename().string()});
        if (conn) {
            return conn;
        }
        ultimo = conn.error();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return std::unexpected(ultimo);
}

Result<ops::Value> chamar(app::ServerConnection& conn, std::string_view proc, const ops::Value& args) {
    auto bytes = conn.call(proc, ops::encode(args));
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return ops::decode(*bytes);
}

// A lista de textos de uma resposta.
std::vector<std::string> lista(const Result<ops::Value>& v) {
    std::vector<std::string> out;
    if (v && v->list()) {
        for (const auto& item : *v->list()) {
            out.push_back(*item.text());
        }
    }
    return out;
}

} // namespace

int main() {
    TestSuite suite;
    const auto db = std::filesystem::temp_directory_path() /
                    ("modb-proc-range-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     ".modb");
    const server::Module modulos[] = {termos_module()};
    auto srv = server::start(server::Options{.database = db, .host = "127.0.0.1", .port = 0, .log = "off"}, modulos);
    suite.check(srv.has_value(), "start com o módulo de termos");
    if (srv) {
        std::thread laco{[&] { (void)srv->serve_forever(); }};
        if (auto conn = conectar(srv->port(), db); !conn) {
            suite.check(false, "cliente conecta");
        } else {
            for (const auto& [texto, n] : std::vector<std::pair<std::string, std::int64_t>>{
                     {"lago", 1}, {"lago-sul", 2}, {"lagoa", 3}, {"laguna", 4}, {"lá", 5}, {"láctea", 6}, {"mar", 7}}) {
                suite.check(chamar(*conn, "termos.criar", ops::Value::object({{"texto", texto}, {"n", n}})).has_value(),
                            "cria " + texto);
            }
            suite.check(lista(chamar(*conn, "termos.prefixo", ops::Value::object({{"prefixo", "lago"}}))) ==
                            std::vector<std::string>{"lago", "lago-sul", "lagoa"},
                        "prefixo acha os que começam com o texto, e só esses");
            suite.check(lista(chamar(*conn, "termos.prefixo", ops::Value::object({{"prefixo", "lá"}}))) ==
                            std::vector<std::string>{"lá", "láctea"},
                        "prefixo com acento (bytes UTF-8)");
            suite.check(lista(chamar(*conn, "termos.prefixo", ops::Value::object({{"prefixo", "la"}}))) ==
                            std::vector<std::string>{"lago", "lago-sul", "lagoa", "laguna"},
                        "'la' não acha 'lá': a comparação é de bytes");
            suite.check(lista(chamar(*conn, "termos.prefixo", ops::Value::object({{"prefixo", ""}}))).size() == 7,
                        "prefixo vazio devolve todos");
            suite.check(lista(chamar(*conn, "termos.prefixo", ops::Value::object({{"prefixo", "rio"}}))).empty(),
                        "prefixo sem ocorrência devolve vazio");
            suite.check(lista(chamar(*conn, "termos.faixa", ops::Value::object({{"de", 2}, {"ate", 4}}))) ==
                            std::vector<std::string>{"lago-sul", "lagoa", "laguna"},
                        "faixa de inteiros inclui as duas pontas");
            suite.check(lista(chamar(*conn, "termos.criar_e_prefixo",
                                     ops::Value::object({{"texto", "marina"}, {"prefixo", "mar"}}))) ==
                            std::vector<std::string>{"mar", "marina"},
                        "numa escrita, o prefixo vê o que a própria chamada gravou");
            auto sem = chamar(*conn, "termos.sem_indice", ops::Value::object({}));
            suite.check(!sem && sem.error().code == ErrorCode::invalid_argument &&
                            sem.error().message.find("requires an index") != std::string::npos,
                        "campo sem índice é erro claro numa leitura, não uma varredura");
            auto sem_escrita = chamar(*conn, "termos.sem_indice_escrita", ops::Value::object({}));
            suite.check(!sem_escrita && sem_escrita.error().code == ErrorCode::invalid_argument,
                        "campo sem índice é erro claro numa escrita");
        }
        srv->request_stop();
        laco.join();
    }
    std::error_code ignored;
    std::filesystem::remove(db, ignored);
    std::filesystem::remove(db.string() + ".wal", ignored);
    return suite.finish();
}
