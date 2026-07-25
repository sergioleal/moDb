// Cobre o encode/decode de TODOS os tipos de mensagem do canal de replicação
// (Fase 14C), não só hello/frame/gap: a suíte anterior deixava HelloOk,
// BootstrapRequest/Begin/Chunk/End, WalAck, ReplicationError e
// ReplicationCancel inteiramente sem round-trip — nem o caminho feliz de
// encode/decode rodava para esses tipos.
#include "modb/net/replication_protocol.hpp"
#include "modb/storage/binary.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

using namespace modb;
using namespace modb::net;
using namespace modb::object;
using modb::storage::BinaryWriter;

namespace {

// Um frame válido é [u32 length][u8 type][payload], length = 1 + |payload|.
// Cortar o corpo em qualquer prefixo estrito (ajustando só o length externo
// para casar com o novo tamanho) precisa sempre falhar: decode_replication_message
// confere `length + 4 != frame.size()` antes de despachar por tipo, e cada
// caso lê campos de tamanho fixo (ou length-prefixed) que continuam com os
// valores ORIGINAIS — um corte cedo demais sempre deixa algum campo sem bytes
// suficientes. Mesma técnica usada em protocol_test.cpp, sem a exceção de
// "campo aditivo tolerado": este protocolo não tem nenhum campo opcional.
void check_all_truncations_rejected(TestSuite& suite, const ReplicationMessage& original,
                                    std::string_view label) {
    auto encoded = encode_replication_message(original);
    if (!encoded || encoded->size() <= 5) {
        return;
    }
    const std::size_t body_total = encoded->size() - 4;
    bool all_rejected = true;
    std::size_t offending = 0;
    for (std::size_t body_len = 1; body_len < body_total; ++body_len) {
        BinaryWriter frame;
        frame.write_u32(static_cast<std::uint32_t>(body_len));
        frame.write_bytes(std::span<const std::byte>(*encoded).subspan(4, body_len));
        auto truncated = std::move(frame).take();
        if (decode_replication_message(truncated).has_value()) {
            all_rejected = false;
            offending = body_len;
            break;
        }
    }
    suite.check(all_rejected, std::string{label} + ": every truncated prefix of the body is "
                                                    "rejected (offending body_len=" +
                                   std::to_string(offending) + ")");
}

template <typename Message>
void check_round_trip(TestSuite& suite, const Message& original, std::string_view label) {
    auto encoded = encode_replication_message(original);
    suite.check(encoded.has_value(), std::string{label} + " encodes");
    if (!encoded) {
        return;
    }
    auto decoded = decode_replication_message(*encoded);
    suite.check(decoded.has_value(), std::string{label} + " decodes");
    if (!decoded) {
        return;
    }
    const auto* out = std::get_if<Message>(&*decoded);
    suite.check(out != nullptr && *out == original, std::string{label} + " round-trip");
    check_all_truncations_rejected(suite, ReplicationMessage{original}, label);
}

} // namespace

int main() {
    TestSuite suite;

    ReplicationHello hello;
    hello.version = 1;
    hello.database_uuid.bytes[0] = 0xAB;
    hello.timeline_id = TimelineId{7};
    check_round_trip(suite, hello, "ReplicationHello");

    ReplicationHelloOk hello_ok;
    hello_ok.version = 1;
    hello_ok.database_uuid.bytes[1] = 0xCD;
    hello_ok.timeline_id = TimelineId{7};
    hello_ok.primary_commit_lsn = 4242;
    check_round_trip(suite, hello_ok, "ReplicationHelloOk");

    BootstrapRequest req_known;
    req_known.has_known = true;
    req_known.known_uuid.bytes[2] = 0xEF;
    req_known.known_timeline = TimelineId{3};
    req_known.known_lsn = 99;
    check_round_trip(suite, req_known, "BootstrapRequest (peer conhecido)");

    BootstrapRequest req_unknown;
    req_unknown.has_known = false;
    check_round_trip(suite, req_unknown, "BootstrapRequest (peer novo)");

    BootstrapBegin begin;
    begin.page_size = 4096;
    begin.cut_lsn = 100;
    begin.epoch = 2;
    begin.baseline = 16;
    begin.size_bytes = 65536;
    begin.content_crc = 0xDEADBEEF;
    check_round_trip(suite, begin, "BootstrapBegin");

    BootstrapChunk chunk;
    chunk.offset = 4096;
    chunk.bytes = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    check_round_trip(suite, chunk, "BootstrapChunk");

    BootstrapEnd end;
    end.content_crc = 0x12345678;
    check_round_trip(suite, end, "BootstrapEnd");

    WalSubscribe subscribe;
    subscribe.database_uuid.bytes[3] = 0x01;
    subscribe.timeline_id = TimelineId{5};
    subscribe.from_lsn = 10;
    check_round_trip(suite, subscribe, "WalSubscribe");

    // WalFrame não usa check_round_trip: crc=0 é sentinela de "calcule pra
    // mim" (linha 114 de replication_protocol.cpp), então o crc decodificado
    // é o crc32(records) real, nunca o 0 original — comparar a struct inteira
    // com `==` sempre falharia por causa desse campo. Compara os demais campos
    // e ainda reaproveita o fuzz de truncamento.
    {
        WalFrame frame;
        frame.first_lsn = 10;
        frame.last_lsn = 12;
        frame.records = {std::byte{1}, std::byte{2}, std::byte{3}};
        auto encoded = encode_replication_message(frame);
        suite.check(encoded.has_value(), "WalFrame encodes");
        if (encoded) {
            auto decoded = decode_replication_message(*encoded);
            suite.check(decoded.has_value(), "WalFrame decodes");
            if (decoded) {
                const auto* out = std::get_if<WalFrame>(&*decoded);
                suite.check(out != nullptr && out->first_lsn == frame.first_lsn &&
                                out->last_lsn == frame.last_lsn && out->records == frame.records,
                            "WalFrame round-trip (crc is server-computed on encode)");
            }
            check_all_truncations_rejected(suite, ReplicationMessage{frame}, "WalFrame");
        }
    }

    WalAck ack;
    ack.applied_lsn = 77;
    check_round_trip(suite, ack, "WalAck");

    WalGap gap;
    gap.oldest_available_lsn = 5;
    check_round_trip(suite, gap, "WalGap");

    ReplicationHeartbeat heartbeat;
    heartbeat.primary_commit_lsn = 321;
    check_round_trip(suite, heartbeat, "ReplicationHeartbeat");

    ReplicationError error;
    error.code = ErrorCode::protocol_error;
    error.message = "boom";
    check_round_trip(suite, error, "ReplicationError");

    ReplicationCancel cancel;
    check_round_trip(suite, cancel, "ReplicationCancel");

    // --- Frames hostis ---
    {
        std::vector<std::byte> too_short(4, std::byte{0});
        suite.check(!decode_replication_message(too_short).has_value(),
                    "frame under 5 bytes is rejected");
    }
    {
        // length mentiroso: diz um tamanho que não bate com o frame real.
        BinaryWriter w;
        w.write_u32(100);
        w.write_u8(static_cast<std::uint8_t>(ReplicationMessageType::hello));
        auto lying = std::move(w).take();
        suite.check(!decode_replication_message(lying).has_value(), "length mismatch is rejected");
    }
    {
        // WalFrame com CRC explicitamente errado é rejeitado no decode.
        WalFrame bad_frame;
        bad_frame.first_lsn = 1;
        bad_frame.last_lsn = 2;
        bad_frame.records = {std::byte{9}};
        bad_frame.crc = 0xFFFFFFFF;  // != 0, então encode usa este valor em vez de crc32(records)
        auto encoded = encode_replication_message(bad_frame);
        suite.check(encoded.has_value(), "WalFrame with explicit bad crc encodes");
        if (encoded) {
            auto decoded = decode_replication_message(*encoded);
            suite.check(!decoded.has_value(), "WalFrame CRC mismatch is rejected on decode");
        }
    }
    {
        // Tipo fora do enum ReplicationMessageType é rejeitado.
        BinaryWriter w;
        w.write_u32(1);
        w.write_u8(200);
        auto unknown = std::move(w).take();
        suite.check(!decode_replication_message(unknown).has_value(),
                    "unknown replication message type is rejected");
    }

    return suite.finish();
}
