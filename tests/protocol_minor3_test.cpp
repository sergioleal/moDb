// Codec do protocolo minor 3 (ADR-029, PLANO_PROPOSTAS_REGISTRY R8): a extensão
// de delegação e chave de idempotência nos pedidos e o `detail` no OpResult de
// erro. Sem campo preenchido, os bytes são os do minor 2; com, só um decoder de
// minor 3 os aceita, e a negociação garante que só ele os recebe.

#include "modb/net/protocol.hpp"
#include "modb/net/server.hpp"

#include "test_support.hpp"

#include <cstddef>
#include <string>
#include <vector>

using namespace modb;
using namespace modb::net;

namespace {

template <typename T>
Result<T> round_trip(const T& message) {
    auto bytes = encode_message(message);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto decoded = decode_message(*bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    if (const auto* typed = std::get_if<T>(&*decoded)) {
        return *typed;
    }
    return std::unexpected(Error{ErrorCode::protocol_error, "decoded to another message type"});
}

std::size_t encoded_size(const Message& message) {
    auto bytes = encode_message(message);
    return bytes ? bytes->size() : 0;
}

// Um OpCall como o minor 2 o codifica: | len | 9 | call_id | proc | args_len | args |.
std::vector<std::byte> minor2_op_call(std::uint32_t call_id, std::string_view proc) {
    std::vector<std::byte> payload;
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            payload.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
        }
    };
    u32(call_id);
    u32(static_cast<std::uint32_t>(proc.size()));
    for (char c : proc) {
        payload.push_back(static_cast<std::byte>(c));
    }
    u32(0);
    std::vector<std::byte> frame;
    const auto length = static_cast<std::uint32_t>(payload.size() + 1);
    for (int i = 0; i < 4; ++i) {
        frame.push_back(static_cast<std::byte>((length >> (8 * i)) & 0xFF));
    }
    frame.push_back(static_cast<std::byte>(MessageType::op_call));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

} // namespace

int main() {
    TestSuite suite;
    suite.check(protocol_minor == 3, "protocolo no minor 3");

    const Delegation user{.subject = "user:24", .attributes = {{"email", "ana@exemplo.org"}, {"tenant", "t1"}}};

    // --- OpCall ---
    {
        auto full = round_trip(OpCall{.call_id = 7,
                                      .operation_id = "agents.create",
                                      .args = {std::byte{1}, std::byte{0}},
                                      .acting_as = user,
                                      .idempotency_key = "9f1c2b7e-0000-4000-8000-000000000001"});
        suite.check(full && full->acting_as == user && full->idempotency_key == "9f1c2b7e-0000-4000-8000-000000000001" &&
                        full->operation_id == "agents.create" && full->args.size() == 2,
                    "OpCall leva delegação e chave de ida e volta");
        auto only_key = round_trip(OpCall{.call_id = 8, .operation_id = "x", .idempotency_key = "k"});
        suite.check(only_key && only_key->acting_as.empty() && only_key->idempotency_key == "k",
                    "OpCall com só a chave");

        const OpCall plain{.call_id = 3, .operation_id = "notas.listar"};
        const auto old = minor2_op_call(3, "notas.listar");
        suite.check(encoded_size(plain) == old.size(), "OpCall sem extensão tem os bytes do minor 2");
        auto decoded = decode_message(old);
        const auto* call = decoded ? std::get_if<OpCall>(&*decoded) : nullptr;
        suite.check(call != nullptr && call->acting_as.empty() && call->idempotency_key.empty(),
                    "um OpCall do minor 2 decodifica sem delegação nem chave");

        auto bad = old;
        bad.push_back(std::byte{0x04});  // bit desconhecido
        bad[0] = static_cast<std::byte>(static_cast<std::uint8_t>(bad[0]) + 1);
        auto refused = decode_message(bad);
        suite.check(!refused && refused.error().code == ErrorCode::protocol_error, "bit desconhecido na extensão é recusado");

        auto too_long = encode_message(OpCall{.operation_id = "x", .idempotency_key = std::string(129, 'k')});
        suite.check(!too_long && too_long.error().code == ErrorCode::value_too_large, "chave acima de 128 bytes é recusada");
        auto no_subject = encode_message(OpCall{.operation_id = "x", .acting_as = Delegation{.attributes = {{"a", "b"}}}});
        suite.check(!no_subject, "atributos de delegação sem delegado são recusados");
    }

    // --- Query, FacadeList, FacadeOpen ---
    {
        auto query = round_trip(Query{.query_id = 4, .description = QueryDescription{.limit = 10}, .acting_as = user});
        suite.check(query && query->acting_as == user && query->description.limit == 10, "Query leva a delegação");
        auto list = round_trip(FacadeList{.request_id = 5, .acting_as = user});
        suite.check(list && list->acting_as == user, "FacadeList leva a delegação");
        auto open = round_trip(FacadeOpen{.request_id = 6, .facade_id = "accounts", .facade_version = 2, .acting_as = user});
        suite.check(open && open->acting_as == user && open->facade_id == "accounts" && open->facade_version == 2,
                    "FacadeOpen leva a delegação");
        suite.check(encoded_size(FacadeList{.request_id = 5}) == 4 + 1 + 4, "FacadeList sem extensão tem os bytes do minor 2");

        const Message with = OpCall{.acting_as = user};
        const Message without = OpCall{};
        const Message other = StreamEnd{};
        suite.check(request_delegation(with) != nullptr && request_delegation(with)->subject == "user:24" &&
                        request_delegation(without) == nullptr && request_delegation(other) == nullptr,
                    "request_delegation acha a delegação de um pedido");
    }

    // --- OpResult.detail ---
    {
        const std::vector<std::byte> detail{std::byte{1}, std::byte{7}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
        auto failed = round_trip(OpResult{.call_id = 9, .ok = false, .code = ErrorCode::conflict, .message = "já existe",
                                          .detail = detail});
        suite.check(failed && !failed->ok && failed->code == ErrorCode::conflict && failed->message == "já existe" &&
                        failed->detail == detail,
                    "OpResult de erro leva o detail de ida e volta");
        const OpResult plain{.call_id = 9, .ok = false, .code = ErrorCode::conflict, .message = "já existe"};
        const OpResult with_detail{.call_id = 9, .ok = false, .code = ErrorCode::conflict, .message = "já existe",
                                   .detail = detail};
        suite.check(encoded_size(with_detail) == encoded_size(plain) + 4 + detail.size(),
                    "sem detail, o OpResult de erro tem os bytes do minor 2");
        auto ok = round_trip(OpResult{.call_id = 1, .ok = true, .payload = {std::byte{1}, std::byte{0}}, .detail = detail});
        suite.check(ok && ok->ok && ok->detail.empty(), "num sucesso o detail não vai no fio");
    }

    // --- negociação ---
    {
        auto old_client = negotiate_hello(Hello{.minor = 2}, HelloOk{}, Compression::none);
        suite.check(old_client && old_client->minor == 2, "cliente de minor 2 negocia minor 2 com o servidor de minor 3");
        auto new_client = negotiate_hello(Hello{.minor = 3}, HelloOk{}, Compression::none);
        suite.check(new_client && new_client->minor == 3, "cliente de minor 3 negocia minor 3");
    }

    return suite.finish();
}
