#pragma once

// Socket TCP nativo Win32/POSIX, no mesmo padrão de NativeFile (Fase 8B).

#include "modb/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace modb::net {

class NativeSocket {
public:
    NativeSocket() noexcept = default;
    NativeSocket(const NativeSocket&) = delete;
    NativeSocket& operator=(const NativeSocket&) = delete;
    NativeSocket(NativeSocket&& other) noexcept;
    NativeSocket& operator=(NativeSocket&& other) noexcept;
    ~NativeSocket();

    [[nodiscard]] static Result<NativeSocket> listen(std::string_view host, std::uint16_t port,
                                                     int backlog = 16);
    [[nodiscard]] static Result<NativeSocket> connect(std::string_view host, std::uint16_t port);
    [[nodiscard]] Result<NativeSocket> accept();

    [[nodiscard]] Result<void> send_all(std::span<const std::byte> bytes);
    [[nodiscard]] Result<void> recv_exact(std::span<std::byte> destination);

    // Ajusta SO_SNDBUF/SO_RCVBUF (Fase 8D: janela TCP pequena nos testes).
    [[nodiscard]] Result<void> set_send_buffer_bytes(std::size_t bytes);
    [[nodiscard]] Result<void> set_recv_buffer_bytes(std::size_t bytes);

    // Timeout de recv (Fase 8F); 0 = bloqueante sem limite.
    [[nodiscard]] Result<void> set_recv_timeout_ms(std::uint32_t milliseconds);

    [[nodiscard]] Result<std::uint16_t> local_port() const;
    [[nodiscard]] Result<void> close();
    [[nodiscard]] bool is_open() const noexcept;

private:
#ifdef _WIN32
    explicit NativeSocket(std::uintptr_t socket) noexcept : socket_{socket} {}
    std::uintptr_t socket_ = static_cast<std::uintptr_t>(-1);
#else
    explicit NativeSocket(int fd) noexcept : fd_{fd} {}
    int fd_ = -1;
#endif
};

} // namespace modb::net
