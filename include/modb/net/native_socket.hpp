#pragma once

// Socket TCP nativo Win32/POSIX, no mesmo padrão de NativeFile (Fase 8B), e
// socket local AF_UNIX para o link proxy ↔ engine (ADR-028).

#include "modb/error.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
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

    // Socket local (AF_UNIX; no Windows, 10 1803+). `listen_local` apaga um
    // socket velho no caminho antes do bind e, no POSIX, deixa o arquivo com
    // permissão 0600 (só o dono do processo conecta). Quem escuta apaga o
    // arquivo quando termina (`remove_local`).
    [[nodiscard]] static Result<NativeSocket> listen_local(const std::filesystem::path& path,
                                                           int backlog = 16);
    [[nodiscard]] static Result<NativeSocket> connect_local(const std::filesystem::path& path);
    static void remove_local(const std::filesystem::path& path) noexcept;

    [[nodiscard]] Result<void> send_all(std::span<const std::byte> bytes);
    [[nodiscard]] Result<void> recv_exact(std::span<std::byte> destination);

    // Ajusta SO_SNDBUF/SO_RCVBUF (Fase 8D: janela TCP pequena nos testes).
    [[nodiscard]] Result<void> set_send_buffer_bytes(std::size_t bytes);
    [[nodiscard]] Result<void> set_recv_buffer_bytes(std::size_t bytes);

    // Timeout de recv (Fase 8F); 0 = bloqueante sem limite.
    [[nodiscard]] Result<void> set_recv_timeout_ms(std::uint32_t milliseconds);

    [[nodiscard]] Result<std::uint16_t> local_port() const;
    // "a.b.c.d:porta" do outro lado de uma conexão TCP; "local" num socket AF_UNIX.
    [[nodiscard]] Result<std::string> peer_address() const;
    [[nodiscard]] Result<void> close();
    // Encerra envio e recebimento sem fechar o descritor: uma recv/send
    // bloqueada em OUTRA thread acorda com connection_closed. É o que o
    // servidor usa para desligar sessões ociosas na parada (S5.5).
    [[nodiscard]] Result<void> shutdown() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

private:
#ifdef _WIN32
    explicit NativeSocket(std::uintptr_t socket) noexcept : socket_{socket} {}
    // O descritor é lido por uma thread (recv, shutdown) enquanto outra pode
    // fechá-lo: acesso atômico (C11; a corrida apareceu no TSan, C4).
    [[nodiscard]] std::uintptr_t raw() const noexcept {
        return std::atomic_ref<std::uintptr_t>{const_cast<std::uintptr_t&>(socket_)}.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uintptr_t take() noexcept {
        return std::atomic_ref<std::uintptr_t>{socket_}.exchange(static_cast<std::uintptr_t>(-1),
                                                                 std::memory_order_acq_rel);
    }
    alignas(8) std::uintptr_t socket_ = static_cast<std::uintptr_t>(-1);
#else
    explicit NativeSocket(int fd) noexcept : fd_{fd} {}
    // Ver a versão Windows: acesso atômico ao descritor.
    [[nodiscard]] int raw() const noexcept {
        return std::atomic_ref<int>{const_cast<int&>(fd_)}.load(std::memory_order_acquire);
    }
    [[nodiscard]] int take() noexcept { return std::atomic_ref<int>{fd_}.exchange(-1, std::memory_order_acq_rel); }
    alignas(4) int fd_ = -1;
    // Só preenchido para o socket de ESCUTA (por `listen()`): truque do
    // "self-pipe" para `accept()` acordar de forma bem definida quando
    // `close()` é chamado de outra thread. Fechar o fd de escuta enquanto
    // uma thread está bloqueada dentro do `::accept()` propriamente dito é
    // comportamento não especificado em POSIX (o fd pode até ser reaproveitado
    // por um `open()`/`socket()` concorrente antes da thread acordar) --
    // `accept()` primeiro espera em `poll()` por este pipe OU pelo socket,
    // nunca bloqueia dentro do `::accept()` de verdade sem antes saber que
    // há uma conexão pronta.
    int stop_pipe_read_ = -1;
    int stop_pipe_write_ = -1;
    void close_stop_pipe() noexcept;
#endif
};

} // namespace modb::net
