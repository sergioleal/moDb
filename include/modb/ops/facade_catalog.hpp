#pragma once

// Catálogo runtime de facades (Fase 11A). Backing store: vector<>; lookup por
// FacadeId + versão (ADR-014).

#include "modb/error.hpp"
#include "modb/ops/facade_descriptor.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace modb::ops {

class FacadeCatalog {
public:
    [[nodiscard]] Result<void> register_facade(FacadeDescriptor desc);

    // Id ausente → facade_not_found. Id presente, versão diferente →
    // incompatible_facade_version.
    [[nodiscard]] Result<FacadeDescriptor> find(std::string_view facade_id,
                                                std::uint32_t version) const;

    [[nodiscard]] Result<const MethodDescriptor*> find_method(std::string_view facade_id,
                                                             std::uint32_t version,
                                                             std::string_view operation_id) const;

    [[nodiscard]] std::span<const FacadeDescriptor> list() const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return facades_.size(); }

private:
    std::vector<FacadeDescriptor> facades_{};
};

} // namespace modb::ops
