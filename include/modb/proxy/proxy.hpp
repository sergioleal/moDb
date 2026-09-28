#pragma once

// Proxy de acesso remoto ao engine (ADR-028, PLANO_PROXY X5).
//
// Aceita clientes do protocolo do moDb em TCP, negocia o Hello com cada um,
// autentica e autoriza cada mensagem pela `Policy`, e repassa as permitidas ao
// engine por um link local (AF_UNIX) em que as sessões de todos os clientes
// são multiplexadas. As respostas voltam pelo mesmo caminho, com a compressão
// negociada com o cliente e crédito por stream devolvido ao engine à medida que
// o cliente consome: um cliente lento para só os streams dele.

#include "modb/error.hpp"
#include "modb/net/protocol.hpp"
#include "modb/proxy/policy.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace modb::proxy {

struct Options {
    // Socket local do engine (`Server::listen_local`).
    std::filesystem::path engine{};
    // Segredo do link (`Server::set_link_secret`); vazio = nenhum.
    std::string link_secret{};
    // Nome do proxy nos logs do engine.
    std::string name{"modb-proxy"};
    std::string host{"127.0.0.1"};
    // 0 = qualquer porta livre.
    std::uint16_t port{7474};
    std::uint32_t idle_timeout_ms{net::default_idle_timeout_ms};
    net::Compression preferred_codec{net::Compression::rle};
    // Frames que um stream pode ter a caminho do cliente antes de o proxy
    // entregá-los (o crédito inicial de cada stream no engine).
    std::uint32_t stream_credit{8};
    // Espera máxima entre tentativas de reabrir o link quando o engine cai.
    std::uint32_t reconnect_max_ms{5'000};
};

class Proxy {
public:
    // Abre o link com o engine e escuta os clientes. Falha se o engine não
    // estiver no ar: é erro de configuração, não queda.
    [[nodiscard]] static Result<Proxy> start(Options options, std::shared_ptr<Policy> policy);

    Proxy(Proxy&&) noexcept;
    Proxy& operator=(Proxy&&) noexcept;
    Proxy(const Proxy&) = delete;
    Proxy& operator=(const Proxy&) = delete;
    // Para e junta as threads.
    ~Proxy();

    [[nodiscard]] std::uint16_t port() const noexcept;
    // O link com o engine está aberto agora.
    [[nodiscard]] bool link_up() const noexcept;
    // Clientes com sessão aberta no engine.
    [[nodiscard]] std::size_t session_count() const noexcept;

    // Aceita clientes até `request_stop()`.
    [[nodiscard]] Result<void> serve_forever();
    // Fecha o listener, os clientes e o link (SIGINT/SIGTERM).
    void request_stop() noexcept;

    // Testes: janela TCP pequena para os clientes (backpressure).
    void use_small_socket_buffers(bool enabled) noexcept;

    struct Impl;

private:
    explicit Proxy(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace modb::proxy
