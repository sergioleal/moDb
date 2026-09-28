#pragma once

// Servidor TCP das Fases 8B–8F: sessão com leitor ativo, Cancel, multiplexação,
// backpressure, limites/timeout e compressão negociada (RLE com fallback none).

#include "modb/error.hpp"
#include "modb/net/native_socket.hpp"
#include "modb/net/shm_ring.hpp"
#include "modb/net/protocol.hpp"
#include "modb/ops/facade_catalog.hpp"
#include "modb/ops/operation_registry.hpp"
#include "modb/query/operators.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <atomic>

namespace modb::net {

struct EngineServices;

// Limite de objetos no frame em trânsito (fila de saída da Fase 8D).
inline constexpr std::size_t max_in_flight_objects = 8;

[[nodiscard]] Result<void> send_message(NativeSocket& socket, const Message& message);
[[nodiscard]] Result<Message> recv_message(NativeSocket& socket);
[[nodiscard]] Result<Message> recv_message(NativeSocket& socket, std::uint32_t negotiated_max_frame,
                                           std::uint16_t max_expansion_ratio);

// Negocia o Hello de um cliente: major/minor, e o codec (`preferred` se o
// cliente o aceita, senão none). `offer` traz baseline e limites; volta com a
// versão e o codec escolhidos. Usado pelo servidor direto e pelo proxy.
[[nodiscard]] Result<HelloOk> negotiate_hello(const Hello& hello, HelloOk offer, Compression preferred);

// Contadores do último fluxo (produzidos − enviados = objetos na fila local).
struct StreamStats {
    std::uint64_t produced{0};
    std::uint64_t sent{0};
    std::uint64_t max_outstanding{0};
};

class Server {
public:
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) noexcept;
    Server& operator=(Server&&) = delete;
    ~Server();

    // Abre (ou cria) o banco e escuta em TCP: o modo direto, sem proxy.
    [[nodiscard]] static Result<Server> listen(const std::filesystem::path& path,
                                               std::string_view host = "127.0.0.1",
                                               std::uint16_t port = 0);
    // Abre (ou cria) o banco sem escutar nada; depois `listen_tcp` e/ou
    // `listen_local` (ADR-028: o engine atrás de proxies escuta só local).
    [[nodiscard]] static Result<Server> open(const std::filesystem::path& path);

    [[nodiscard]] Result<void> listen_tcp(std::string_view host, std::uint16_t port);
    // Link dos proxies num socket AF_UNIX (ADR-028). O arquivo sai no destrutor.
    [[nodiscard]] Result<void> listen_local(const std::filesystem::path& socket_path);

    // 0 = sem TCP.
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    // Vazio = sem link local.
    [[nodiscard]] const std::filesystem::path& local_path() const noexcept { return local_path_; }
    [[nodiscard]] std::string_view database_name() const noexcept { return database_name_; }
    [[nodiscard]] object::BaselineId baseline() const noexcept { return baseline_; }
    [[nodiscard]] StreamStats last_stream_stats() const noexcept;
    [[nodiscard]] object::Database& database() noexcept { return *database_; }
    [[nodiscard]] const object::Database& database() const noexcept { return *database_; }
    [[nodiscard]] std::size_t open_snapshot_count() const noexcept {
        return database_ ? database_->open_snapshot_count() : 0;
    }
    [[nodiscard]] Compression selected_codec() const noexcept {
        return selected_codec_.load(std::memory_order_relaxed);
    }

    void fail_stream_after(std::size_t objects) noexcept { fail_after_ = objects; }
    void use_small_socket_buffers(bool enabled) noexcept { small_buffers_ = enabled; }
    void set_max_concurrent_streams(std::uint16_t limit) noexcept {
        max_concurrent_streams_ = limit == 0 ? 1 : limit;
    }
    void set_idle_timeout_ms(std::uint32_t milliseconds) noexcept {
        idle_timeout_ms_ = milliseconds;
    }
    void set_preferred_codec(Compression codec) noexcept { preferred_codec_ = codec; }
    void set_operation_registry(std::shared_ptr<ops::OperationRegistry> registry) noexcept {
        operations_ = std::move(registry);
    }
    void set_facade_catalog(std::shared_ptr<ops::FacadeCatalog> catalog) noexcept {
        facades_ = std::move(catalog);
    }
    // Segredo que o proxy manda no LinkHello (vazio = nenhum, só a permissão
    // do socket protege o link).
    void set_link_secret(std::string secret) { link_secret_ = std::move(secret); }
    // Threads que executam as mensagens das sessões de um link (0 = núcleos da máquina).
    void set_link_workers(std::size_t workers) noexcept { link_workers_ = workers; }

    // Aceita uma conexão e mantém a sessão até o peer fechar (Hello + Queries/OpCalls).
    [[nodiscard]] Result<void> serve_one();

    // Aceita um link de proxy e o atende até ele fechar.
    [[nodiscard]] Result<void> serve_one_link();

    // Aceita sessões TCP e links em loop até `request_stop()` ou falha de um listener.
    [[nodiscard]] Result<void> serve_forever();

    // Fecha o listener para despertar `accept` e encerrar o loop (SIGINT/SIGTERM).
    void request_stop() noexcept;
    [[nodiscard]] bool stop_requested() const noexcept { return stop_requested_.load(); }

private:
    Server(std::shared_ptr<object::Database> database, object::DatabaseId database_id,
           std::string database_name, object::BaselineId baseline);

    [[nodiscard]] Result<void> handle_connection(NativeSocket& peer);
    // Um link de proxy: muitas sessões multiplexadas (engine_link.cpp).
    [[nodiscard]] Result<void> handle_link(NativeSocket& peer);
    // Laço de accept de um listener; `link` escolhe o tratamento da conexão.
    [[nodiscard]] Result<void> accept_loop(NativeSocket& listener, bool link);
    // O que uma sessão nova usa do engine (configuração corrente do servidor).
    [[nodiscard]] EngineServices session_services();

    std::shared_ptr<object::Database> database_;
    object::DatabaseId database_id_{};
    NativeSocket listener_;
    std::uint16_t port_{0};
    NativeSocket local_listener_;
    std::filesystem::path local_path_{};
    std::string link_secret_{};
    std::size_t link_workers_{0};
    std::string database_name_;
    object::BaselineId baseline_{};
    std::optional<std::size_t> fail_after_{};
    bool small_buffers_{false};
    std::uint16_t max_concurrent_streams_{default_max_concurrent_streams};
    std::uint32_t idle_timeout_ms_{default_idle_timeout_ms};
    Compression preferred_codec_{Compression::rle};
    std::atomic<Compression> selected_codec_{Compression::none};
    StreamStats last_stats_{};
    mutable std::unique_ptr<std::mutex> stats_mutex_{std::make_unique<std::mutex>()};
    std::shared_ptr<ops::OperationRegistry> operations_{};
    std::shared_ptr<ops::FacadeCatalog> facades_{};
    std::atomic<bool> stop_requested_{false};
    // Sessões abertas em serve_forever: request_stop dá shutdown em cada uma,
    // acordando a leitura bloqueada, em vez de esperar o idle timeout (S5.5).
    struct ActiveSessions {
        std::mutex mu;
        std::uint64_t next_id{0};
        std::unordered_map<std::uint64_t, NativeSocket*> sockets;
    };
    std::unique_ptr<ActiveSessions> active_{std::make_unique<ActiveSessions>()};
};

} // namespace modb::net
