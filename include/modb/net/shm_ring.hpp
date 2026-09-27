#pragma once

// Anel de memória compartilhada para OpCall/OpResult (ADR-026): o equivalente
// local do RDMA. Cliente e servidor na mesma máquina trocam mensagens por uma
// região mapeada pelos dois, sem syscall nem troca de thread por mensagem.
//
// Layout da região (little-endian; posições u64 alinhadas, uma por linha de cache):
//
//   0    magic "MDBSHM01" | ring_bytes u32 | reservado
//   64   req_tail   u64  cliente escreve: fim dos pedidos publicados
//   128  req_head   u64  servidor escreve: pedidos já consumidos
//   192  resp_tail  u64  servidor escreve: fim das respostas publicadas
//   256  resp_head  u64  cliente escreve: respostas já consumidas
//   320  client_state u32 (0 novo, 1 anexado, 2 saindo)
//   384  server_state u32 (0 novo, 1 servindo, 2 encerrado)
//   512  anel de pedidos   [ring_bytes]
//   512 + ring_bytes       anel de respostas [ring_bytes]
//
// Cada mensagem no anel é o frame do TCP (| length u32 | type u8 | payload |),
// começando numa posição múltipla de 8 e ocupando align8(4 + length) bytes.
// Uma mensagem nunca quebra na volta do anel: se não cabe até o fim, o
// produtor grava o marcador 0xFFFFFFFF e recomeça do início. As posições só
// crescem; o deslocamento é posição % ring_bytes. Publicar = gravar os bytes e
// depois a posição nova (release); consumir = ler a posição (acquire) e depois
// os bytes.

#include "modb/error.hpp"
#include "modb/net/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace modb::net::shm {

inline constexpr std::uint32_t k_header_bytes = 512;
inline constexpr std::uint32_t k_default_ring_bytes = 1u << 20;
inline constexpr std::uint32_t k_min_ring_bytes = 4096;
inline constexpr std::uint32_t k_max_ring_bytes = 64u << 20;
inline constexpr std::uint32_t k_padding_marker = 0xFFFFFFFFu;

inline constexpr std::size_t k_off_ring_bytes = 8;
inline constexpr std::size_t k_off_req_tail = 64;
inline constexpr std::size_t k_off_req_head = 128;
inline constexpr std::size_t k_off_resp_tail = 192;
inline constexpr std::size_t k_off_resp_head = 256;
inline constexpr std::size_t k_off_client_state = 320;
inline constexpr std::size_t k_off_server_state = 384;

enum class ClientState : std::uint32_t { fresh = 0, attached = 1, leaving = 2 };
enum class ServerState : std::uint32_t { fresh = 0, serving = 1, closed = 2 };

// Tamanho de anel aceito (múltiplo de 8, entre os limites); 0 = padrão.
[[nodiscard]] Result<std::uint32_t> normalize_ring_bytes(std::uint32_t requested);

// A região mapeada. O servidor cria (nome único); o cliente abre pelo nome que
// veio no ShmAttachOk.
class Region {
public:
    Region() = default;
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;
    Region(Region&& other) noexcept;
    Region& operator=(Region&& other) noexcept;
    ~Region();

    [[nodiscard]] static Result<Region> create(std::uint32_t ring_bytes);
    [[nodiscard]] static Result<Region> open(ShmRegionKind kind, std::string_view name, std::uint32_t ring_bytes);

    [[nodiscard]] std::byte* data() const noexcept { return base_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint32_t ring_bytes() const noexcept { return ring_bytes_; }
    [[nodiscard]] ShmRegionKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    // Tira o nome do sistema de arquivos (POSIX); o mapeamento continua válido.
    // No Windows o mapeamento nomeado some sozinho com o último handle.
    void unlink() noexcept;

    [[nodiscard]] std::uint32_t load_state(std::size_t offset) const noexcept;
    void store_state(std::size_t offset, std::uint32_t value) noexcept;

private:
    void release() noexcept;

    std::byte* base_{nullptr};
    std::size_t size_{0};
    std::uint32_t ring_bytes_{0};
    ShmRegionKind kind_{ShmRegionKind::file_path};
    std::string name_{};
    bool owner_{false};
    bool unlinked_{false};
    void* handle_{nullptr};  // HANDLE do mapeamento no Windows
    int fd_{-1};             // POSIX
};

// Um sentido do anel. O produtor só escreve `tail`; o consumidor só escreve `head`.
class Ring {
public:
    Ring() = default;
    Ring(std::byte* data, std::uint32_t ring_bytes, std::byte* tail, std::byte* head) noexcept
        : data_{data}, ring_bytes_{ring_bytes}, tail_{tail}, head_{head} {}

    // Anel de pedidos / de respostas de uma região.
    [[nodiscard]] static Ring requests(const Region& region) noexcept;
    [[nodiscard]] static Ring responses(const Region& region) noexcept;

    // Publica um frame inteiro. false = sem espaço agora (tente de novo);
    // erro = o frame nunca caberia neste anel.
    [[nodiscard]] Result<bool> try_write(std::span<const std::byte> frame);

    // O próximo frame, no lugar (válido até `pop`); nullopt = anel vazio.
    [[nodiscard]] Result<std::optional<std::span<const std::byte>>> peek();
    // Libera o frame devolvido pelo último `peek`.
    void pop() noexcept;

    [[nodiscard]] std::uint32_t capacity() const noexcept { return ring_bytes_; }

private:
    std::byte* data_{nullptr};
    std::uint32_t ring_bytes_{0};
    std::byte* tail_{nullptr};
    std::byte* head_{nullptr};
    std::uint64_t pending_{0};  // bytes do frame visto por peek
};

// Espera adaptativa, como o polling de uma fila de RDMA: gira, depois cede a
// CPU, depois dorme em passos crescentes até `max_sleep`. A janela de `yield`
// é longa de propósito: dormir custa ~0,5-1 ms de latência para acordar (no
// Windows, mesmo com o timer de alta resolução), e com chamadas em sequência o
// intervalo entre uma e outra passa fácil dos 200 us -- medido com o
// modb_rpc_bench, janelas curtas derrubavam payloads de 64 KiB de ~29 mil para
// ~3 mil chamadas/s. Ocioso, o anel dorme depois de 5 ms.
class Backoff {
public:
    void wait();
    void reset() noexcept { started_ = false; }

    static constexpr auto k_spin = std::chrono::microseconds{100};
    static constexpr auto k_yield = std::chrono::microseconds{5000};
    static constexpr auto k_max_sleep = std::chrono::microseconds{1000};

private:
    bool started_{false};
    std::chrono::steady_clock::time_point since_{};
    std::chrono::microseconds sleep_{50};
};

} // namespace modb::net::shm
