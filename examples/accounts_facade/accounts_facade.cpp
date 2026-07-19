#include "examples/accounts_facade/accounts_facade.hpp"

namespace modb::examples {

ops::FacadeDescriptor accounts_facade_descriptor() {
    return ops::FacadeDescriptor{
        .facade_id = std::string{AccountsFacade::k_id},
        .facade_version = AccountsFacade::k_version,
        .mode = ops::FacadeMode::read_write,
        .methods =
            {
                ops::MethodDescriptor{.operation_id = std::string{TransferFunds::k_id},
                                      .method_version = 1,
                                      .mode = TransferFunds::k_mode},
            },
    };
}

ops::ModuleManifest accounts_facade_manifest(object::BaselineId baseline) {
    ops::ModuleManifest manifest{
        .id = "accounts_facade",
        .module_version = 1,
        .baseline = baseline,
        .api_version = ops::runtime_api_version,
        .methods =
            {
                ops::ExportedMethod{.id = std::string{TransferFunds::k_id},
                                    .mode = TransferFunds::k_mode},
            },
        .facades = {accounts_facade_descriptor()},
    };
    manifest.hash = ops::compute_manifest_hash(manifest);
    return manifest;
}

Result<void> register_accounts_facade_module(ops::OperationRegistry& registry) {
    return registry.register_operation<TransferFunds>(std::string{TransferFunds::k_id});
}

} // namespace modb::examples
