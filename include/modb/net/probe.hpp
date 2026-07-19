#pragma once

// Marcadores de probe para implantação serverless (Fase 13D).

#include "modb/error.hpp"

#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace modb::net {

// Escreve (ou toca) um arquivo de probe. Falhas de I/O viram io_error.
[[nodiscard]] inline Result<void> write_probe_file(const std::filesystem::path& path,
                                                   std::string_view contents = "ready\n") {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        return std::unexpected(Error{ErrorCode::io_error, "could not write probe file"});
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!out) {
        return std::unexpected(Error{ErrorCode::io_error, "could not flush probe file"});
    }
    return {};
}

[[nodiscard]] inline Result<void> remove_probe_file(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return {};
}

} // namespace modb::net
