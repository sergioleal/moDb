#pragma once

// Handle embedded tipado para invocação de métodos de uma facade (Fase 11B).

#include "modb/error.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/facade_catalog.hpp"
#include "modb/ops/operation.hpp"
#include "modb/ops/operation_registry.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace modb::ops {

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
        return FacadeHandle{catalog, registry, database, descriptor->facade_id,
                            descriptor->facade_version};
    }

    [[nodiscard]] std::string_view facade_id() const noexcept { return facade_id_; }
    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }

    template <typename Method, typename... Args>
    [[nodiscard]] Result<OperationResult> invoke(Args&&... args) {
        auto method = catalog_->find_method(facade_id_, version_, Method::k_id);
        if (!method) {
            return std::unexpected(method.error());
        }

        auto encoded = Method::encode_args(std::forward<Args>(args)...);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        return registry_->dispatch(Method::k_id, *encoded, *database_);
    }

private:
    FacadeHandle(const FacadeCatalog& catalog, OperationRegistry& registry,
                 object::Database& database, std::string facade_id, std::uint32_t version)
        : catalog_{&catalog}, registry_{&registry}, database_{&database},
          facade_id_{std::move(facade_id)}, version_{version} {}

    const FacadeCatalog* catalog_{};
    OperationRegistry* registry_{};
    object::Database* database_{};
    std::string facade_id_{};
    std::uint32_t version_{};
};

template <typename TFacade>
[[nodiscard]] Result<FacadeHandle<TFacade>> open_facade(const FacadeCatalog& catalog,
                                                        OperationRegistry& registry,
                                                        object::Database& database) {
    return FacadeHandle<TFacade>::open(catalog, registry, database);
}

} // namespace modb::ops
