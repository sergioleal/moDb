#pragma once

// Descritores de facade e método (Fase 11A). Identidade pública é FacadeId +
// versão — nunca a posição no vetor do catálogo (ADR-014).

#include "modb/error.hpp"
#include "modb/ops/operation.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace modb::ops {

enum class FacadeMode : std::uint8_t {
    read_only = 0,
    read_write = 1,
    mixed = 2,
};

struct MethodDescriptor {
    std::string operation_id{};
    std::uint32_t method_version{1};
    OperationMode mode{OperationMode::read_write};

    friend bool operator==(const MethodDescriptor&, const MethodDescriptor&) = default;
};

struct FacadeDescriptor {
    std::string facade_id{};
    std::uint32_t facade_version{1};
    FacadeMode mode{FacadeMode::mixed};
    std::vector<MethodDescriptor> methods{};

    friend bool operator==(const FacadeDescriptor&, const FacadeDescriptor&) = default;
};

// Localiza um método pelo operation_id dentro do descriptor.
[[nodiscard]] Result<const MethodDescriptor*> find_method(const FacadeDescriptor& facade,
                                                          std::string_view operation_id);

} // namespace modb::ops
