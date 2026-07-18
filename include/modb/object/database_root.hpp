#pragma once

// Importa Result e os códigos de erro.
#include "modb/error.hpp"
// Importa ObjectId/BaselineId.
#include "modb/object/ids.hpp"
// Importa PageFile e PageId.
#include "modb/storage/page_file.hpp"

// Disponibiliza inteiros de largura fixa.
#include <cstdint>
// Disponibiliza a raiz opcional de cada subsistema.
#include <optional>

namespace modb::object {

// Espelha e persiste a página raiz do banco OO (DBRT, ADR-004), apontada pelo
// campo catalog_root do superbloco. Concentra as raízes do mapa de identidade,
// dos heaps de catálogo e de dados, o contador de ObjectId e a baseline
// corrente. O valor 0 em qualquer campo de página/id significa "ainda não
// existe".
class DatabaseRoot {
public:
    // Aloca a página DBRT, grava os campos zerados e aponta o superbloco.
    [[nodiscard]] static Result<DatabaseRoot> create(storage::PageFile& file);
    // Lê a página DBRT indicada pelo superbloco e valida magic/versão.
    [[nodiscard]] static Result<DatabaseRoot> open(storage::PageFile& file);

    // Identifica a página DBRT no arquivo.
    [[nodiscard]] storage::PageId page() const noexcept { return page_; }

    // Raízes opcionais (nullopt quando o campo persistido é 0).
    [[nodiscard]] std::optional<storage::PageId> identity_dir() const noexcept;
    [[nodiscard]] std::optional<storage::PageId> catalog_heap_root() const noexcept;
    [[nodiscard]] std::optional<storage::PageId> data_heap_root() const noexcept;
    // Diretório de índices (Fase 7B); nullopt quando nenhum índice foi criado.
    // Gravado em espaço reservado do DBRT v2 (arquivos antigos leem 0), então
    // não muda a versão do formato.
    [[nodiscard]] std::optional<storage::PageId> index_dir() const noexcept;
    // Próximo ObjectId a alocar; começa em first_user_object_id.
    [[nodiscard]] std::uint64_t next_object_id() const noexcept { return next_object_id_; }
    // Baseline corrente; invalid_object_id (0) quando ainda não há nenhuma.
    [[nodiscard]] BaselineId current_baseline() const noexcept {
        return BaselineId{current_baseline_};
    }
    // Época MVCC global, incrementada uma vez por commit durável (Fase 6A).
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }

    // Cada setter atualiza o espelho e regrava a página imediatamente, para que
    // a raiz em disco nunca fique atrás do estado observável.
    [[nodiscard]] Result<void> set_identity_dir(storage::PageId id);
    [[nodiscard]] Result<void> set_catalog_heap_root(storage::PageId id);
    [[nodiscard]] Result<void> set_data_heap_root(storage::PageId id);
    [[nodiscard]] Result<void> set_next_object_id(std::uint64_t next);
    [[nodiscard]] Result<void> set_current_baseline(BaselineId baseline);
    [[nodiscard]] Result<void> advance_epoch();
    [[nodiscard]] Result<void> set_index_dir(storage::PageId id);

private:
    DatabaseRoot(storage::PageFile& file, storage::PageId page) noexcept
        : file_{&file}, page_{page} {}

    // Regrava a página DBRT a partir do espelho em memória.
    [[nodiscard]] Result<void> persist();

    // Arquivo cuja vida é controlada pelo chamador.
    storage::PageFile* file_;
    // Página onde o DBRT está gravado.
    storage::PageId page_;
    // Espelho dos campos persistidos.
    std::uint64_t identity_dir_{};
    std::uint64_t catalog_heap_root_{};
    std::uint64_t data_heap_root_{};
    std::uint64_t next_object_id_{first_user_object_id};
    std::uint64_t current_baseline_{};
    std::uint64_t epoch_{};
    std::uint64_t index_dir_{};
};

} // namespace modb::object
