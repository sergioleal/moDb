#include "modb/ops/facade_catalog.hpp"

#include <string>
#include <utility>

namespace modb::ops {
namespace {

[[nodiscard]] bool same_id(std::string_view left, std::string_view right) noexcept {
    return left == right;
}

} // namespace

Result<const MethodDescriptor*> find_method(const FacadeDescriptor& facade,
                                            std::string_view operation_id) {
    if (operation_id.empty()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "operation_id must not be empty"});
    }
    for (const auto& method : facade.methods) {
        if (method.operation_id == operation_id) {
            return &method;
        }
    }
    return std::unexpected(Error{ErrorCode::facade_method_not_found,
                                 "method not in facade '" + facade.facade_id + "': " +
                                     std::string{operation_id}});
}

Result<void> FacadeCatalog::register_facade(FacadeDescriptor desc) {
    if (desc.facade_id.empty()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "facade_id must not be empty"});
    }
    for (const auto& method : desc.methods) {
        if (method.operation_id.empty()) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "method operation_id must not be empty"});
        }
    }
    for (const auto& existing : facades_) {
        if (same_id(existing.facade_id, desc.facade_id) &&
            existing.facade_version == desc.facade_version) {
            return std::unexpected(Error{ErrorCode::invalid_argument,
                                         "facade already registered: " + desc.facade_id +
                                             " v" + std::to_string(desc.facade_version)});
        }
    }
    facades_.push_back(std::move(desc));
    return {};
}

Result<FacadeDescriptor> FacadeCatalog::find(std::string_view facade_id,
                                             std::uint32_t version) const {
    if (facade_id.empty()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "facade_id must not be empty"});
    }

    bool id_seen = false;
    for (const auto& facade : facades_) {
        if (!same_id(facade.facade_id, facade_id)) {
            continue;
        }
        id_seen = true;
        if (facade.facade_version == version) {
            return facade;
        }
    }
    if (!id_seen) {
        return std::unexpected(
            Error{ErrorCode::facade_not_found, "facade not found: " + std::string{facade_id}});
    }
    return std::unexpected(Error{ErrorCode::incompatible_facade_version,
                                 "incompatible facade version for '" + std::string{facade_id} +
                                     "': requested " + std::to_string(version)});
}

Result<const MethodDescriptor*> FacadeCatalog::find_method(std::string_view facade_id,
                                                           std::uint32_t version,
                                                           std::string_view operation_id) const {
    if (facade_id.empty()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "facade_id must not be empty"});
    }
    if (operation_id.empty()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "operation_id must not be empty"});
    }

    bool id_seen = false;
    for (const auto& facade : facades_) {
        if (!same_id(facade.facade_id, facade_id)) {
            continue;
        }
        id_seen = true;
        if (facade.facade_version != version) {
            continue;
        }
        return modb::ops::find_method(facade, operation_id);
    }
    if (!id_seen) {
        return std::unexpected(
            Error{ErrorCode::facade_not_found, "facade not found: " + std::string{facade_id}});
    }
    return std::unexpected(Error{ErrorCode::incompatible_facade_version,
                                 "incompatible facade version for '" + std::string{facade_id} +
                                     "': requested " + std::to_string(version)});
}

std::span<const FacadeDescriptor> FacadeCatalog::list() const noexcept {
    return facades_;
}

} // namespace modb::ops
