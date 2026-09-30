#include "notas_procs.hpp"

#include <charconv>
#include <chrono>
#include <stdexcept>
#include <thread>

using modb::Result;
using modb::object::ObjectId;
using modb::ops::Args;
using modb::ops::Value;
using modb::ops::ValueList;
using modb::server::conflict;
using modb::server::Context;
using modb::server::invalid;
using modb::server::Mode;
using modb::server::not_found;
using namespace modb::examples::notas;

namespace {

modb::object::BindingBuilder<Nota> nota_binding() {
    modb::object::BindingBuilder<Nota> b{"Nota"};
    b.field<1>("texto", &Nota::texto);
    return b;
}

Value nota_json(ObjectId id, const Nota& n) { return Value::object({{"id", id}, {"texto", n.texto}}); }

// Configuração `notas.max_texto`: um inteiro de 1 a 1000000 (bytes).
Result<std::size_t> parse_max_texto(std::string_view text) {
    std::size_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size() || value < 1 || value > 1000000) {
        return std::unexpected(invalid("use um inteiro de 1 a 1000000"));
    }
    return value;
}

// Texto obrigatório, não vazio, até `notas.max_texto` bytes e único (pelo
// índice). `proprio` é a nota sendo editada.
Result<std::string> texto_valido(Context& c, const Args& a, ObjectId proprio = {}) {
    auto texto = a.text("texto");
    if (!texto) {
        return texto;
    }
    if (texto->empty()) {
        return std::unexpected(invalid("a nota não pode ser vazia"));
    }
    // Validado na subida: aqui só converte.
    if (const auto max = *parse_max_texto(c.setting("max_texto")); texto->size() > max) {
        return std::unexpected(invalid("a nota passa de " + std::to_string(max) + " bytes"));
    }
    auto iguais = c.find<Nota>(k_texto, modb::object::AttributeValue{*texto});
    if (!iguais) {
        return std::unexpected(iguais.error());
    }
    for (const auto id : *iguais) {
        if (id != proprio) {
            return std::unexpected(conflict("já existe uma nota com esse texto"));
        }
    }
    return texto;
}

// Lê a nota ou explica que ela não existe.
Result<Nota> nota_existente(Context& c, ObjectId id) {
    auto n = c.read<Nota>(id);
    if (!n && n.error().code == modb::ErrorCode::record_not_found) {
        return std::unexpected(not_found("nota " + std::to_string(id.value) + " não existe"));
    }
    return n;
}

} // namespace

modb::server::Module modb_module_notas_procs() {
    return modb::server::ModuleBuilder{"notas"}
        .type(nota_binding())
        .index<Nota>(k_texto)
        .setting("max_texto", "1000", "Tamanho máximo do texto de uma nota, em bytes (1 a 1000000)",
                 [](std::string_view v) -> Result<void> {
                     if (auto n = parse_max_texto(v); !n) {
                         return std::unexpected(n.error());
                     }
                     return {};
                 })
        .proc("notas.criar", Mode::read_write, "Cria uma nota; o texto é único",
              [](Context& c, const Args& a) -> Result<Value> {
                  auto texto = texto_valido(c, a);
                  if (!texto) {
                      return std::unexpected(texto.error());
                  }
                  auto id = c.create(Nota{*texto});
                  if (!id) {
                      return std::unexpected(id.error());
                  }
                  return Value::object({{"id", *id}});
              })
        .proc("notas.ler", Mode::read_only, "Uma nota pelo id",
              [](Context& c, const Args& a) -> Result<Value> {
                  auto id = a.id("id");
                  if (!id) {
                      return std::unexpected(id.error());
                  }
                  auto n = nota_existente(c, *id);
                  if (!n) {
                      return std::unexpected(n.error());
                  }
                  return nota_json(*id, *n);
              })
        .proc("notas.listar", Mode::read_only, "Todas as notas, ou as que contêm {contem}",
              [](Context& c, const Args& a) -> Result<Value> {
                  auto contem = a.text_or("contem", "");
                  if (!contem) {
                      return std::unexpected(contem.error());
                  }
                  auto notas = contem->empty()
                                   ? c.all<Nota>()
                                   : c.where<Nota>([t = *contem](const Nota& n) { return n.texto.find(t) != std::string::npos; });
                  if (!notas) {
                      return std::unexpected(notas.error());
                  }
                  ValueList itens;
                  for (const auto& [id, n] : *notas) {
                      itens.push_back(nota_json(id, n));
                  }
                  return Value{std::move(itens)};
              })
        .proc("notas.editar", Mode::read_write, "Troca o texto de uma nota",
              [](Context& c, const Args& a) -> Result<Value> {
                  auto id = a.id("id");
                  if (!id) {
                      return std::unexpected(id.error());
                  }
                  if (auto n = nota_existente(c, *id); !n) {
                      return std::unexpected(n.error());
                  }
                  auto texto = texto_valido(c, a, *id);
                  if (!texto) {
                      return std::unexpected(texto.error());
                  }
                  if (auto ok = c.set<&Nota::texto>(*id, *texto); !ok) {
                      return std::unexpected(ok.error());
                  }
                  return nota_json(*id, Nota{*texto});
              })
        .proc("notas.apagar", Mode::read_write, "Apaga uma nota",
              [](Context& c, const Args& a) -> Result<Value> {
                  auto id = a.id("id");
                  if (!id) {
                      return std::unexpected(id.error());
                  }
                  if (auto n = nota_existente(c, *id); !n) {
                      return std::unexpected(n.error());
                  }
                  if (auto ok = c.remove(*id); !ok) {
                      return std::unexpected(ok.error());
                  }
                  return Value::object({});
              })
        .proc("notas.excecao", Mode::read_write, "Cria uma nota e lança exceção (teste de rollback)",
              [](Context& c, const Args& a) -> Result<Value> {
                  auto texto = a.text("texto");
                  if (!texto) {
                      return std::unexpected(texto.error());
                  }
                  (void)c.create(Nota{*texto});
                  throw std::runtime_error("falha simulada depois de escrever");
              })
        .proc("notas.lenta", Mode::read_write, "Cria uma nota, espera {ms} e relê (teste do tempo limite)",
              [](Context& c, const Args& a) -> Result<Value> {
                  auto texto = a.text("texto");
                  if (!texto) {
                      return std::unexpected(texto.error());
                  }
                  auto ms = a.integer_or("ms", 0);
                  if (!ms) {
                      return std::unexpected(ms.error());
                  }
                  auto id = c.create(Nota{*texto});
                  if (!id) {
                      return std::unexpected(id.error());
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds{*ms});
                  // Passou do prazo: o próximo acesso ao banco já falha com operation_timeout.
                  auto nota = c.read<Nota>(*id);
                  if (!nota) {
                      return std::unexpected(nota.error());
                  }
                  return Value::object({{"id", *id}});
              })
        .build();
}
