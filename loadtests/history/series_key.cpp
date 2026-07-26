#include "history/series_key.hpp"

#include "runner/sha256.hpp"

#include <sstream>

namespace modb::loadtest {

std::string compute_series_key(const SeriesKeyInput& input) {
    // Concatenação canônica, um campo por linha com nome, para que a ordem
    // dos campos na struct não afete acidentalmente o hash caso alguém
    // reordene -- e para que seja fácil auditar o que entra no hash.
    std::ostringstream oss;
    oss << "case_id=" << input.case_id << '\n'
        << "workload_version=" << input.workload_version << '\n'
        << "dataset_id=" << input.dataset_id << '\n'
        << "dataset_version=" << input.dataset_version << '\n'
        << "scale=" << input.scale << '\n'
        << "payload=" << input.payload << '\n'
        << "batch=" << input.batch << '\n'
        << "concurrency=" << input.concurrency << '\n'
        << "durability=" << input.durability << '\n'
        << "cache=" << input.cache << '\n'
        << "primary_storage=" << input.primary_storage << '\n'
        << "build_type=" << input.build_type << '\n'
        << "instrumentation=" << input.instrumentation << '\n'
        << "arch=" << input.arch << '\n'
        << "page_size=" << input.page_size << '\n'
        << "format_version=" << input.format_version << '\n'
        << "protocol_version=" << input.protocol_version << '\n'
        << "host_class=" << input.host_class << '\n'
        << "target=" << input.target << '\n';

    const auto digest = modb::bench::sha256_text(oss.str());
    const auto hex = modb::bench::sha256_hex(digest);
    return hex.substr(0, 16);
}

} // namespace modb::loadtest
