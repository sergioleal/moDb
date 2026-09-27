// Anel de memória compartilhada (ADR-026): região, escrita/leitura no lugar,
// volta do anel com marcador, anel cheio, frame grande demais, corrupção, e as
// mensagens ShmAttach/ShmAttachOk.

#include "modb/net/protocol.hpp"
#include "modb/net/shm_ring.hpp"

#include "test_support.hpp"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace modb;
using namespace modb::net;

namespace {

std::vector<std::byte> frame_de(std::uint32_t call_id, std::size_t args_bytes) {
    OpCall call{.call_id = call_id, .operation_id = "x.y", .args = std::vector<std::byte>(args_bytes, std::byte{7})};
    return *encode_message(call);
}

std::uint32_t call_id_de(std::span<const std::byte> frame) {
    auto m = decode_message(frame);
    const auto* c = m ? std::get_if<OpCall>(&*m) : nullptr;
    return c != nullptr ? c->call_id : 0;
}

} // namespace

int main() {
    TestSuite suite;

    suite.check(shm::normalize_ring_bytes(0) == shm::k_default_ring_bytes, "0 = anel padrão");
    suite.check(!shm::normalize_ring_bytes(100), "anel pequeno demais é recusado");
    suite.check(!shm::normalize_ring_bytes(shm::k_max_ring_bytes + 8), "anel grande demais é recusado");

    auto servidor = shm::Region::create(4096);
    suite.check(servidor.has_value(), "o servidor cria a região");
    if (!servidor) {
        return suite.finish();
    }
    auto cliente = shm::Region::open(servidor->kind(), servidor->name(), servidor->ring_bytes());
    suite.check(cliente.has_value(), "o cliente abre a região pelo nome");
    if (!cliente) {
        return suite.finish();
    }
    suite.check(!shm::Region::open(servidor->kind(), servidor->name(), 8192),
                "abrir com o tamanho errado é recusado (cabeçalho confere)");
    suite.check(cliente->load_state(shm::k_off_server_state) == static_cast<std::uint32_t>(shm::ServerState::serving),
                "o cliente vê o estado gravado pelo servidor");

    auto produtor = shm::Ring::requests(*cliente);
    auto consumidor = shm::Ring::requests(*servidor);

    // --- ida e volta, no lugar ---
    suite.check(consumidor.peek() && !consumidor.peek()->has_value(), "anel novo está vazio");
    const auto f1 = frame_de(1, 10);
    suite.check(produtor.try_write(f1) == true, "escreve um frame");
    auto visto = consumidor.peek();
    suite.check(visto && visto->has_value() && (*visto)->size() == f1.size() &&
                    std::memcmp((*visto)->data(), f1.data(), f1.size()) == 0,
                "o consumidor vê o mesmo frame, byte a byte");
    suite.check(call_id_de(**visto) == 1, "o frame decodifica no lugar");
    consumidor.pop();
    suite.check(!consumidor.peek()->has_value(), "depois do pop o anel volta a ficar vazio");

    // --- volta do anel: muitas mensagens de tamanhos variados, em ordem ---
    bool ordem = true;
    std::uint32_t esperado = 100;
    for (std::uint32_t i = 100; i < 400; ++i) {
        const auto f = frame_de(i, (i * 37) % 900);
        while (produtor.try_write(f) != true) {
            auto v = consumidor.peek();
            if (!v || !v->has_value()) {
                ordem = false;
                break;
            }
            ordem = ordem && call_id_de(**v) == esperado++;
            consumidor.pop();
        }
    }
    for (auto v = consumidor.peek(); v && v->has_value(); v = consumidor.peek()) {
        ordem = ordem && call_id_de(**v) == esperado++;
        consumidor.pop();
    }
    suite.check(ordem && esperado == 400, "300 frames atravessam várias voltas do anel, em ordem e inteiros");

    // --- cheio e grande demais ---
    const auto grande = frame_de(9, 1500);
    suite.check(produtor.try_write(grande) == true && produtor.try_write(grande) == true,
                "cabem dois frames de 1,5 KiB");
    suite.check(produtor.try_write(grande) == false, "o terceiro não cabe: anel cheio (tente de novo)");
    auto enorme = produtor.try_write(frame_de(10, 5000));
    suite.check(!enorme && enorme.error().code == ErrorCode::value_too_large,
                "frame maior que o anel é erro, não espera eterna");
    while (consumidor.peek() && consumidor.peek()->has_value()) {
        consumidor.pop();
    }

    // --- entre threads ---
    {
        auto respostas_srv = shm::Ring::responses(*servidor);
        auto respostas_cli = shm::Ring::responses(*cliente);
        std::thread eco{[&] {
            shm::Backoff b;
            for (int n = 0; n < 2000;) {
                auto v = consumidor.peek();
                if (!v || !v->has_value()) {
                    b.wait();
                    continue;
                }
                b.reset();
                const std::vector<std::byte> copia((*v)->begin(), (*v)->end());
                consumidor.pop();
                while (respostas_srv.try_write(copia) != true) {
                }
                ++n;
            }
        }};
        bool ok = true;
        shm::Backoff b;
        for (std::uint32_t i = 1; i <= 2000; ++i) {
            while (produtor.try_write(frame_de(i, i % 300)) != true) {
            }
            for (;;) {
                auto v = respostas_cli.peek();
                if (v && v->has_value()) {
                    ok = ok && call_id_de(**v) == i;
                    respostas_cli.pop();
                    break;
                }
                b.wait();
            }
            b.reset();
        }
        eco.join();
        suite.check(ok, "2000 pedidos e respostas entre duas threads, sem perda nem troca");
    }

    // --- corrupção ---
    {
        // Um cliente que publica um frame e depois corrompe o comprimento dele.
        std::uint64_t head = 0;
        std::memcpy(&head, servidor->data() + shm::k_off_req_head, sizeof head);
        suite.check(produtor.try_write(frame_de(1, 1)) == true, "publica um frame para corromper");
        const std::uint32_t lixo = 0x7FFFFFFF;
        std::memcpy(servidor->data() + shm::k_header_bytes + head % servidor->ring_bytes(), &lixo, sizeof lixo);
        auto v = consumidor.peek();
        suite.check(!v && v.error().code == ErrorCode::protocol_error, "comprimento absurdo no anel é protocol_error");
    }

    // --- mensagens do protocolo ---
    {
        auto bytes = encode_message(ShmAttach{.request_id = 7, .ring_bytes = 65536});
        auto m = bytes ? decode_message(*bytes) : Result<Message>{std::unexpected(bytes.error())};
        suite.check(m && std::get_if<ShmAttach>(&*m) && std::get<ShmAttach>(*m).ring_bytes == 65536,
                    "ShmAttach ida e volta");
        const ShmAttachOk ok{.request_id = 7, .kind = ShmRegionKind::file_path, .name = "/dev/shm/x", .ring_bytes = 4096};
        auto b2 = encode_message(ok);
        auto m2 = b2 ? decode_message(*b2) : Result<Message>{std::unexpected(b2.error())};
        suite.check(m2 && std::get_if<ShmAttachOk>(&*m2) && std::get<ShmAttachOk>(*m2) == ok, "ShmAttachOk ida e volta");
        const ShmAttachOk falha{.request_id = 8, .ok = false, .code = ErrorCode::invalid_argument, .message = "não"};
        auto b3 = encode_message(falha);
        auto m3 = b3 ? decode_message(*b3) : Result<Message>{std::unexpected(b3.error())};
        suite.check(m3 && std::get_if<ShmAttachOk>(&*m3) && !std::get<ShmAttachOk>(*m3).ok &&
                        std::get<ShmAttachOk>(*m3).message == "não",
                    "ShmAttachOk com erro ida e volta");
    }

    return suite.finish();
}
