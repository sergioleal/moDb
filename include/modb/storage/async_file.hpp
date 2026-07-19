#pragma once

// AsyncFile: I/O por completion com fallback síncrono explícito (Fase 13B / ADR-013).

#include "modb/error.hpp"
#include "modb/storage/native_file.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace modb::storage {

enum class IoBackend : std::uint8_t {
    io_uring = 0,
    iocp = 1,
    sync_fallback = 2,
};

[[nodiscard]] constexpr std::string_view to_string(IoBackend backend) noexcept {
    switch (backend) {
    case IoBackend::io_uring:
        return "io_uring";
    case IoBackend::iocp:
        return "iocp";
    case IoBackend::sync_fallback:
        return "sync_fallback";
    }
    return "unknown";
}

struct AsyncFileOptions {
    // Máximo de operações enfileiradas antes de drain/barrier.
    std::size_t max_inflight{64};
    // Se true, open falha quando só há fallback síncrono.
    bool require_async{false};
};

// Abstração única de arquivo com submit/drain. No backend sync_fallback as
// operações são aplicadas em `drain`/`barrier` via NativeFile; o backend e a
// razão do fallback são observáveis.
class AsyncFile {
public:
    using Mode = NativeFile::Mode;

    AsyncFile() = default;
    AsyncFile(const AsyncFile&) = delete;
    AsyncFile& operator=(const AsyncFile&) = delete;
    AsyncFile(AsyncFile&&) noexcept;
    AsyncFile& operator=(AsyncFile&&) noexcept;
    ~AsyncFile();

    [[nodiscard]] static Result<AsyncFile> open(const std::filesystem::path& path, Mode mode,
                                                AsyncFileOptions options = {});

    // Enfileira escrita (copia o buffer). Excede max_inflight → invalid_argument.
    [[nodiscard]] Result<void> submit_write_at(std::uint64_t offset,
                                               std::span<const std::byte> source);
    // Enfileira leitura para `destination` (válido até drain).
    [[nodiscard]] Result<void> submit_read_at(std::uint64_t offset,
                                              std::span<std::byte> destination);
    // Enfileira flush durável (fsync / FlushFileBuffers).
    [[nodiscard]] Result<void> submit_sync();

    // Atalhos: submit + drain de uma operação (completion antes do retorno).
    [[nodiscard]] Result<void> write_at(std::uint64_t offset, std::span<const std::byte> source);
    [[nodiscard]] Result<void> read_at(std::uint64_t offset, std::span<std::byte> destination);
    [[nodiscard]] Result<void> sync();

    // Aplica todas as operações pendentes em ordem (barreira de completion).
    [[nodiscard]] Result<void> drain();
    // Alias de drain — WAL → flush → páginas deve chamar barrier entre fases.
    [[nodiscard]] Result<void> barrier() { return drain(); }

    // Descarta operações ainda não drenadas (no-op se fila vazia).
    [[nodiscard]] Result<void> cancel_all();

    [[nodiscard]] Result<void> close();

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] IoBackend backend() const noexcept { return backend_; }
    [[nodiscard]] std::string_view backend_name() const noexcept { return to_string(backend_); }
    [[nodiscard]] std::string_view fallback_reason() const noexcept { return fallback_reason_; }
    [[nodiscard]] std::size_t inflight() const noexcept { return queue_.size(); }
    [[nodiscard]] std::size_t max_inflight() const noexcept { return max_inflight_; }

private:
    enum class OpKind : std::uint8_t { read = 0, write = 1, sync = 2 };

    struct Op {
        OpKind kind{OpKind::sync};
        std::uint64_t offset{0};
        std::vector<std::byte> write_bytes{};
        std::byte* read_ptr{nullptr};
        std::size_t read_size{0};
    };

    [[nodiscard]] Result<void> enqueue(Op op);
    [[nodiscard]] Result<void> apply(const Op& op);

    NativeFile file_{};
    IoBackend backend_{IoBackend::sync_fallback};
    std::string fallback_reason_{};
    std::size_t max_inflight_{64};
    std::vector<Op> queue_{};
};

} // namespace modb::storage
