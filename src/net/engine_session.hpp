#pragma once

// Sessão do engine independente do transporte (ADR-028, PLANO_PROXY X1).
//
// Uma `EngineSession` é o estado de um cliente dentro do engine: streams de
// consulta em andamento (com seus tokens de Cancel), o anel de memória
// compartilhada, se houver, e a execução de OpCall/Facade*. Ela não lê de
// socket nenhum: quem a alimenta (o modo TCP direto hoje, o link do proxy
// depois) entrega cada mensagem já decodificada, e as respostas saem por um
// `SessionSink`.

#include "modb/error.hpp"
#include "modb/net/protocol.hpp"
#include "modb/net/server.hpp"
#include "modb/net/shm_ring.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/caller.hpp"
#include "modb/ops/facade_catalog.hpp"
#include "modb/ops/operation_registry.hpp"
#include "modb/query/operators.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace modb::net {

// Saída de uma sessão. `send` é chamado de várias threads (a da sessão e os
// workers de consulta) e pode bloquear: é por ele que chega o backpressure.
class SessionSink {
public:
    SessionSink() = default;
    SessionSink(const SessionSink&) = delete;
    SessionSink& operator=(const SessionSink&) = delete;
    virtual ~SessionSink() = default;

    [[nodiscard]] virtual Result<void> send(const Message& message) = 0;

    // Frames de um stream de consulta. No socket é um `send` comum (a janela
    // TCP faz o backpressure); no link espera crédito do stream (X4).
    [[nodiscard]] virtual Result<void> send_stream_frame(const ObjectFrame& frame) { return send(frame); }

    // O stream acabou (StreamEnd/StreamError enviado ou sessão encerrada):
    // o link descarta o crédito que sobrou.
    virtual void stream_closed(std::uint32_t /*query_id*/) noexcept {}
};

// Saída direta num socket: um mutex serializa os frames de threads diferentes.
class SocketSink final : public SessionSink {
public:
    explicit SocketSink(NativeSocket& socket) noexcept : socket_{&socket} {}

    [[nodiscard]] Result<void> send(const Message& message) override;

private:
    NativeSocket* socket_;
    std::mutex write_mu_;
};

// O que a sessão usa do engine. Montado pelo servidor na abertura da sessão
// (a configuração do `Server` pode mudar entre sessões, não durante uma).
struct EngineServices {
    std::shared_ptr<object::Database> database{};
    std::shared_ptr<ops::OperationRegistry> operations{};
    std::shared_ptr<ops::FacadeCatalog> facades{};
    std::optional<std::size_t> fail_after{};
    std::uint16_t max_concurrent_streams{default_max_concurrent_streams};
    // Anel de memória compartilhada (ADR-026) só no modo direto: atrás de um
    // proxy, quem atende o anel é o proxy.
    bool allow_shm{true};
    // Chamado ao fim de cada stream com os contadores dele.
    std::function<void(const StreamStats&)> on_stream_stats{};
    // Parada do servidor inteiro (o anel shm a observa).
    const std::atomic<bool>* server_stop{nullptr};
    // Quem é o cliente da sessão (dado pelo proxy no SessionOpen); anônimo no modo direto.
    ops::Caller caller{};
};

// Executa um OpCall no engine e monta a resposta.
[[nodiscard]] OpResult execute_op_call(const EngineServices& services, const OpCall& call);

class EngineSession {
public:
    EngineSession(EngineServices services, SessionSink& sink, Compression codec);
    EngineSession(const EngineSession&) = delete;
    EngineSession& operator=(const EngineSession&) = delete;
    // Chama `finish()`.
    ~EngineSession();

    // Trata na hora as mensagens que não podem esperar a fila da sessão
    // (Cancel de um stream em andamento). Devolve true se consumiu a mensagem.
    // Pode ser chamado da thread leitora enquanto `handle` roda em outra.
    [[nodiscard]] bool handle_urgent(const Message& message);

    // Trata uma mensagem do cliente: Query abre um worker; OpCall e Facade*
    // executam aqui e respondem pelo sink. Erro = a sessão deve acabar (falha
    // do sink ou mensagem que não cabe numa sessão).
    [[nodiscard]] Result<void> handle(const Message& message);

    // Cancela os streams, para o anel e junta as threads. Idempotente.
    void finish();

private:
    void run_query(const Query& query);
    [[nodiscard]] Result<void> attach_shm(const ShmAttach& attach);
    void serve_shm(shm::Region& region);
    void forget_token(std::uint32_t query_id);

    EngineServices services_;
    SessionSink* sink_;
    Compression codec_;
    std::atomic<bool> stop_{false};

    std::mutex tokens_mu_;
    std::unordered_map<std::uint32_t, query::CancellationToken> tokens_;

    std::mutex workers_mu_;
    std::vector<std::thread> workers_;
    std::atomic<int> live_workers_{0};

    std::unique_ptr<shm::Region> shm_region_;
    std::thread shm_thread_;
    bool finished_{false};
};

} // namespace modb::net
