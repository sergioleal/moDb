#include "loadtest_facade.hpp"

#include "dataset_user.hpp"
#include "user_type.hpp"

#include "modb/storage/binary.hpp"

namespace modb::loadtest {
namespace {

Error make_args_error(std::string message) {
    return Error{ErrorCode::invalid_argument, std::move(message)};
}

} // namespace

Result<std::vector<std::byte>> CreateBatch::encode_args(std::uint64_t seed,
                                                        std::uint64_t start_index,
                                                        std::uint64_t count,
                                                        std::string_view payload) {
    if (payload.size() > 0xff) {
        return std::unexpected(make_args_error("payload id too long"));
    }
    storage::BinaryWriter writer;
    writer.write_u64(seed);
    writer.write_u64(start_index);
    writer.write_u64(count);
    writer.write_u8(static_cast<std::uint8_t>(payload.size()));
    writer.write_bytes(std::as_bytes(std::span{payload}));
    return std::move(writer).take();
}

Result<std::unique_ptr<ops::Operation>> CreateBatch::decode(std::span<const std::byte> args) {
    storage::BinaryReader reader{args};
    const auto seed = reader.read_u64();
    if (!seed) {
        return std::unexpected(seed.error());
    }
    const auto start_index = reader.read_u64();
    if (!start_index) {
        return std::unexpected(start_index.error());
    }
    const auto count = reader.read_u64();
    if (!count) {
        return std::unexpected(count.error());
    }
    const auto payload_len = reader.read_u8();
    if (!payload_len) {
        return std::unexpected(payload_len.error());
    }
    const auto payload_bytes = reader.read_bytes(*payload_len);
    if (!payload_bytes) {
        return std::unexpected(payload_bytes.error());
    }
    if (!reader.at_end()) {
        return std::unexpected(make_args_error("CreateBatch args have trailing bytes"));
    }
    std::string payload(reinterpret_cast<const char*>(payload_bytes->data()), payload_bytes->size());
    return std::unique_ptr<ops::Operation>{
        new CreateBatch{*seed, *start_index, *count, std::move(payload)}};
}

Result<ops::OperationResult> CreateBatch::execute(ops::ExecutionContext& context) {
    auto& access = context.objects();
    for (std::uint64_t i = 0; i < count_; ++i) {
        const auto generated = generate_user(seed_, start_index_ + i, payload_);
        if (auto created = access.create<User>(to_engine_user(generated)); !created) {
            return std::unexpected(created.error());
        }
    }
    return ops::OperationResult{};
}

ops::FacadeDescriptor loadtest_facade_descriptor() {
    return ops::FacadeDescriptor{
        .facade_id = std::string{LoadtestFacade::k_id},
        .facade_version = LoadtestFacade::k_version,
        .mode = ops::FacadeMode::read_write,
        .methods =
            {
                ops::MethodDescriptor{.operation_id = std::string{CreateBatch::k_id},
                                      .method_version = 1,
                                      .mode = CreateBatch::k_mode},
            },
    };
}

ops::ModuleManifest loadtest_facade_manifest(object::BaselineId baseline) {
    ops::ModuleManifest manifest{
        .id = "loadtest_facade",
        .module_version = 1,
        .baseline = baseline,
        .api_version = ops::runtime_api_version,
        .methods =
            {
                ops::ExportedMethod{.id = std::string{CreateBatch::k_id}, .mode = CreateBatch::k_mode},
            },
        .facades = {loadtest_facade_descriptor()},
    };
    manifest.hash = ops::compute_manifest_hash(manifest);
    return manifest;
}

Result<void> register_loadtest_facade_module(ops::OperationRegistry& registry) {
    return registry.register_operation<CreateBatch>(std::string{CreateBatch::k_id});
}

} // namespace modb::loadtest
