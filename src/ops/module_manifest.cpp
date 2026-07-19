#include "modb/ops/module_manifest.hpp"

#include <sstream>
#include <string>

namespace modb::ops {
namespace {

// FNV-1a 64-bit → hex (sem dependência criptográfica no MVP).
[[nodiscard]] BinaryHash fnv1a_hex(std::string_view data) {
    std::uint64_t hash = 14695981039346656037ull;
    for (char c : data) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
}

[[nodiscard]] const ExportedMethod* find_export(const ModuleManifest& manifest,
                                                std::string_view operation_id) noexcept {
    for (const auto& method : manifest.methods) {
        if (method.id == operation_id) {
            return &method;
        }
    }
    return nullptr;
}

} // namespace

BinaryHash compute_manifest_hash(const ModuleManifest& manifest) {
    std::ostringstream body;
    body << manifest.id << '|' << manifest.module_version << '|' << manifest.baseline.value << '|'
         << manifest.api_version << '|' << (manifest.migration ? '1' : '0');
    for (const auto& method : manifest.methods) {
        body << '|' << method.id << ':' << static_cast<int>(method.mode);
    }
    for (const auto& facade : manifest.facades) {
        body << "|F:" << facade.facade_id << ':' << facade.facade_version << ':'
             << static_cast<int>(facade.mode);
        for (const auto& method : facade.methods) {
            body << "|M:" << method.operation_id << ':' << method.method_version << ':'
                 << static_cast<int>(method.mode);
        }
    }
    return fnv1a_hex(body.str());
}

Result<void> register_facades_from_manifest(const ModuleManifest& manifest,
                                            const OperationRegistry& operations,
                                            FacadeCatalog& catalog) {
    for (const auto& facade : manifest.facades) {
        if (facade.facade_id.empty()) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "manifest facade_id must not be empty"});
        }
        if (facade.methods.empty()) {
            return std::unexpected(Error{ErrorCode::invalid_argument,
                                         "manifest facade '" + facade.facade_id +
                                             "' exports no methods"});
        }
        for (const auto& method : facade.methods) {
            if (method.operation_id.empty()) {
                return std::unexpected(Error{ErrorCode::invalid_argument,
                                             "manifest facade method id must not be empty"});
            }
            const auto* exported = find_export(manifest, method.operation_id);
            if (exported == nullptr) {
                return std::unexpected(Error{ErrorCode::invalid_argument,
                                             "facade method not in module exports: " +
                                                 method.operation_id});
            }
            if (exported->mode != method.mode) {
                return std::unexpected(Error{ErrorCode::invalid_argument,
                                             "facade method mode mismatches export: " +
                                                 method.operation_id});
            }
            if (!operations.contains(method.operation_id)) {
                return std::unexpected(Error{ErrorCode::operation_not_found,
                                             "facade method not registered: " +
                                                 method.operation_id});
            }
        }
        if (auto status = catalog.register_facade(facade); !status) {
            return status;
        }
    }
    return {};
}

Result<void> ModuleLoader::load(const ModuleManifest& manifest,
                                object::BaselineId database_baseline,
                                OperationRegistry& registry, Registrar registrar) {
    if (manifest.id.empty()) {
        return std::unexpected(Error{ErrorCode::incompatible_module, "module id is empty"});
    }
    if (manifest.api_version != runtime_api_version) {
        return std::unexpected(
            Error{ErrorCode::incompatible_module, "module api_version mismatch"});
    }
    if (!manifest.migration && manifest.baseline.value != 0 &&
        manifest.baseline.value != database_baseline.value) {
        return std::unexpected(
            Error{ErrorCode::incompatible_module, "module baseline incompatible"});
    }
    if (manifest.methods.empty()) {
        return std::unexpected(Error{ErrorCode::incompatible_module, "module exports nothing"});
    }
    const auto expected = compute_manifest_hash(manifest);
    if (manifest.hash != expected) {
        return std::unexpected(
            Error{ErrorCode::incompatible_module, "module hash does not match manifest contents"});
    }
    if (!is_admitted(manifest.hash)) {
        return std::unexpected(
            Error{ErrorCode::incompatible_module, "module hash not in allowlist"});
    }
    if (!registrar) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "module registrar is empty"});
    }
    return registrar(registry);
}

Result<void> ModuleLoader::load(const ModuleManifest& manifest,
                                object::BaselineId database_baseline,
                                OperationRegistry& registry, FacadeCatalog& catalog,
                                Registrar registrar) {
    if (auto status = load(manifest, database_baseline, registry, std::move(registrar)); !status) {
        return status;
    }
    return register_facades_from_manifest(manifest, registry, catalog);
}

} // namespace modb::ops
