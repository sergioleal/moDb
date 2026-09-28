// Codec do link proxy ↔ engine (ADR-028, PLANO_PROXY X3): ida e volta de cada
// mensagem de controle e de mensagens do cliente, regra de sessão (controle do
// link só na sessão 0, o resto fora dela), entradas hostis e envio por socket.

#include "modb/net/link_protocol.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

using namespace modb;
using namespace modb::net;

namespace {

bool round_trips(const LinkFrame& frame) {
    auto bytes = encode_link_frame(frame);
    if (!bytes) {
        return false;
    }
    auto back = decode_link_frame(*bytes);
    return back && *back == frame;
}

} // namespace

int main() {
    TestSuite suite;

    // --- ida e volta ---
    suite.check(round_trips({.session = 0, .body = LinkControl{LinkHello{.proxy_name = "p1", .secret = "s3cr3t"}}}),
                "LinkHello ida e volta");
    suite.check(round_trips({.session = 0,
                             .body = LinkControl{LinkHelloOk{.baseline = object::BaselineId{42},
                                                             .database_name = "app.modb",
                                                             .max_concurrent_streams = 9}}}),
                "LinkHelloOk ida e volta");
    suite.check(round_trips({.session = 0,
                             .body = LinkControl{LinkHelloOk{.ok = false,
                                                             .code = ErrorCode::incompatible_protocol_version,
                                                             .message = "link v9"}}}),
                "LinkHelloOk de recusa ida e volta");
    suite.check(round_trips({.session = 7,
                             .body = LinkControl{SessionOpen{.client_minor = 2,
                                                             .principal = "ana",
                                                             .roles = {"leitor", "admin"},
                                                             .attributes = {{"ip", "10.0.0.1"}, {"tls", "1"}}}}}),
                "SessionOpen ida e volta");
    suite.check(round_trips({.session = 7, .body = LinkControl{SessionOpenOk{}}}), "SessionOpenOk ida e volta");
    suite.check(round_trips({.session = 7,
                             .body = LinkControl{SessionClose{.code = ErrorCode::protocol_error, .message = "x"}}}),
                "SessionClose ida e volta");
    suite.check(round_trips({.session = 0xFFFFFFFFu, .body = LinkControl{StreamCredit{.query_id = 3, .frames = 16}}}),
                "StreamCredit ida e volta (sessão máxima)");
    suite.check(round_trips({.session = 7,
                             .body = Message{OpCall{.call_id = 1,
                                                    .operation_id = "a.b",
                                                    .args = std::vector<std::byte>(300, std::byte{9})}}}),
                "OpCall de sessão ida e volta");
    suite.check(round_trips({.session = 7, .body = Message{Cancel{.query_id = 5}}}), "Cancel de sessão ida e volta");

    // --- o que vem depois do session é um frame do cliente sem mudança ---
    {
        const Message call = OpCall{.call_id = 9, .operation_id = "x.y"};
        auto link = encode_link_frame({.session = 3, .body = call});
        auto plain = encode_message(call);
        suite.check(link && plain && link->size() == plain->size() + 4 &&
                        std::equal(plain->begin(), plain->end(), link->begin() + 4),
                    "mensagem de sessão = session + frame do cliente");
    }

    // --- regra de sessão ---
    suite.check_error(encode_link_frame({.session = 0, .body = Message{Cancel{.query_id = 1}}}),
                      ErrorCode::protocol_error, "mensagem do cliente na sessão 0 é recusada");
    suite.check_error(encode_link_frame({.session = 4, .body = LinkControl{LinkHello{}}}), ErrorCode::protocol_error,
                      "LinkHello fora da sessão 0 é recusado");
    suite.check_error(encode_link_frame({.session = 0, .body = LinkControl{SessionOpen{}}}), ErrorCode::protocol_error,
                      "SessionOpen na sessão 0 é recusado");
    {
        // O decoder aplica a mesma regra a bytes feitos à mão.
        auto bytes = *encode_link_frame({.session = 4, .body = LinkControl{SessionOpenOk{}}});
        bytes[0] = std::byte{0};
        suite.check_error(decode_link_frame(bytes), ErrorCode::protocol_error,
                          "decoder recusa controle de sessão na sessão 0");
    }

    // --- entradas hostis ---
    {
        auto bytes = *encode_link_frame({.session = 2, .body = LinkControl{SessionClose{.message = "tchau"}}});
        suite.check_error(decode_link_frame(std::span<const std::byte>{bytes}.first(bytes.size() - 1)),
                          ErrorCode::unexpected_end_of_input, "frame truncado");
        auto extra = bytes;
        extra.push_back(std::byte{0});
        suite.check_error(decode_link_frame(extra), ErrorCode::trailing_data, "bytes sobrando");
        auto unknown = bytes;
        unknown[8] = std::byte{0xF0};
        suite.check_error(decode_link_frame(unknown), ErrorCode::protocol_error, "tipo de controle desconhecido");
        auto huge = bytes;
        huge[4] = huge[5] = huge[6] = huge[7] = std::byte{0xFF};
        suite.check_error(decode_link_frame(huge), ErrorCode::frame_too_large, "length gigante");
        suite.check_error(decode_link_frame(std::span<const std::byte>{bytes}.first(5)),
                          ErrorCode::unexpected_end_of_input, "cabeçalho incompleto");
    }
    {
        auto bytes = *encode_link_frame({.session = 2, .body = LinkControl{SessionOpenOk{}}});
        bytes[9] = std::byte{2};  // ok fora de {0, 1}
        suite.check_error(decode_link_frame(bytes), ErrorCode::protocol_error, "booleano inválido");
    }
    {
        SessionOpen many{.roles = std::vector<std::string>(max_link_roles + 1, "r")};
        suite.check_error(encode_link_frame({.session = 1, .body = LinkControl{many}}), ErrorCode::value_too_large,
                          "roles demais são recusadas na codificação");
    }

    // --- pelo socket local ---
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = std::filesystem::current_path() / ("modb-link-" + std::to_string(stamp) + ".sock");
        auto listener = NativeSocket::listen_local(path);
        suite.check(listener.has_value(), "listener do link");
        if (listener) {
            const LinkFrame hello{.session = 0, .body = LinkControl{LinkHello{.proxy_name = "t"}}};
            const LinkFrame call{.session = 5, .body = Message{OpCall{.call_id = 2, .operation_id = "q.r"}}};
            std::vector<LinkFrame> got;
            std::thread engine{[&] {
                auto peer = listener->accept();
                for (int i = 0; peer && i < 2; ++i) {
                    if (auto frame = recv_link_frame(*peer)) {
                        got.push_back(std::move(*frame));
                    }
                }
            }};
            auto proxy = NativeSocket::connect_local(path);
            suite.check(proxy && send_link_frame(*proxy, hello) && send_link_frame(*proxy, call),
                        "proxy envia pelo link");
            engine.join();
            suite.check(got.size() == 2 && got[0] == hello && got[1] == call, "engine recebe os frames na ordem");
        }
        NativeSocket::remove_local(path);
    }

    return suite.finish();
}
