#pragma once

// Importa Result para reportar falhas de I/O sem exceções.
#include "modb/error.hpp"
#include "modb/storage/native_file.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace modb::storage {

// Backend efetivo usado por AsyncFile. Windows usa IOCP; Linux/POSIX usa AIO.
// Outras plataformas podem cair em fallback síncrono em implementação futura.
enum class IoBackend : std::uint8_t {
    iocp = 0,
    posix_aio = 1,
    sync_fallback = 2,
};

[[nodiscard]] constexpr std::string_view to_string(IoBackend backend) noexcept {
    switch (backend) {
    case IoBackend::iocp:
        return "iocp";
    case IoBackend::posix_aio:
        return "posix_aio";
    case IoBackend::sync_fallback:
        return "sync_fallback";
    }
    return "unknown";
}

struct AsyncFileOptions {
    // Máximo de operações pendentes aceitas entre drains/barriers.
    std::size_t max_inflight{64};
    // Se true, open falha quando só há fallback síncrono.
    bool require_async{false};
};

// I/O posicional assíncrono com barreiras explícitas. Operações submetidas antes
// de um sync podem ser executadas em paralelo pelo backend; sync/barrier preserva
// a ordem de durabilidade esperada pelo WAL.
class AsyncFile {
public:
    using Mode = NativeFile::Mode;

    AsyncFile() noexcept = default;
    AsyncFile(const AsyncFile&) = delete;
    AsyncFile& operator=(const AsyncFile&) = delete;
    AsyncFile(AsyncFile&&) noexcept;
    AsyncFile& operator=(AsyncFile&&) noexcept;
    ~AsyncFile();

    [[nodiscard]] static Result<AsyncFile> open(const std::filesystem::path& path, Mode mode,
                                                AsyncFileOptions options = {});

    [[nodiscard]] Result<void> submit_write_at(std::uint64_t offset,
                                               std::span<const std::byte> source);
    [[nodiscard]] Result<void> submit_read_at(std::uint64_t offset,
                                              std::span<std::byte> destination);
    [[nodiscard]] Result<void> submit_sync();

    [[nodiscard]] Result<void> write_at(std::uint64_t offset, std::span<const std::byte> source);
    [[nodiscard]] Result<void> read_at(std::uint64_t offset, std::span<std::byte> destination);
    [[nodiscard]] Result<void> sync();

    [[nodiscard]] Result<void> drain();
    [[nodiscard]] Result<void> barrier() { return drain(); }
    [[nodiscard]] Result<void> cancel_all();
    [[nodiscard]] Result<void> close();

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] IoBackend backend() const noexcept;
    [[nodiscard]] std::string_view backend_name() const noexcept { return to_string(backend()); }
    [[nodiscard]] std::string_view fallback_reason() const noexcept;
    [[nodiscard]] std::size_t inflight() const noexcept;
    [[nodiscard]] std::size_t max_inflight() const noexcept;

private:
    class Impl;

    explicit AsyncFile(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_{};
};

} // namespace modb::storage
