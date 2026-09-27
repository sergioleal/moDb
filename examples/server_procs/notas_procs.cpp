#include "notas_procs.hpp"

#include "modb/ops/operation.hpp"

#include <memory>

namespace modb::examples::notas {

namespace {

object::BindingBuilder<Nota> nota_binding() {
    object::BindingBuilder<Nota> b{"Nota"};
    b.field<1>("texto", &Nota::texto);
    return b;
}

ops::OperationResult resultado(const ops::Value& v) { return ops::OperationResult{.payload = ops::encode(v)}; }

class Criar final : public ops::Operation {
public:
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_write;
    explicit Criar(std::string texto) : texto_{std::move(texto)} {}
    [[nodiscard]] std::string_view id() const noexcept override { return k_criar; }
    [[nodiscard]] ops::OperationMode mode() const noexcept override { return k_mode; }
    static Result<std::unique_ptr<ops::Operation>> decode(std::span<const std::byte> bytes) {
        auto args = ops::Args::decode(bytes);
        auto texto = args ? args->text("texto") : Result<std::string>{std::unexpected(args.error())};
        if (!texto) {
            return std::unexpected(texto.error());
        }
        return std::unique_ptr<ops::Operation>{new Criar{std::move(*texto)}};
    }
    Result<ops::OperationResult> execute(ops::ExecutionContext& context) override {
        if (texto_.empty()) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "a nota não pode ser vazia"});
        }
        auto nota = context.objects().create(Nota{texto_});
        if (!nota) {
            return std::unexpected(nota.error());
        }
        return resultado(ops::Value::object({{"id", nota->id()}}));
    }

private:
    std::string texto_;
};

class Ler final : public ops::Operation {
public:
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_only;
    explicit Ler(object::ObjectId id) : id_{id} {}
    [[nodiscard]] std::string_view id() const noexcept override { return k_ler; }
    [[nodiscard]] ops::OperationMode mode() const noexcept override { return k_mode; }
    static Result<std::unique_ptr<ops::Operation>> decode(std::span<const std::byte> bytes) {
        auto args = ops::Args::decode(bytes);
        auto id = args ? args->id("id") : Result<object::ObjectId>{std::unexpected(args.error())};
        if (!id) {
            return std::unexpected(id.error());
        }
        return std::unique_ptr<ops::Operation>{new Ler{*id}};
    }
    Result<ops::OperationResult> execute(ops::ExecutionContext& context) override {
        auto nota = context.objects().read<Nota>(id_);
        if (!nota) {
            return std::unexpected(nota.error());
        }
        return resultado(ops::Value::object({{"id", id_}, {"texto", nota->texto}}));
    }

private:
    object::ObjectId id_;
};

} // namespace

} // namespace modb::examples::notas

modb::server::Module modb_module_notas_procs() {
    using namespace modb::examples::notas;
    return modb::server::Module{
        .id = "notas",
        .version = 1,
        .prepare = [](modb::object::Database& db) { return db.bind(nota_binding()); },
        .register_procs =
            [](modb::ops::OperationRegistry& registry) -> modb::Result<void> {
            if (auto ok = registry.register_operation<Criar>(std::string{k_criar}); !ok) {
                return ok;
            }
            return registry.register_operation<Ler>(std::string{k_ler});
        },
        .methods = {{.id = std::string{k_criar}, .mode = Criar::k_mode},
                    {.id = std::string{k_ler}, .mode = Ler::k_mode}},
    };
}
