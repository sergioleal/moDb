#include "notas_procs.hpp"

#include "modb/ops/operation.hpp"
#include "modb/storage/binary.hpp"

#include <memory>

namespace modb::examples::notas {

std::vector<std::byte> texto_para_bytes(std::string_view texto) {
    const auto* p = reinterpret_cast<const std::byte*>(texto.data());
    return {p, p + texto.size()};
}

std::string bytes_para_texto(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::vector<std::byte> id_para_bytes(std::uint64_t id) {
    storage::BinaryWriter writer;
    writer.write_u64(id);
    return std::move(writer).take();
}

Result<std::uint64_t> bytes_para_id(std::span<const std::byte> bytes) {
    storage::BinaryReader reader{bytes};
    auto id = reader.read_u64();
    if (!id) {
        return std::unexpected(id.error());
    }
    if (!reader.at_end()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "id com bytes sobrando"});
    }
    return *id;
}

namespace {

object::BindingBuilder<Nota> nota_binding() {
    object::BindingBuilder<Nota> b{"Nota"};
    b.field<1>("texto", &Nota::texto);
    return b;
}

class Criar final : public ops::Operation {
public:
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_write;
    explicit Criar(std::string texto) : texto_{std::move(texto)} {}
    [[nodiscard]] std::string_view id() const noexcept override { return k_criar; }
    [[nodiscard]] ops::OperationMode mode() const noexcept override { return k_mode; }
    static Result<std::unique_ptr<ops::Operation>> decode(std::span<const std::byte> args) {
        return std::unique_ptr<ops::Operation>{new Criar{bytes_para_texto(args)}};
    }
    Result<ops::OperationResult> execute(ops::ExecutionContext& context) override {
        if (texto_.empty()) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "a nota não pode ser vazia"});
        }
        auto nota = context.objects().create(Nota{texto_});
        if (!nota) {
            return std::unexpected(nota.error());
        }
        return ops::OperationResult{.payload = id_para_bytes(nota->id().value)};
    }

private:
    std::string texto_;
};

class Ler final : public ops::Operation {
public:
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_only;
    explicit Ler(std::uint64_t id) : id_{id} {}
    [[nodiscard]] std::string_view id() const noexcept override { return k_ler; }
    [[nodiscard]] ops::OperationMode mode() const noexcept override { return k_mode; }
    static Result<std::unique_ptr<ops::Operation>> decode(std::span<const std::byte> args) {
        auto id = bytes_para_id(args);
        if (!id) {
            return std::unexpected(id.error());
        }
        return std::unique_ptr<ops::Operation>{new Ler{*id}};
    }
    Result<ops::OperationResult> execute(ops::ExecutionContext& context) override {
        auto nota = context.objects().read<Nota>(object::ObjectId{id_});
        if (!nota) {
            return std::unexpected(nota.error());
        }
        return ops::OperationResult{.payload = texto_para_bytes(nota->texto)};
    }

private:
    std::uint64_t id_;
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
