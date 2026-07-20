#pragma once

// Canal privilegiado de replicação (Fase 14C): mesmo framing da Fase 8
// (length u32 | type u8 | payload), tipos ≥ 100 — incompatível com consulta.

#include "modb/error.hpp"
#include "modb/object/ids.hpp"
#include "modb/storage/binary.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace modb::net {

inline constexpr std::uint16_t replication_protocol_version = 1;
inline constexpr std::uint8_t replication_type_base = 100;

enum class ReplicationMessageType : std::uint8_t {
    hello = 100,
    hello_ok = 101,
    bootstrap_request = 102,
    bootstrap_begin = 103,
    bootstrap_chunk = 104,
    bootstrap_end = 105,
    wal_subscribe = 106,
    wal_frame = 107,
    wal_ack = 108,
    wal_gap = 109,
    heartbeat = 110,
    error = 111,
    cancel = 112,
};

struct ReplicationHello {
    std::uint16_t version{replication_protocol_version};
    object::DatabaseUuid database_uuid{};
    object::TimelineId timeline_id{};
    friend bool operator==(const ReplicationHello&, const ReplicationHello&) = default;
};

struct ReplicationHelloOk {
    std::uint16_t version{replication_protocol_version};
    object::DatabaseUuid database_uuid{};
    object::TimelineId timeline_id{};
    std::uint64_t primary_commit_lsn{};
    friend bool operator==(const ReplicationHelloOk&, const ReplicationHelloOk&) = default;
};

struct BootstrapRequest {
    bool has_known{false};
    object::DatabaseUuid known_uuid{};
    object::TimelineId known_timeline{};
    std::uint64_t known_lsn{};
    friend bool operator==(const BootstrapRequest&, const BootstrapRequest&) = default;
};

struct BootstrapBegin {
    std::uint32_t page_size{};
    std::uint64_t cut_lsn{};
    std::uint64_t epoch{};
    std::uint64_t baseline{};
    std::uint64_t size_bytes{};
    std::uint32_t content_crc{};
    friend bool operator==(const BootstrapBegin&, const BootstrapBegin&) = default;
};

struct BootstrapChunk {
    std::uint64_t offset{};
    std::vector<std::byte> bytes{};
    friend bool operator==(const BootstrapChunk&, const BootstrapChunk&) = default;
};

struct BootstrapEnd {
    std::uint32_t content_crc{};
    friend bool operator==(const BootstrapEnd&, const BootstrapEnd&) = default;
};

struct WalSubscribe {
    object::DatabaseUuid database_uuid{};
    object::TimelineId timeline_id{};
    std::uint64_t from_lsn{};
    friend bool operator==(const WalSubscribe&, const WalSubscribe&) = default;
};

struct WalFrame {
    std::uint64_t first_lsn{};
    std::uint64_t last_lsn{};
    std::vector<std::byte> records{};
    std::uint32_t crc{};
    friend bool operator==(const WalFrame&, const WalFrame&) = default;
};

struct WalAck {
    std::uint64_t applied_lsn{};
    friend bool operator==(const WalAck&, const WalAck&) = default;
};

struct WalGap {
    std::uint64_t oldest_available_lsn{};
    friend bool operator==(const WalGap&, const WalGap&) = default;
};

struct ReplicationHeartbeat {
    std::uint64_t primary_commit_lsn{};
    friend bool operator==(const ReplicationHeartbeat&, const ReplicationHeartbeat&) = default;
};

struct ReplicationError {
    ErrorCode code{ErrorCode::protocol_error};
    std::string message{};
    friend bool operator==(const ReplicationError&, const ReplicationError&) = default;
};

struct ReplicationCancel {
    friend bool operator==(const ReplicationCancel&, const ReplicationCancel&) = default;
};

using ReplicationMessage =
    std::variant<ReplicationHello, ReplicationHelloOk, BootstrapRequest, BootstrapBegin,
                 BootstrapChunk, BootstrapEnd, WalSubscribe, WalFrame, WalAck, WalGap,
                 ReplicationHeartbeat, ReplicationError, ReplicationCancel>;

[[nodiscard]] Result<std::vector<std::byte>> encode_replication_message(
    const ReplicationMessage& message);
[[nodiscard]] Result<ReplicationMessage> decode_replication_message(
    std::span<const std::byte> frame);

} // namespace modb::net
