#pragma once

// FailpointFile (Fase 5, PROTOCOLO_FASES §Failpoints): decorator de teste sobre
// o sink do WAL que encaminha para um sink real até `writes_before_failure`
// escritas e, daí em diante, faz toda escrita falhar com io_error — simulando o
// processo morto no meio da gravação do log.

// Importa a interface WalSink e a fábrica nativa.
#include "modb/tx/wal.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <utility>

namespace modb::test {

// Sink que falha após um número fixo de escritas.
class FailpointWalSink final : public modb::tx::WalSink {
public:
    FailpointWalSink(std::unique_ptr<modb::tx::WalSink> inner, std::size_t writes_before_failure)
        : inner_{std::move(inner)}, remaining_{writes_before_failure} {}

    modb::Result<void> write_at(std::uint64_t offset,
                                std::span<const std::byte> source) override {
        if (remaining_ == 0) {
            return std::unexpected(modb::Error{modb::ErrorCode::io_error,
                                               "failpoint: simulated WAL write failure"});
        }
        --remaining_;
        return inner_->write_at(offset, source);
    }

    modb::Result<void> sync() override { return inner_->sync(); }

private:
    std::unique_ptr<modb::tx::WalSink> inner_;
    std::size_t remaining_;
};

// Fábrica que embrulha o sink nativo num FailpointWalSink. `writes_before_failure`
// conta as escritas do WAL permitidas (a 1ª é o cabeçalho) antes de a próxima
// falhar.
inline modb::tx::WalFileFactory failing_wal_factory(std::size_t writes_before_failure) {
    return [writes_before_failure](const std::filesystem::path& path)
               -> modb::Result<std::unique_ptr<modb::tx::WalSink>> {
        auto native = modb::tx::open_native_wal_sink(path);
        if (!native) {
            return std::unexpected(native.error());
        }
        return std::make_unique<FailpointWalSink>(std::move(*native), writes_before_failure);
    };
}

} // namespace modb::test
