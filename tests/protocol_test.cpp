#include "modb/error.hpp"
#include "modb/net/protocol.hpp"
#include "modb/net/query_description.hpp"
#include "modb/object/attribute_value.hpp"
#include "modb/storage/binary.hpp"

#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

using modb::ErrorCode;
using modb::net::Cancel;
using modb::net::Compression;
using modb::net::FacadeList;
using modb::net::FacadeListOk;
using modb::net::FacadeOpen;
using modb::net::FacadeOpenOk;
using modb::net::Hello;
using modb::net::HelloOk;
using modb::net::Message;
using modb::net::ObjectEnvelope;
using modb::net::ObjectFrame;
using modb::net::OpCall;
using modb::net::OpResult;
using modb::net::Query;
using modb::net::QueryDescription;
using modb::net::StreamBegin;
using modb::net::StreamEnd;
using modb::net::StreamError;
using modb::net::max_frame_bytes;
using modb::object::AttributeValue;
using modb::object::FieldId;
using modb::object::ObjectId;
using modb::object::TypeDefinitionId;
using modb::storage::BinaryWriter;

// Um frame válido é [u32 length][u8 type][payload...], length = 1 + |payload|.
// Cortar o corpo em QUALQUER prefixo estrito (ajustando só o length externo
// para casar com o novo tamanho) precisa sempre falhar: os campos internos
// (contagens, tamanhos de string, sub-mensagens) continuam com os valores
// ORIGINAIS, então um corte cedo demais necessariamente deixa um campo sem
// bytes suficientes. Isso exercita, de um só golpe, praticamente todo branch
// "unexpected_end_of_input"/"X.error()" de cada decoder de mensagem — sem
// precisar montar à mão um frame hostil por campo.
// `tolerated_tail` bytes at the very end of the body are allowed to be
// missing without an error: Hello/HelloOk end with an "aditivo" `minor` u16
// (Fase 10E) that older peers never send, and the decoder deliberately skips
// it (and any further trailing bytes) instead of failing — that is real,
// intentional forward-compatibility, not a bug. Everything strictly before
// that tolerated tail must still fail to decode.
void check_all_truncations_rejected(TestSuite& suite, const Message& original,
                                    std::string_view label, std::size_t tolerated_tail = 0) {
    auto encoded = modb::net::encode_message(original);
    if (!encoded || encoded->size() <= 5) {
        return;  // sem encode ou sem corpo interessante para truncar.
    }
    const std::size_t body_total = encoded->size() - 4;
    const std::size_t strict_limit = body_total > tolerated_tail ? body_total - tolerated_tail : 0;
    bool all_rejected = true;
    std::size_t offending = 0;
    for (std::size_t body_len = 1; body_len < strict_limit; ++body_len) {
        BinaryWriter frame;
        frame.write_u32(static_cast<std::uint32_t>(body_len));
        frame.write_bytes(std::span<const std::byte>(*encoded).subspan(4, body_len));
        auto truncated = std::move(frame).take();
        if (modb::net::decode_message(truncated).has_value()) {
            all_rejected = false;
            offending = body_len;
            break;
        }
    }
    suite.check(all_rejected, std::string{label} + ": every truncated prefix of the mandatory "
                                                    "body is rejected (offending body_len=" +
                                   std::to_string(offending) + ")");
}

void check_round_trip(TestSuite& suite, const Message& original, std::string_view label,
                      std::size_t tolerated_tail = 0) {
    auto encoded = modb::net::encode_message(original);
    suite.check(encoded.has_value(), std::string{label} + " encodes");
    if (!encoded) {
        return;
    }
    auto decoded = modb::net::decode_message(*encoded);
    suite.check(decoded.has_value(), std::string{label} + " decodes");
    if (!decoded) {
        return;
    }
    suite.check(*decoded == original, std::string{label} + " round-trip");
    check_all_truncations_rejected(suite, original, label, tolerated_tail);
}

std::vector<std::byte> prepend_length(std::uint32_t length, std::span<const std::byte> body) {
    BinaryWriter writer;
    writer.write_u32(length);
    writer.write_bytes(body);
    return std::move(writer).take();
}

} // namespace

int main() {
    TestSuite suite;

    // --- Round-trip de cada mensagem (8A) ---
    check_round_trip(suite,
                     Hello{.version = modb::net::protocol_version,
                           .database_name = "demo",
                           .accepted_codecs = {Compression::none}},
                     "Hello", /*tolerated_tail=*/2);  // `minor` (u16) e' aditivo (Fase 10E)

    check_round_trip(suite,
                     HelloOk{.version = modb::net::protocol_version,
                             .baseline = ObjectId{42},
                             .selected_codec = Compression::none,
                             .max_frame_bytes = max_frame_bytes},
                     "HelloOk", /*tolerated_tail=*/2);  // idem

    // Peer antigo (anterior a' Fase 10E): Hello/HelloOk sem os 2 bytes finais
    // de `minor` ainda decodificam, usando o default (0), em vez de falhar.
    {
        auto full = modb::net::encode_message(Hello{.version = modb::net::protocol_version,
                                                     .database_name = "demo",
                                                     .accepted_codecs = {Compression::none}});
        suite.check(full.has_value(), "Hello (old-peer compat): base message encodes");
        if (full && full->size() > 6) {
            const std::size_t body_len = (full->size() - 4) - 2;  // omite os 2 bytes de `minor`
            BinaryWriter frame;
            frame.write_u32(static_cast<std::uint32_t>(body_len));
            frame.write_bytes(std::span<const std::byte>(*full).subspan(4, body_len));
            auto decoded = modb::net::decode_message(std::move(frame).take());
            const auto* hello = decoded ? std::get_if<Hello>(&*decoded) : nullptr;
            suite.check(hello != nullptr && hello->minor == 0,
                        "Hello without the trailing `minor` field decodes with the default "
                        "(old-peer compat)");
        }
    }
    {
        auto full = modb::net::encode_message(HelloOk{.version = modb::net::protocol_version,
                                                       .baseline = ObjectId{42},
                                                       .selected_codec = Compression::none,
                                                       .max_frame_bytes = max_frame_bytes});
        suite.check(full.has_value(), "HelloOk (old-peer compat): base message encodes");
        if (full && full->size() > 6) {
            const std::size_t body_len = (full->size() - 4) - 2;
            BinaryWriter frame;
            frame.write_u32(static_cast<std::uint32_t>(body_len));
            frame.write_bytes(std::span<const std::byte>(*full).subspan(4, body_len));
            auto decoded = modb::net::decode_message(std::move(frame).take());
            const auto* hello_ok = decoded ? std::get_if<HelloOk>(&*decoded) : nullptr;
            suite.check(hello_ok != nullptr && hello_ok->minor == 0,
                        "HelloOk without the trailing `minor` field decodes with the default "
                        "(old-peer compat)");
        }
    }

    QueryDescription description{
        .type = TypeDefinitionId{16},
        .limit = 10,
        .equals =
            modb::net::EqualityFilter{.field = FieldId{2}, .value = AttributeValue{std::int64_t{7}}},
        .project = {FieldId{1}, FieldId{2}},
    };
    check_round_trip(suite, Query{.query_id = 9, .description = description}, "Query");

    check_round_trip(suite, StreamBegin{.query_id = 9}, "StreamBegin");

    ObjectEnvelope envelope{
        .object_id = ObjectId{100},
        .type_definition_id = TypeDefinitionId{16},
        .payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}},
    };
    check_round_trip(suite,
                     ObjectFrame{.query_id = 9,
                                 .compression = Compression::none,
                                 .records = {envelope, envelope}},
                     "ObjectFrame");

    check_round_trip(suite, StreamEnd{.query_id = 9, .total = 2}, "StreamEnd");
    check_round_trip(suite,
                     StreamError{.query_id = 9,
                                 .code = ErrorCode::invalid_argument,
                                 .message = "boom"},
                     "StreamError");
    check_round_trip(suite, Cancel{.query_id = 9}, "Cancel");

    check_round_trip(suite,
                     OpCall{.call_id = 3,
                            .operation_id = "account.transfer",
                            .args = {std::byte{1}, std::byte{2}}},
                     "OpCall");
    check_round_trip(suite,
                     OpResult{.call_id = 3,
                              .ok = true,
                              .payload = {std::byte{9}}},
                     "OpResult ok");
    check_round_trip(suite,
                     OpResult{.call_id = 4,
                              .ok = false,
                              .code = ErrorCode::invalid_argument,
                              .message = "insufficient funds"},
                     "OpResult error");

    check_round_trip(suite, FacadeList{.request_id = 11}, "FacadeList");
    check_round_trip(
        suite,
        FacadeListOk{
            .request_id = 11,
            .facades =
                {
                    modb::ops::FacadeDescriptor{
                        .facade_id = "accounts",
                        .facade_version = 1,
                        .mode = modb::ops::FacadeMode::read_write,
                        .methods =
                            {
                                modb::ops::MethodDescriptor{
                                    .operation_id = "account.transfer",
                                    .method_version = 1,
                                    .mode = modb::ops::OperationMode::read_write,
                                },
                            },
                    },
                },
        },
        "FacadeListOk");
    check_round_trip(suite,
                     FacadeOpen{.request_id = 12,
                                .facade_id = "accounts",
                                .facade_version = 1},
                     "FacadeOpen");
    check_round_trip(suite,
                     FacadeOpenOk{.request_id = 12,
                                  .ok = true,
                                  .facade_id = "accounts",
                                  .facade_version = 1},
                     "FacadeOpenOk ok");
    check_round_trip(suite,
                     FacadeOpenOk{.request_id = 13,
                                  .ok = false,
                                  .code = ErrorCode::incompatible_facade_version,
                                  .message = "wrong version",
                                  .facade_id = "accounts",
                                  .facade_version = 99},
                     "FacadeOpenOk error");

    // Frame com um único slot é válido.
    check_round_trip(suite,
                     ObjectFrame{.query_id = 1,
                                 .compression = Compression::none,
                                 .records = {envelope}},
                     "ObjectFrame single slot");

    // --- Frames hostis ---
    {
        // Truncado: length promete bytes que não existem.
        const auto truncated = prepend_length(10, std::span<const std::byte>{});
        auto decoded = modb::net::decode_message(truncated);
        suite.check(!decoded && decoded.error().code == ErrorCode::unexpected_end_of_input,
                    "truncated frame is rejected");
    }
    {
        // Length mentiroso: length pequeno demais para o type byte.
        std::array<std::byte, 4> only_length{};
        only_length[0] = std::byte{0}; // length = 0
        auto decoded = modb::net::decode_message(only_length);
        suite.check(!decoded && decoded.error().code == ErrorCode::protocol_error,
                    "zero length frame is rejected");
    }
    {
        // Length > 16 MiB: rejeita sem alocar o body.
        BinaryWriter writer;
        writer.write_u32(max_frame_bytes + 1);
        writer.write_u8(1);
        auto decoded = modb::net::decode_message(writer.bytes());
        suite.check(!decoded && decoded.error().code == ErrorCode::frame_too_large,
                    "frame > 16 MiB is rejected");
    }
    {
        // Diretório inválido: offset fora da área de dados.
        BinaryWriter payload;
        payload.write_u32(1); // query_id
        payload.write_u32(1); // record_count
        payload.write_u8(0);  // compression none
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u32(4); // uncompressed_size
        payload.write_u32(4); // encoded_size
        payload.write_u32(10); // offset fora
        payload.write_u32(1);  // length
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u8(0);

        BinaryWriter frame;
        const auto length = static_cast<std::uint32_t>(1 + payload.bytes().size());
        frame.write_u32(length);
        frame.write_u8(static_cast<std::uint8_t>(modb::net::MessageType::object_frame));
        frame.write_bytes(payload.bytes());

        auto decoded = modb::net::decode_message(frame.bytes());
        suite.check(!decoded && decoded.error().code == ErrorCode::protocol_error,
                    "slot outside data area is rejected");
    }
    {
        // Sobreposição de slots.
        BinaryWriter data;
        // Dois envelopes mínimos falsos: só precisamos de bytes na área.
        for (int i = 0; i < 40; ++i) {
            data.write_u8(static_cast<std::uint8_t>(i));
        }

        BinaryWriter payload;
        payload.write_u32(1);
        payload.write_u32(2);
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u32(static_cast<std::uint32_t>(data.bytes().size()));
        payload.write_u32(static_cast<std::uint32_t>(data.bytes().size()));
        payload.write_u32(0);
        payload.write_u32(20);
        payload.write_u32(10); // sobrepõe [0,20)
        payload.write_u32(20);
        payload.write_bytes(data.bytes());

        BinaryWriter frame;
        frame.write_u32(static_cast<std::uint32_t>(1 + payload.bytes().size()));
        frame.write_u8(static_cast<std::uint8_t>(modb::net::MessageType::object_frame));
        frame.write_bytes(payload.bytes());

        auto decoded = modb::net::decode_message(frame.bytes());
        suite.check(!decoded && decoded.error().code == ErrorCode::protocol_error,
                    "overlapping slots are rejected");
    }
    {
        // Envelope truncado dentro do slot.
        BinaryWriter data;
        data.write_u64(1); // object_id
        data.write_u64(2); // type
        data.write_u32(8); // payload_length mentirosa
        data.write_u8(0);  // só 1 byte de payload

        BinaryWriter payload;
        payload.write_u32(1);
        payload.write_u32(1);
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u32(static_cast<std::uint32_t>(data.bytes().size()));
        payload.write_u32(static_cast<std::uint32_t>(data.bytes().size()));
        payload.write_u32(0);
        payload.write_u32(static_cast<std::uint32_t>(data.bytes().size()));
        payload.write_bytes(data.bytes());

        BinaryWriter frame;
        frame.write_u32(static_cast<std::uint32_t>(1 + payload.bytes().size()));
        frame.write_u8(static_cast<std::uint8_t>(modb::net::MessageType::object_frame));
        frame.write_bytes(payload.bytes());

        auto decoded = modb::net::decode_message(frame.bytes());
        suite.check(!decoded, "truncated ObjectEnvelope in slot is rejected");
    }
    {
        // Lixo: bytes aleatórios não podem crashar.
        std::vector<std::byte> garbage(64);
        for (std::size_t i = 0; i < garbage.size(); ++i) {
            garbage[i] = static_cast<std::byte>(i * 37u + 11u);
        }
        auto decoded = modb::net::decode_message(garbage);
        suite.check(!decoded, "random garbage yields protocol failure without crash");
    }
    {
        // String hostil: length de string maior que o restante, sem alocar gigante.
        BinaryWriter payload;
        payload.write_u16(modb::net::protocol_version);
        payload.write_u32(1u << 20); // promete 1 MiB de nome
        // nenhum byte de nome

        BinaryWriter frame;
        frame.write_u32(static_cast<std::uint32_t>(1 + payload.bytes().size()));
        frame.write_u8(static_cast<std::uint8_t>(modb::net::MessageType::hello));
        frame.write_bytes(payload.bytes());

        auto decoded = modb::net::decode_message(frame.bytes());
        suite.check(!decoded, "lying string length is rejected without huge allocation");
    }

    // --- 8F: compressão RLE ---
    {
        std::vector<std::byte> zeros(256, std::byte{0});
        auto compressed = modb::net::compress_rle(zeros);
        suite.check(compressed.has_value(), "RLE compresses zeros");
        if (compressed) {
            suite.check(compressed->size() < zeros.size(), "RLE shrinks repeated bytes");
            auto inflated = modb::net::decompress_rle(*compressed, static_cast<std::uint32_t>(zeros.size()));
            suite.check(inflated.has_value() && *inflated == zeros, "RLE round-trip zeros");
        }
    }
    {
        // Frame compressível: payload repetitivo grande o bastante para o limiar.
        std::vector<std::byte> payload(128, std::byte{'A'});
        ObjectEnvelope fat{
            .object_id = ObjectId{1},
            .type_definition_id = TypeDefinitionId{2},
            .payload = payload,
        };
        ObjectFrame original{.query_id = 3, .compression = Compression::rle, .records = {fat}};
        auto encoded = modb::net::encode_message(original);
        suite.check(encoded.has_value(), "compressible ObjectFrame encodes");
        if (encoded) {
            auto decoded = modb::net::decode_message(*encoded);
            suite.check(decoded.has_value(), "compressible ObjectFrame decodes");
            if (decoded) {
                const auto* frame = std::get_if<ObjectFrame>(&*decoded);
                suite.check(frame != nullptr && frame->compression == Compression::rle,
                            "compressible frame stays compressed on wire");
                suite.check(frame != nullptr && frame->records == original.records,
                            "compressible frame logical payload matches");
            }
        }
    }
    {
        // Incompressível: preferência rle faz fallback para none.
        std::vector<std::byte> random_bytes(128);
        for (std::size_t i = 0; i < random_bytes.size(); ++i) {
            random_bytes[i] = static_cast<std::byte>((i * 131u + 17u) & 0xffu);
        }
        ObjectEnvelope noisy{
            .object_id = ObjectId{9},
            .type_definition_id = TypeDefinitionId{2},
            .payload = random_bytes,
        };
        ObjectFrame original{.query_id = 4, .compression = Compression::rle, .records = {noisy}};
        auto encoded = modb::net::encode_message(original);
        suite.check(encoded.has_value(), "incompressible ObjectFrame encodes");
        if (encoded) {
            auto decoded = modb::net::decode_message(*encoded);
            suite.check(decoded.has_value(), "incompressible ObjectFrame decodes");
            if (decoded) {
                const auto* frame = std::get_if<ObjectFrame>(&*decoded);
                suite.check(frame != nullptr && frame->compression == Compression::none,
                            "incompressible frame falls back to none");
                suite.check(frame != nullptr && frame->records == original.records,
                            "incompressible frame logical payload matches");
            }
        }
    }
    {
        // Codec desconhecido rejeitado sem alocar área descomprimida enorme.
        BinaryWriter payload;
        payload.write_u32(1); // query_id
        payload.write_u32(0); // record_count
        payload.write_u8(99); // codec desconhecido
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u32(1u << 20); // uncompressed_size hostil
        payload.write_u32(1);        // encoded_size mínimo
        payload.write_u8(0);

        BinaryWriter frame;
        frame.write_u32(static_cast<std::uint32_t>(1 + payload.bytes().size()));
        frame.write_u8(static_cast<std::uint8_t>(modb::net::MessageType::object_frame));
        frame.write_bytes(payload.bytes());

        auto decoded = modb::net::decode_message(frame.bytes());
        suite.check(!decoded && decoded.error().code == ErrorCode::protocol_error,
                    "unknown compression codec is rejected");
    }
    {
        // Razão de expansão inválida: rejeita antes de alocar.
        BinaryWriter payload;
        payload.write_u32(1);
        payload.write_u32(0);
        payload.write_u8(static_cast<std::uint8_t>(Compression::rle));
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u8(0);
        payload.write_u32(1024); // uncompressed
        payload.write_u32(1);    // encoded — razão 1024 > default 8
        payload.write_u8(0);

        BinaryWriter frame;
        frame.write_u32(static_cast<std::uint32_t>(1 + payload.bytes().size()));
        frame.write_u8(static_cast<std::uint8_t>(modb::net::MessageType::object_frame));
        frame.write_bytes(payload.bytes());

        auto decoded = modb::net::decode_message(frame.bytes());
        suite.check(!decoded && decoded.error().code == ErrorCode::protocol_error,
                    "excessive expansion ratio is rejected");
    }

    return suite.finish();
}
