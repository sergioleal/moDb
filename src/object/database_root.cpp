// Importa a interface de DatabaseRoot.
#include "modb/object/database_root.hpp"

// Importa BinaryWriter/BinaryReader para montar e ler a página.
#include "modb/storage/binary.hpp"

// Disponibiliza o bloco fixo da assinatura.
#include <array>
// Disponibiliza std::equal ao comparar o magic.
#include <algorithm>
#include <limits>
// Disponibiliza std::to_string nas mensagens de erro.
#include <string>

namespace modb::object {
namespace {

using storage::Page;
using storage::PageId;

// Assinatura dos quatro primeiros bytes da página DBRT.
constexpr std::array<std::byte, 4> dbrt_magic{std::byte{'D'}, std::byte{'B'}, std::byte{'R'},
                                              std::byte{'T'}};
// Versão do layout da página raiz.
constexpr std::uint16_t dbrt_version = 2;
constexpr std::uint16_t legacy_dbrt_version = 1;

// Converte um campo de página persistido (0 = ausente) em PageId opcional.
std::optional<PageId> optional_page(std::uint64_t value) {
    if (value == 0) {
        return std::nullopt;
    }
    return PageId{value};
}

} // namespace

Result<DatabaseRoot> DatabaseRoot::create(storage::PageFile& file) {
    // Reserva a página física que guardará o DBRT.
    auto page_id = file.allocate_page();
    if (!page_id) {
        return std::unexpected(page_id.error());
    }
    // Monta o objeto com os campos zerados (next_object_id já no piso de usuário).
    DatabaseRoot root{file, *page_id};
    if (auto persisted = root.persist(); !persisted) {
        return std::unexpected(persisted.error());
    }
    // Aponta o superbloco para a página recém-criada.
    if (auto linked = file.set_catalog_root(*page_id); !linked) {
        return std::unexpected(linked.error());
    }
    return root;
}

Result<DatabaseRoot> DatabaseRoot::open(storage::PageFile& file) {
    // O superbloco precisa apontar para uma página DBRT.
    const auto root_page = file.catalog_root();
    if (!root_page) {
        return std::unexpected(Error{ErrorCode::invalid_file_format,
                                     "database has no object store root (not an ODB++ file)"});
    }
    auto page = file.read(*root_page);
    if (!page) {
        return std::unexpected(page.error());
    }

    // Valida a assinatura antes de confiar em qualquer campo.
    if (!std::equal(dbrt_magic.begin(), dbrt_magic.end(), page->bytes().begin())) {
        return std::unexpected(
            Error{ErrorCode::invalid_file_format, "database root page is missing the DBRT magic"});
    }

    storage::BinaryReader reader{page->bytes()};
    // Consome o magic já validado.
    if (auto skipped = reader.read_bytes(dbrt_magic.size()); !skipped) {
        return std::unexpected(skipped.error());
    }
    auto version = reader.read_u16();
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version != dbrt_version && *version != legacy_dbrt_version) {
        return std::unexpected(Error{ErrorCode::incompatible_format_version,
                                     "unsupported database root version: " +
                                         std::to_string(*version)});
    }
    auto flags = reader.read_u16();
    auto identity_dir = reader.read_u64();
    auto catalog_heap_root = reader.read_u64();
    auto data_heap_root = reader.read_u64();
    auto next_object_id = reader.read_u64();
    auto current_baseline = reader.read_u64();
    if (!flags || !identity_dir || !catalog_heap_root || !data_heap_root || !next_object_id ||
        !current_baseline) {
        return std::unexpected(
            Error{ErrorCode::corrupt_page, "database root page is truncated"});
    }
    // Um next_object_id abaixo do piso de usuário indica corrupção.
    if (*next_object_id < first_user_object_id) {
        return std::unexpected(
            Error{ErrorCode::corrupt_page, "database root has an invalid next object id"});
    }

    DatabaseRoot root{file, *root_page};
    root.identity_dir_ = *identity_dir;
    root.catalog_heap_root_ = *catalog_heap_root;
    root.data_heap_root_ = *data_heap_root;
    root.next_object_id_ = *next_object_id;
    root.current_baseline_ = *current_baseline;
    // DBRT v1 não carregava época; sua primeira abertura na 6A a inicializa
    // explicitamente como zero e regrava o layout v2.
    if (*version == dbrt_version) {
        auto epoch = reader.read_u64();
        if (!epoch) {
            return std::unexpected(Error{ErrorCode::corrupt_page,
                                         "database root page is truncated before epoch"});
        }
        root.epoch_ = *epoch;
        // Campo de índice em espaço reservado v2: arquivos gravados antes da
        // Fase 7B têm zeros aqui (página zerada), o que significa "sem índices".
        if (auto index_dir = reader.read_u64(); index_dir) {
            root.index_dir_ = *index_dir;
        }
    } else if (auto upgraded = root.persist(); !upgraded) {
        return std::unexpected(upgraded.error());
    }
    return root;
}

std::optional<PageId> DatabaseRoot::identity_dir() const noexcept {
    return optional_page(identity_dir_);
}
std::optional<PageId> DatabaseRoot::catalog_heap_root() const noexcept {
    return optional_page(catalog_heap_root_);
}
std::optional<PageId> DatabaseRoot::data_heap_root() const noexcept {
    return optional_page(data_heap_root_);
}
std::optional<PageId> DatabaseRoot::index_dir() const noexcept {
    return optional_page(index_dir_);
}

Result<void> DatabaseRoot::persist() {
    storage::BinaryWriter writer;
    writer.write_bytes(dbrt_magic);
    writer.write_u16(dbrt_version);
    writer.write_u16(0);  // flags reservadas
    writer.write_u64(identity_dir_);
    writer.write_u64(catalog_heap_root_);
    writer.write_u64(data_heap_root_);
    writer.write_u64(next_object_id_);
    writer.write_u64(current_baseline_);
    writer.write_u64(epoch_);
    writer.write_u64(index_dir_);

    Page page;
    const auto bytes = writer.bytes();
    std::copy(bytes.begin(), bytes.end(), page.bytes().begin());
    return file_->write(page_, page);
}

Result<void> DatabaseRoot::set_identity_dir(storage::PageId id) {
    const auto previous = identity_dir_;
    identity_dir_ = id.value;
    if (auto persisted = persist(); !persisted) {
        identity_dir_ = previous;
        return std::unexpected(persisted.error());
    }
    return {};
}

Result<void> DatabaseRoot::set_catalog_heap_root(storage::PageId id) {
    const auto previous = catalog_heap_root_;
    catalog_heap_root_ = id.value;
    if (auto persisted = persist(); !persisted) {
        catalog_heap_root_ = previous;
        return std::unexpected(persisted.error());
    }
    return {};
}

Result<void> DatabaseRoot::set_data_heap_root(storage::PageId id) {
    const auto previous = data_heap_root_;
    data_heap_root_ = id.value;
    if (auto persisted = persist(); !persisted) {
        data_heap_root_ = previous;
        return std::unexpected(persisted.error());
    }
    return {};
}

Result<void> DatabaseRoot::set_next_object_id(std::uint64_t next) {
    const auto previous = next_object_id_;
    next_object_id_ = next;
    if (auto persisted = persist(); !persisted) {
        next_object_id_ = previous;
        return std::unexpected(persisted.error());
    }
    return {};
}

Result<void> DatabaseRoot::set_current_baseline(BaselineId baseline) {
    const auto previous = current_baseline_;
    current_baseline_ = baseline.value;
    if (auto persisted = persist(); !persisted) {
        current_baseline_ = previous;
        return std::unexpected(persisted.error());
    }
    return {};
}

Result<void> DatabaseRoot::set_index_dir(storage::PageId id) {
    const auto previous = index_dir_;
    index_dir_ = id.value;
    if (auto persisted = persist(); !persisted) {
        index_dir_ = previous;
        return std::unexpected(persisted.error());
    }
    return {};
}

Result<void> DatabaseRoot::advance_epoch() {
    if (epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(Error{ErrorCode::value_too_large, "database epoch is exhausted"});
    }
    const auto previous = epoch_;
    ++epoch_;
    if (auto persisted = persist(); !persisted) {
        epoch_ = previous;
        return std::unexpected(persisted.error());
    }
    return {};
}

} // namespace modb::object
