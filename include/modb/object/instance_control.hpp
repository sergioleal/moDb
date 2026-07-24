#pragma once

// Arquivo de controle do primary `wal_only` (Fase 15): identidade + LSNs sem
// PageFile durável. Magic "MCTL".

#include "modb/error.hpp"
#include "modb/object/ids.hpp"

#include <cstdint>
#include <filesystem>

namespace modb::object {

inline constexpr std::size_t instance_control_size = 64;

struct InstanceControl {
    DatabaseUuid database_uuid{};
    TimelineId timeline_id{1};
    std::uint64_t next_lsn{1};
    std::uint64_t checkpoint_lsn{0};
    std::uint64_t follower_ack_lsn{0};
};

[[nodiscard]] Result<void> write_instance_control(const std::filesystem::path& path,
                                                  const InstanceControl& control);
[[nodiscard]] Result<InstanceControl> read_instance_control(const std::filesystem::path& path);
[[nodiscard]] bool is_instance_control_file(const std::filesystem::path& path);

} // namespace modb::object
