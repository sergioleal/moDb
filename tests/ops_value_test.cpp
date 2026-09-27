// ops::Value: codificação binária, JSON e leitura tipada de argumentos de proc
// (PLANO_SERVIDOR_PROCS S2).

#include "modb/ops/value.hpp"

#include "test_support.hpp"

#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace modb;
using ops::Value;

namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> xs) {
    std::vector<std::byte> out;
    for (auto x : xs) {
        out.push_back(static_cast<std::byte>(x));
    }
    return out;
}

} // namespace

int main() {
    TestSuite suite;

    const Value todos = Value::object({
        {"nulo", nullptr},
        {"sim", true},
        {"nao", false},
        {"inteiro", std::int64_t{-42}},
        {"grande", std::numeric_limits<std::int64_t>::max()},
        {"real", 3.25},
        {"real_inteiro", 2.0},
        {"texto", "Grande Sertão: Veredas \xf0\x9f\x93\x9a \"aspas\" \\ \n"},
        {"vazio", ""},
        {"id", object::ObjectId{12345}},
        {"lista", ops::ValueList{1, "dois", ops::ValueList{}, Value::object({{"x", 1}})}},
        {"mapa_vazio", ops::ValueMap{}},
    });

    // --- binário ---
    {
        const auto codificado = ops::encode(todos);
        auto volta = ops::decode(codificado);
        suite.check(volta && *volta == todos, "binário: ida e volta preserva todos os tipos");
        suite.check(!codificado.empty() && codificado[0] == std::byte{ops::value_encoding_version},
                    "binário: primeiro byte é a versão");
        suite.check(ops::decode(ops::encode(Value{})) == Value{}, "binário: null sozinho");

        auto ver = ops::decode(bytes({9, 0}));
        suite.check(!ver && ver.error().code == ErrorCode::invalid_encoding, "binário: versão desconhecida é recusada");
        suite.check(!ops::decode({}), "binário: entrada vazia é recusada");
        suite.check(!ops::decode(bytes({1, 99})), "binário: tag desconhecida é recusada");
        suite.check(!ops::decode(bytes({1, 1, 2})), "binário: booleano diferente de 0/1 é recusado");
        suite.check(!ops::decode(bytes({1, 0, 0})), "binário: bytes sobrando são recusados");
        auto truncado = ops::encode(todos);
        truncado.resize(truncado.size() / 2);
        suite.check(!ops::decode(truncado), "binário: valor truncado é recusado");
        // Lista que diz ter 4 bilhões de itens em 5 bytes: não pode reservar memória por isso.
        suite.check(!ops::decode(bytes({1, 6, 0xff, 0xff, 0xff, 0xff})), "binário: contagem maior que a entrada é recusada");
        std::vector<std::byte> fundo{std::byte{1}};
        for (int i = 0; i < ops::value_max_depth + 5; ++i) {
            for (auto b : bytes({6, 1, 0, 0, 0})) {
                fundo.push_back(b);
            }
        }
        fundo.push_back(std::byte{0});
        suite.check(!ops::decode(fundo), "binário: aninhamento além do limite é recusado");
        // Chave repetida num mapa.
        suite.check(!ops::decode(bytes({1, 7, 2, 0, 0, 0, 1, 0, 0, 0, 'a', 0, 1, 0, 0, 0, 'a', 0})),
                    "binário: chave repetida é recusada");
    }

    // --- JSON ---
    {
        const auto texto = ops::to_json(todos);
        auto volta = ops::from_json(texto);
        // id vira número em JSON: volta como integer, o resto é igual.
        auto esperado_mapa = *todos.map();
        esperado_mapa["id"] = std::int64_t{12345};
        suite.check(volta && *volta == Value{esperado_mapa}, "JSON: ida e volta (id vira inteiro)");
        suite.check(texto.find("\"real_inteiro\":2.0") != std::string::npos, "JSON: real inteiro mantém o ponto");
        suite.check(ops::to_json(Value::object({{"a", ops::ValueList{1, 2}}, {"b", nullptr}})) == R"({"a":[1,2],"b":null})",
                    "JSON: saída compacta e ordenada");
        auto escape = ops::from_json(R"("aé😀\n")");
        suite.check(escape && *escape->text() == "a\xc3\xa9\xf0\x9f\x98\x80\n", "JSON: escapes e par substituto");
        auto numero = ops::from_json("[1, -2, 3.5, 1e3, 9223372036854775807]");
        suite.check(numero && numero->list()->at(0).integer() && numero->list()->at(2).real() &&
                        numero->list()->at(3).real() && *numero->list()->at(4).integer() == std::numeric_limits<std::int64_t>::max(),
                    "JSON: inteiros viram integer, o resto real");
        suite.check(!ops::from_json("{\"a\":1,}"), "JSON: vírgula sobrando é erro");
        suite.check(!ops::from_json("{\"a\":1} x"), "JSON: conteúdo depois do valor é erro");
        suite.check(!ops::from_json("\"sem fim"), "JSON: texto sem fechar é erro");
        suite.check(!ops::from_json(std::string(200, '[')), "JSON: aninhamento profundo é erro");
        suite.check(!ops::from_json("-"), "JSON: sinal sozinho é erro");
    }

    // --- Args ---
    {
        auto args = ops::Args::from_value(Value::object({
            {"exemplar", object::ObjectId{7}},
            {"leitor", std::int64_t{40}},  // id que veio de JSON
            {"dias", std::int64_t{14}},
            {"nome", "Ana"},
            {"ativo", true},
            {"autores", ops::ValueList{object::ObjectId{1}, std::int64_t{2}}},
            {"nulo", nullptr},
        }));
        suite.check(args.has_value(), "Args a partir de um mapa");
        if (args) {
            suite.check(args->id("exemplar") == object::ObjectId{7}, "id vindo como id");
            suite.check(args->id("leitor") == object::ObjectId{40}, "id vindo como inteiro (JSON)");
            suite.check(args->integer("dias") == 14, "inteiro");
            suite.check(args->text("nome") == std::string{"Ana"}, "texto");
            suite.check(args->boolean("ativo") == true, "booleano");
            auto autores = args->ids("autores");
            suite.check(autores && autores->size() == 2 && (*autores)[1] == object::ObjectId{2}, "lista de ids");
            suite.check(args->text_or("ausente", "padrão") == std::string{"padrão"}, "opcional ausente usa o padrão");
            suite.check(args->integer_or("nulo", 5) == 5, "opcional nulo usa o padrão");
            auto ausente = args->text("ausente");
            suite.check(!ausente && ausente.error().code == ErrorCode::invalid_argument &&
                            ausente.error().message == "argument 'ausente' is required",
                        "obrigatório ausente diz qual argumento");
            auto errado = args->integer("nome");
            suite.check(!errado && errado.error().message == "argument 'nome' must be an integer, got text",
                        "tipo errado diz o esperado e o recebido");
            suite.check(!args->text_or("dias", ""), "opcional presente com tipo errado continua erro");
            suite.check(!args->id("dias").has_value() == false, "inteiro positivo serve como id");
            suite.check(!ops::Args::from_value(Value::object({{"x", std::int64_t{0}}}))->id("x"), "id 0 é recusado");
        }
        suite.check(!ops::Args::from_value(Value{ops::ValueList{}}), "argumentos que não são mapa são recusados");
        auto vazio = ops::Args::decode({});
        suite.check(vazio && vazio->values().empty(), "sem bytes = sem argumentos");
        auto via_bytes = ops::Args::decode(ops::Args{*Value::object({{"k", "v"}}).map()}.encode());
        suite.check(via_bytes && via_bytes->text("k") == std::string{"v"}, "Args encode/decode");
    }

    return suite.finish();
}
