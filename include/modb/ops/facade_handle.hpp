#pragma once

// Handle tipado para invocação de métodos de uma facade (Fases 11B/11D).
// Transporte: embedded (OperationRegistry) ou remoto (Client::call / OpCall).

#include "modb/error.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/facade_catalog.hpp"
#include "modb/ops/facade_descriptor.hpp"
#include "modb/ops/operation.hpp"
#include "modb/ops/operation_registry.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace modb::ops {

// Despacha um método já validado na facade (embedded ou rede).
using FacadeInvoker =
    std::function<Result<OperationResult>(std::string_view operation_id,
                                          std::span<const std::byte> args)>;

template <typename TFacade>
class FacadeHandle {
public:
    [[nodiscard]] static Result<FacadeHandle> open(const FacadeCatalog& catalog,
                                                   OperationRegistry& registry,
                                                   object::Database& database) {
        auto descriptor = catalog.find(TFacade::k_id, TFacade::k_version);
        if (!descriptor) {
            return std::unexpected(descriptor.error());
        }
        FacadeInvoker invoker = [&registry, &database](std::string_view operation_id,
                                                       std::span<const std::byte> args) {
            return registry.dispatch(operation_id, args, database);
        };
        return FacadeHandle{std::move(*descriptor), std::move(invoker)};
    }

    [[nodiscard]] static Result<FacadeHandle> open(FacadeDescriptor descriptor,
                                                   FacadeInvoker invoker) {
        if (!invoker) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "facade invoker must not be empty"});
        }
        if (descriptor.facade_id != TFacade::k_id ||
            descriptor.facade_version != TFacade::k_version) {
            return std::unexpected(Error{ErrorCode::incompatible_facade_version,
                                         "descriptor does not match facade type"});
        }
        return FacadeHandle{std::move(descriptor), std::move(invoker)};
    }

    [[nodiscard]] std::string_view facade_id() const noexcept { return descriptor_.facade_id; }
    [[nodiscard]] std::uint32_t version() const noexcept { return descriptor_.facade_version; }
    [[nodiscard]] const FacadeDescriptor& descriptor() const noexcept { return descriptor_; }

    template <typename Method, typename... Args>
    [[nodiscard]] Result<OperationResult> invoke(Args&&... args) {
        auto method = find_method(descriptor_, Method::k_id);
        if (!method) {
            return std::unexpected(method.error());
        }

        auto encoded = Method::encode_args(std::forward<Args>(args)...);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        return invoker_(Method::k_id, *encoded);
    }

private:
    FacadeHandle(FacadeDescriptor descriptor, FacadeInvoker invoker)
        : descriptor_{std::move(descriptor)}, invoker_{std::move(invoker)} {}

    FacadeDescriptor descriptor_{};
    FacadeInvoker invoker_{};
};

template <typename TFacade>
[[nodiscard]] Result<FacadeHandle<TFacade>> open_facade(const FacadeCatalog& catalog,
                                                        OperationRegistry& registry,
                                                        object::Database& database) {
    return FacadeHandle<TFacade>::open(catalog, registry, database);
}

} // namespace modb::ops
