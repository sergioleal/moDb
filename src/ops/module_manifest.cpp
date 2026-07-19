#include "modb/ops/module_manifest.hpp"

#include <sstream>

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

} // namespace

BinaryHash compute_manifest_hash(const ModuleManifest& manifest) {
    std::ostringstream body;
    body << manifest.id << '|' << manifest.module_version << '|' << manifest.baseline.value << '|'
         << manifest.api_version << '|' << (manifest.migration ? '1' : '0');
    for (const auto& method : manifest.methods) {
        body << '|' << method.id << ':' << static_cast<int>(method.mode);
    }
    return fnv1a_hex(body.str());
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

} // namespace modb::ops
