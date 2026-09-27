#include "modb/server/module.hpp"

namespace modb::server {

namespace {

// Operação genérica: argumentos já decodificados + a função da proc.
class FunctionOperation final : public ops::Operation {
public:
    FunctionOperation(std::string name, Mode mode, std::shared_ptr<ProcFn> fn, ops::Args args)
        : name_{std::move(name)}, mode_{mode}, fn_{std::move(fn)}, args_{std::move(args)} {}

    [[nodiscard]] std::string_view id() const noexcept override { return name_; }
    [[nodiscard]] ops::OperationMode mode() const noexcept override { return mode_; }

    Result<ops::OperationResult> execute(ops::ExecutionContext& context) override {
        Context c{context};
        auto value = (*fn_)(c, args_);
        if (!value) {
            return std::unexpected(value.error());
        }
        return ops::OperationResult{.payload = ops::encode(*value)};
    }

private:
    std::string name_;
    Mode mode_;
    std::shared_ptr<ProcFn> fn_;
    ops::Args args_;
};

} // namespace

ModuleBuilder& ModuleBuilder::proc(std::string name, Mode mode, std::string description, ProcFn fn) {
    procs_.push_back(Proc{std::move(name), mode, std::move(description), std::make_shared<ProcFn>(std::move(fn))});
    return *this;
}

Module ModuleBuilder::build() const {
    Module module;
    module.id = id_;
    module.version = version_;
    const auto preparers = preparers_;
    module.prepare = [preparers](object::Database& db) -> Result<void> {
        for (const auto& prepare : preparers) {
            if (auto ok = prepare(db); !ok) {
                return ok;
            }
        }
        return {};
    };
    const auto procs = procs_;
    module.register_procs = [procs](ops::OperationRegistry& registry) -> Result<void> {
        for (const auto& p : procs) {
            if (!p.fn || !*p.fn) {
                return std::unexpected(Error{ErrorCode::invalid_argument, "proc without a function: " + p.name});
            }
            auto factory = [name = p.name, mode = p.mode,
                            fn = p.fn](std::span<const std::byte> bytes) -> Result<std::unique_ptr<ops::Operation>> {
                auto args = ops::Args::decode(bytes);
                if (!args) {
                    return std::unexpected(args.error());
                }
                return std::unique_ptr<ops::Operation>{new FunctionOperation{name, mode, fn, std::move(*args)}};
            };
            if (auto ok = registry.register_factory(p.name, std::move(factory), p.mode); !ok) {
                return ok;
            }
        }
        return {};
    };
    for (const auto& p : procs_) {
        module.methods.push_back(ops::ExportedMethod{.id = p.name, .mode = p.mode});
        module.descriptions.emplace(p.name, p.description);
    }
    return module;
}

} // namespace modb::server
