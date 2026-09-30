// Fixa o número de cada ErrorCode (docs/COMPATIBILIDADE.md, "Códigos de erro").
//
// O número viaja no OpResult e é lido por clientes em outras linguagens, que não
// recompilam junto com o servidor. Um código renumerado quebra esses clientes em
// silêncio; este teste quebra a compilação antes. Código novo: entra no fim do
// enum e no fim desta tabela, com o próximo número.

#include "modb/error.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <utility>

namespace {

using modb::ErrorCode;

constexpr std::array k_codes = std::to_array<std::pair<ErrorCode, int>>({
    {ErrorCode::invalid_identifier, 0},
    {ErrorCode::invalid_argument, 1},
    {ErrorCode::empty_schema, 2},
    {ErrorCode::duplicate_column, 3},
    {ErrorCode::column_not_found, 4},
    {ErrorCode::value_count_mismatch, 5},
    {ErrorCode::type_mismatch, 6},
    {ErrorCode::null_constraint_violation, 7},
    {ErrorCode::duplicate_table, 8},
    {ErrorCode::table_not_found, 9},
    {ErrorCode::file_already_exists, 10},
    {ErrorCode::file_not_found, 11},
    {ErrorCode::io_error, 12},
    {ErrorCode::invalid_file_format, 13},
    {ErrorCode::incompatible_format_version, 14},
    {ErrorCode::corrupt_file, 15},
    {ErrorCode::page_not_found, 16},
    {ErrorCode::reserved_page, 17},
    {ErrorCode::unexpected_end_of_input, 18},
    {ErrorCode::invalid_encoding, 19},
    {ErrorCode::trailing_data, 20},
    {ErrorCode::value_too_large, 21},
    {ErrorCode::too_many_columns, 22},
    {ErrorCode::invalid_page_format, 23},
    {ErrorCode::incompatible_page_version, 24},
    {ErrorCode::corrupt_page, 25},
    {ErrorCode::page_full, 26},
    {ErrorCode::slot_not_found, 27},
    {ErrorCode::record_too_large, 28},
    {ErrorCode::page_chain_cycle, 29},
    {ErrorCode::record_not_found, 30},
    {ErrorCode::duplicate_field, 31},
    {ErrorCode::field_not_found, 32},
    {ErrorCode::duplicate_type, 33},
    {ErrorCode::type_not_found, 34},
    {ErrorCode::invalid_object_id, 35},
    {ErrorCode::binding_mismatch, 36},
    {ErrorCode::incompatible_projection, 37},
    {ErrorCode::transaction_required, 38},
    {ErrorCode::transaction_active, 39},
    {ErrorCode::transaction_committed, 40},
    {ErrorCode::commit_recovery_required, 41},
    {ErrorCode::database_recovery_required, 42},
    {ErrorCode::wal_corrupt, 43},
    {ErrorCode::snapshot_conflict, 44},
    {ErrorCode::protocol_error, 45},
    {ErrorCode::frame_too_large, 46},
    {ErrorCode::connection_closed, 47},
    {ErrorCode::operation_not_found, 48},
    {ErrorCode::incompatible_module, 49},
    {ErrorCode::incompatible_protocol_version, 50},
    {ErrorCode::facade_not_found, 51},
    {ErrorCode::facade_method_not_found, 52},
    {ErrorCode::incompatible_facade_version, 53},
    {ErrorCode::invalid_edge, 54},
    {ErrorCode::edge_target_not_found, 55},
    {ErrorCode::graph_limit_exceeded, 56},
    {ErrorCode::graph_cycle, 57},
    {ErrorCode::replica_read_only, 58},
    {ErrorCode::replication_gap, 59},
    {ErrorCode::timeline_mismatch, 60},
    {ErrorCode::database_uuid_mismatch, 61},
    {ErrorCode::bootstrap_required, 62},
    {ErrorCode::invalid_instance_config, 63},
    {ErrorCode::data_files_disabled, 64},
    {ErrorCode::no_data_replica, 65},
    {ErrorCode::commit_await_replica_timeout, 66},
    {ErrorCode::invalid_replica_state, 67},
    {ErrorCode::replica_download_failed, 68},
    {ErrorCode::manifest_hash_mismatch, 69},
    {ErrorCode::conflict, 70},
    {ErrorCode::internal_error, 71},
    {ErrorCode::operation_timeout, 72},
    {ErrorCode::unauthenticated, 73},
    {ErrorCode::permission_denied, 74},
});

// Cada código tem o número da tabela.
constexpr bool values_pinned() {
    for (const auto& [code, value] : k_codes) {
        if (static_cast<int>(code) != value) {
            return false;
        }
    }
    return true;
}
static_assert(values_pinned(), "um ErrorCode mudou de número: códigos novos entram no fim, nenhum é renumerado");

// A tabela é contínua a partir de 0: nenhum número foi pulado nem repetido.
constexpr bool table_contiguous() {
    for (std::size_t i = 0; i < k_codes.size(); ++i) {
        if (k_codes[i].second != static_cast<int>(i)) {
            return false;
        }
    }
    return true;
}
static_assert(table_contiguous(), "a tabela de ErrorCode precisa ser contínua a partir de 0");

} // namespace

int main() {
    std::cout << k_codes.size() << " ErrorCode com número fixo\n";
    return 0;
}
