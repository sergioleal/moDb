#include "modb/net/replication_protocol.hpp"
#include "modb/storage/endian.hpp"

#include <array>
#include <cstring>

namespace modb::net {
namespace {

using storage::load_le;
using storage::store_le;

constexpr std::size_t frame_header = 5; // length u32 + type u8

void write_uuid(storage::BinaryWriter& w, const object::DatabaseUuid& uuid) {
    w.write_bytes(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(uuid.bytes.data()), uuid.bytes.size()});
}

Result<object::DatabaseUuid> read_uuid(storage::BinaryReader& r) {
    auto bytes = r.read_bytes(16);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    object::DatabaseUuid uuid{};
    for (std::size_t i = 0; i < 16; ++i) {
        uuid.bytes[i] = static_cast<std::uint8_t>((*bytes)[i]);
    }
    return uuid;
}

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) ? (0xEDB88320U ^ (value >> 1)) : (value >> 1);
            }
            result[index] = value;
        }
        return result;
    }();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc = table[(crc ^ std::to_integer<std::uint8_t>(byte)) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

Result<std::vector<std::byte>> finish_frame(ReplicationMessageType type,
                                            storage::BinaryWriter& payload) {
    const auto body = payload.bytes();
    const auto length = static_cast<std::uint32_t>(1 + body.size());
    std::vector<std::byte> frame(4 + length);
    store_le(std::span<std::byte>{frame.data(), 4}, length);
    frame[4] = static_cast<std::byte>(static_cast<std::uint8_t>(type));
    if (!body.empty()) {
        std::memcpy(frame.data() + 5, body.data(), body.size());
    }
    return frame;
}

} // namespace

Result<std::vector<std::byte>> encode_replication_message(const ReplicationMessage& message) {
    return std::visit(
        [](const auto& msg) -> Result<std::vector<std::byte>> {
            using T = std::decay_t<decltype(msg)>;
            storage::BinaryWriter w;
            if constexpr (std::is_same_v<T, ReplicationHello>) {
                w.write_u16(msg.version);
                write_uuid(w, msg.database_uuid);
                w.write_u64(msg.timeline_id.value);
                return finish_frame(ReplicationMessageType::hello, w);
            } else if constexpr (std::is_same_v<T, ReplicationHelloOk>) {
                w.write_u16(msg.version);
                write_uuid(w, msg.database_uuid);
                w.write_u64(msg.timeline_id.value);
                w.write_u64(msg.primary_commit_lsn);
                return finish_frame(ReplicationMessageType::hello_ok, w);
            } else if constexpr (std::is_same_v<T, BootstrapRequest>) {
                w.write_u8(msg.has_known ? 1 : 0);
                write_uuid(w, msg.known_uuid);
                w.write_u64(msg.known_timeline.value);
                w.write_u64(msg.known_lsn);
                return finish_frame(ReplicationMessageType::bootstrap_request, w);
            } else if constexpr (std::is_same_v<T, BootstrapBegin>) {
                w.write_u32(msg.page_size);
                w.write_u64(msg.cut_lsn);
                w.write_u64(msg.epoch);
                w.write_u64(msg.baseline);
                w.write_u64(msg.size_bytes);
                w.write_u32(msg.content_crc);
                return finish_frame(ReplicationMessageType::bootstrap_begin, w);
            } else if constexpr (std::is_same_v<T, BootstrapChunk>) {
                w.write_u64(msg.offset);
                w.write_u32(static_cast<std::uint32_t>(msg.bytes.size()));
                w.write_bytes(msg.bytes);
                return finish_frame(ReplicationMessageType::bootstrap_chunk, w);
            } else if constexpr (std::is_same_v<T, BootstrapEnd>) {
                w.write_u32(msg.content_crc);
                return finish_frame(ReplicationMessageType::bootstrap_end, w);
            } else if constexpr (std::is_same_v<T, WalSubscribe>) {
                write_uuid(w, msg.database_uuid);
                w.write_u64(msg.timeline_id.value);
                w.write_u64(msg.from_lsn);
                return finish_frame(ReplicationMessageType::wal_subscribe, w);
            } else if constexpr (std::is_same_v<T, WalFrame>) {
                w.write_u64(msg.first_lsn);
                w.write_u64(msg.last_lsn);
                w.write_u32(static_cast<std::uint32_t>(msg.records.size()));
                w.write_bytes(msg.records);
                w.write_u32(msg.crc != 0 ? msg.crc : crc32(msg.records));
                return finish_frame(ReplicationMessageType::wal_frame, w);
            } else if constexpr (std::is_same_v<T, WalAck>) {
                w.write_u64(msg.applied_lsn);
                return finish_frame(ReplicationMessageType::wal_ack, w);
            } else if constexpr (std::is_same_v<T, WalGap>) {
                w.write_u64(msg.oldest_available_lsn);
                return finish_frame(ReplicationMessageType::wal_gap, w);
            } else if constexpr (std::is_same_v<T, ReplicationHeartbeat>) {
                w.write_u64(msg.primary_commit_lsn);
                return finish_frame(ReplicationMessageType::heartbeat, w);
            } else if constexpr (std::is_same_v<T, ReplicationError>) {
                w.write_u32(static_cast<std::uint32_t>(msg.code));
                w.write_u32(static_cast<std::uint32_t>(msg.message.size()));
                w.write_bytes(std::span<const std::byte>{
                    reinterpret_cast<const std::byte*>(msg.message.data()), msg.message.size()});
                return finish_frame(ReplicationMessageType::error, w);
            } else if constexpr (std::is_same_v<T, ReplicationCancel>) {
                return finish_frame(ReplicationMessageType::cancel, w);
            }
            return std::unexpected(Error{ErrorCode::protocol_error, "unknown replication message"});
        },
        message);
}

Result<ReplicationMessage> decode_replication_message(std::span<const std::byte> frame) {
    if (frame.size() < 5) {
        return std::unexpected(Error{ErrorCode::protocol_error, "replication frame too short"});
    }
    const auto length = load_le<std::uint32_t>(frame.subspan(0, 4));
    if (length + 4 != frame.size()) {
        return std::unexpected(Error{ErrorCode::protocol_error, "replication frame length mismatch"});
    }
    const auto type = static_cast<ReplicationMessageType>(std::to_integer<std::uint8_t>(frame[4]));
    storage::BinaryReader r{frame.subspan(5)};

    switch (type) {
    case ReplicationMessageType::hello: {
        ReplicationHello msg;
        auto v = r.read_u16();
        auto uuid = read_uuid(r);
        auto tl = r.read_u64();
        if (!v || !uuid || !tl) {
            return std::unexpected(Error{ErrorCode::protocol_error, "bad ReplicationHello"});
        }
        msg.version = *v;
        msg.database_uuid = *uuid;
        msg.timeline_id = object::TimelineId{*tl};
        return msg;
    }
    case ReplicationMessageType::hello_ok: {
        ReplicationHelloOk msg;
        auto v = r.read_u16();
        auto uuid = read_uuid(r);
        auto tl = r.read_u64();
        auto lsn = r.read_u64();
        if (!v || !uuid || !tl || !lsn) {
            return std::unexpected(Error{ErrorCode::protocol_error, "bad ReplicationHelloOk"});
        }
        msg.version = *v;
        msg.database_uuid = *uuid;
        msg.timeline_id = object::TimelineId{*tl};
        msg.primary_commit_lsn = *lsn;
        return msg;
    }
    case ReplicationMessageType::bootstrap_request: {
        BootstrapRequest msg;
        auto flag = r.read_u8();
        auto uuid = read_uuid(r);
        auto tl = r.read_u64();
        auto lsn = r.read_u64();
        if (!flag || !uuid || !tl || !lsn) {
            return std::unexpected(Error{ErrorCode::protocol_error, "bad BootstrapRequest"});
        }
        msg.has_known = *flag != 0;
        msg.known_uuid = *uuid;
        msg.known_timeline = object::TimelineId{*tl};
        msg.known_lsn = *lsn;
        return msg;
    }
    case ReplicationMessageType::bootstrap_begin: {
        BootstrapBegin msg;
        auto ps = r.read_u32();
        auto cut = r.read_u64();
        auto ep = r.read_u64();
        auto bl = r.read_u64();
        auto sz = r.read_u64();
        auto crc = r.read_u32();
        if (!ps || !cut || !ep || !bl || !sz || !crc) {
            return std::unexpected(Error{ErrorCode::protocol_error, "bad BootstrapBegin"});
        }
        msg.page_size = *ps;
        msg.cut_lsn = *cut;
        msg.epoch = *ep;
        msg.baseline = *bl;
        msg.size_bytes = *sz;
        msg.content_crc = *crc;
        return msg;
    }
    case ReplicationMessageType::bootstrap_chunk: {
        BootstrapChunk msg;
        auto off = r.read_u64();
        auto len = r.read_u32();
        if (!off || !len) {
            return std::unexpected(Error{ErrorCode::protocol_error, "bad BootstrapChunk"});
        }
        auto bytes = r.read_bytes(*len);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        msg.offset = *off;
        msg.bytes.assign(bytes->begin(), bytes->end());
        return msg;
    }
    case ReplicationMessageType::bootstrap_end: {
        BootstrapEnd msg;
        auto crc = r.read_u32();
        if (!crc) {
            return std::unexpected(Error{ErrorCode::protocol_error, "bad BootstrapEnd"});
        }
        msg.content_crc = *crc;
        return msg;
    }
    case ReplicationMessageType::wal_subscribe: {
        WalSubscribe msg;
        auto uuid = read_uuid(r);
        auto tl = r.read_u64();
        auto from = r.read_u64();
        if (!uuid || !tl || !from) {
            return std::unexpected(Error{ErrorCode::protocol_error, "bad WalSubscribe"});
        }
        msg.database_uuid = *uuid;
        msg.timeline_id = object::TimelineId{*tl};
        msg.from_lsn = *from;
        return msg;
    }
    case ReplicationMessageType::wal_frame: {
        WalFrame msg;
        auto first = r.read_u64();
        auto last = r.read_u64();
        auto len = r.read_u32();
        if (!first || !last || !len) {
            return std::unexpected(Error{ErrorCode::protocol_error, "bad WalFrame"});
        }
        auto bytes = r.read_bytes(*len);
        auto crc = r.read_u32();
        if (!bytes || !crc) {
            return std::unexpected(Error{ErrorCode::protocol_error, "bad WalFrame body"});
        }
        if (crc32(*bytes) != *crc) {
            return std::unexpected(Error{ErrorCode::protocol_error, "WalFrame CRC mismatch"});
        }
        msg.first_lsn = *first;
        msg.last_lsn = *last;
        msg.records.assign(bytes->begin(), bytes->end());
        msg.crc = *crc;
        return msg;
    }
    case ReplicationMessageType::wal_ack: {
        WalAck msg;
        auto lsn = r.read_u64();
        if (!lsn) {
            return std::unexpected(Error{ErrorCode::protocol_error, "bad WalAck"});
        }
        msg.applied_lsn = *lsn;
        return msg;
    }
    case ReplicationMessageType::wal_gap: {
        WalGap msg;
        auto lsn = r.read_u64();
        if (!lsn) {
            return std::unexpected(Error{ErrorCode::protocol_error, "bad WalGap"});
        }
        msg.oldest_available_lsn = *lsn;
        return msg;
    }
    case ReplicationMessageType::heartbeat: {
        ReplicationHeartbeat msg;
        auto lsn = r.read_u64();
        if (!lsn) {
            return std::unexpected(Error{ErrorCode::protocol_error, "bad Heartbeat"});
        }
        msg.primary_commit_lsn = *lsn;
        return msg;
    }
    case ReplicationMessageType::error: {
        ReplicationError msg;
        auto code = r.read_u32();
        auto len = r.read_u32();
        if (!code || !len) {
            return std::unexpected(Error{ErrorCode::protocol_error, "bad ReplicationError"});
        }
        auto bytes = r.read_bytes(*len);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        msg.code = static_cast<ErrorCode>(*code);
        msg.message.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        return msg;
    }
    case ReplicationMessageType::cancel:
        return ReplicationCancel{};
    }
    return std::unexpected(Error{ErrorCode::protocol_error, "unknown replication type"});
}

} // namespace modb::net
