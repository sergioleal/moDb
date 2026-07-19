#include "examples/transfer_funds/transfer_funds.hpp"

#include "modb/ops/module_manifest.hpp"

#include <cstring>
#include <stdexcept>

namespace modb::examples {
namespace {

Error make_args_error(std::string message) {
    return Error{ErrorCode::invalid_argument, std::move(message)};
}

} // namespace

Result<std::vector<std::byte>> TransferFunds::encode_args(object::ObjectId source,
                                                          object::ObjectId destination,
                                                          std::int64_t amount) {
    storage::BinaryWriter writer;
    writer.write_u64(source.value);
    writer.write_u64(destination.value);
    writer.write_u64(static_cast<std::uint64_t>(amount));
    return std::move(writer).take();
}

Result<std::unique_ptr<ops::Operation>> TransferFunds::decode(std::span<const std::byte> args) {
    storage::BinaryReader reader{args};
    const auto source = reader.read_u64();
    if (!source) {
        return std::unexpected(source.error());
    }
    const auto destination = reader.read_u64();
    if (!destination) {
        return std::unexpected(destination.error());
    }
    const auto amount_bits = reader.read_u64();
    if (!amount_bits) {
        return std::unexpected(amount_bits.error());
    }
    if (!reader.at_end()) {
        return std::unexpected(make_args_error("TransferFunds args have trailing bytes"));
    }
    return std::unique_ptr<ops::Operation>{new TransferFunds{
        object::ObjectId{*source}, object::ObjectId{*destination},
        static_cast<std::int64_t>(*amount_bits)}};
}

Result<ops::OperationResult> TransferFunds::execute(ops::ExecutionContext& context) {
    if (amount_ <= 0) {
        return std::unexpected(make_args_error("transfer amount must be positive"));
    }
    if (source_.value == destination_.value) {
        return std::unexpected(make_args_error("source and destination must differ"));
    }

    auto& access = context.objects();
    auto source_handle = access.get<Account>(source_);
    if (!source_handle) {
        return std::unexpected(source_handle.error());
    }
    auto dest_handle = access.get<Account>(destination_);
    if (!dest_handle) {
        return std::unexpected(dest_handle.error());
    }

    auto source_account = access.materialize(*source_handle);
    if (!source_account) {
        return std::unexpected(source_account.error());
    }
    auto dest_account = access.materialize(*dest_handle);
    if (!dest_account) {
        return std::unexpected(dest_account.error());
    }

    if (source_account->balance < amount_) {
        context.logger().warn("insufficient funds");
        return std::unexpected(Error{ErrorCode::invalid_argument, "insufficient funds"});
    }

    source_account->balance -= amount_;
    dest_account->balance += amount_;

    if (auto status = access.update(*source_handle, *source_account); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = access.update(*dest_handle, *dest_account); !status) {
        return std::unexpected(status.error());
    }

    context.logger().info("transfer committed logically");
    storage::BinaryWriter payload;
    payload.write_u64(static_cast<std::uint64_t>(amount_));
    return ops::OperationResult{.payload = std::move(payload).take()};
}

Result<std::vector<std::byte>> MigrationBumpBalance::encode_args(
    const std::vector<object::ObjectId>& ids, std::int64_t delta) {
    if (ids.size() > 0xffffu) {
        return std::unexpected(make_args_error("too many migration targets"));
    }
    storage::BinaryWriter writer;
    writer.write_u16(static_cast<std::uint16_t>(ids.size()));
    writer.write_u64(static_cast<std::uint64_t>(delta));
    for (const auto id : ids) {
        writer.write_u64(id.value);
    }
    return std::move(writer).take();
}

Result<std::unique_ptr<ops::Operation>> MigrationBumpBalance::decode(
    std::span<const std::byte> args) {
    storage::BinaryReader reader{args};
    const auto count = reader.read_u16();
    if (!count) {
        return std::unexpected(count.error());
    }
    const auto delta_bits = reader.read_u64();
    if (!delta_bits) {
        return std::unexpected(delta_bits.error());
    }
    std::vector<object::ObjectId> ids;
    ids.reserve(*count);
    for (std::uint16_t i = 0; i < *count; ++i) {
        const auto id = reader.read_u64();
        if (!id) {
            return std::unexpected(id.error());
        }
        ids.push_back(object::ObjectId{*id});
    }
    if (!reader.at_end()) {
        return std::unexpected(make_args_error("MigrationBumpBalance trailing bytes"));
    }
    return std::unique_ptr<ops::Operation>{
        new MigrationBumpBalance{std::move(ids), static_cast<std::int64_t>(*delta_bits)}};
}

Result<ops::OperationResult> MigrationBumpBalance::execute(ops::ExecutionContext& context) {
    auto& access = context.objects();
    for (const auto id : ids_) {
        auto handle = access.get<Account>(id);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        auto account = access.materialize(*handle);
        if (!account) {
            return std::unexpected(account.error());
        }
        account->balance += delta_;
        if (auto status = access.update(*handle, *account); !status) {
            return std::unexpected(status.error());
        }
    }
    return ops::OperationResult{};
}

ops::ModuleManifest transfer_funds_manifest(object::BaselineId baseline) {
    ops::ModuleManifest manifest{
        .id = "transfer_funds",
        .module_version = 1,
        .baseline = baseline,
        .api_version = ops::runtime_api_version,
        .methods =
            {
                ops::ExportedMethod{.id = std::string{TransferFunds::k_id},
                                    .mode = TransferFunds::k_mode},
                ops::ExportedMethod{.id = std::string{ThrowingOperation::k_id},
                                    .mode = ThrowingOperation::k_mode},
                ops::ExportedMethod{.id = std::string{MigrationBumpBalance::k_id},
                                    .mode = MigrationBumpBalance::k_mode},
            },
    };
    manifest.hash = ops::compute_manifest_hash(manifest);
    return manifest;
}

Result<void> register_transfer_funds_module(ops::OperationRegistry& registry) {
    if (auto status = registry.register_operation<TransferFunds>(std::string{TransferFunds::k_id});
        !status) {
        return status;
    }
    if (auto status =
            registry.register_operation<ThrowingOperation>(std::string{ThrowingOperation::k_id});
        !status) {
        return status;
    }
    return registry.register_operation<MigrationBumpBalance>(
        std::string{MigrationBumpBalance::k_id});
}

} // namespace modb::examples
