#include "modb/object/instance_control.hpp"

#include "modb/storage/binary.hpp"
#include "modb/storage/native_file.hpp"

#include <array>
#include <cstring>
#include <system_error>

namespace modb::object {
namespace {

constexpr char magic[4] = {'M', 'C', 'T', 'L'};
constexpr std::uint16_t control_version = 1;

} // namespace

Result<void> write_instance_control(const std::filesystem::path& path,
                                    const InstanceControl& control) {
    storage::BinaryWriter writer;
    writer.write_bytes({reinterpret_cast<const std::byte*>(magic), 4});
    writer.write_u16(control_version);
    writer.write_u16(0);
    writer.write_bytes({reinterpret_cast<const std::byte*>(control.database_uuid.bytes.data()),
                        control.database_uuid.bytes.size()});
    writer.write_u64(control.timeline_id.value);
    writer.write_u64(control.next_lsn);
    writer.write_u64(control.checkpoint_lsn);
    writer.write_u64(control.follower_ack_lsn);
    auto bytes = std::move(writer).take();
    if (bytes.size() < instance_control_size) {
        bytes.resize(instance_control_size, std::byte{0});
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto file = storage::NativeFile::open(path, storage::NativeFile::Mode::create_new);
    if (!file) {
        return std::unexpected(file.error());
    }
    if (auto written = file->write_at(0, bytes); !written) {
        return std::unexpected(written.error());
    }
    return file->sync();
}

Result<InstanceControl> read_instance_control(const std::filesystem::path& path) {
    auto file = storage::NativeFile::open(path, storage::NativeFile::Mode::open_existing);
    if (!file) {
        return std::unexpected(file.error());
    }
    std::array<std::byte, instance_control_size> buffer{};
    if (auto read = file->read_at(0, buffer); !read) {
        return std::unexpected(read.error());
    }
    storage::BinaryReader reader{buffer};
    auto mag = reader.read_bytes(4);
    if (!mag || std::memcmp(mag->data(), magic, 4) != 0) {
        return std::unexpected(
            Error{ErrorCode::invalid_file_format, "not an instance control file (MCTL)"});
    }
    auto version = reader.read_u16();
    if (!version || *version != control_version) {
        return std::unexpected(
            Error{ErrorCode::incompatible_format_version, "unsupported MCTL version"});
    }
    (void)reader.read_u16();
    InstanceControl control;
    auto uuid = reader.read_bytes(16);
    if (!uuid || uuid->size() != 16) {
        return std::unexpected(Error{ErrorCode::corrupt_file, "bad uuid in control"});
    }
    for (std::size_t i = 0; i < 16; ++i) {
        control.database_uuid.bytes[i] = static_cast<std::uint8_t>((*uuid)[i]);
    }
    auto timeline = reader.read_u64();
    auto next = reader.read_u64();
    auto checkpoint = reader.read_u64();
    auto ack = reader.read_u64();
    if (!timeline || !next || !checkpoint || !ack) {
        return std::unexpected(Error{ErrorCode::corrupt_file, "truncated MCTL fields"});
    }
    control.timeline_id = TimelineId{*timeline};
    control.next_lsn = *next == 0 ? 1 : *next;
    control.checkpoint_lsn = *checkpoint;
    control.follower_ack_lsn = *ack;
    return control;
}

bool is_instance_control_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return false;
    }
    auto file = storage::NativeFile::open(path, storage::NativeFile::Mode::open_existing);
    if (!file) {
        return false;
    }
    std::array<std::byte, 4> buffer{};
    if (auto read = file->read_at(0, buffer); !read) {
        return false;
    }
    return std::memcmp(buffer.data(), magic, 4) == 0;
}

} // namespace modb::object
