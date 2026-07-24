#pragma once

#include "scenarios/object_store_lifecycle.hpp"

#include <cstdint>
#include <string>

namespace modb::bench {

struct StorageAsyncIoParams {
    std::uint64_t seed{0};
    std::uint64_t object_count{0}; // número de transações (commits)
    std::uint32_t stride{4};       // objetos (imagens de página) por transação
    std::string wal_io{"sync"};    // sync | async (DatabaseOptions::wal_io)
    std::string work_dir;
};

// Mede o custo do commit (WAL: begin + N page-images + commit, cada um com
// sync()) comparando o sink síncrono (NativeFile) com o assíncrono
// (AsyncFile) — Fase 13.7, ADR-019.
[[nodiscard]] SampleResult run_storage_async_io(const StorageAsyncIoParams& params);

} // namespace modb::bench
