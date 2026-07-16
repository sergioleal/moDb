// Importa a interface de IdentityMap.
#include "modb/object/identity_map.hpp"

// Importa store_le/load_le para os campos das páginas.
#include "modb/storage/endian.hpp"

// Disponibiliza std::equal ao validar o magic.
#include <algorithm>
// Disponibiliza o bloco fixo das assinaturas.
#include <array>
// Disponibiliza std::byte e std::size_t.
#include <cstddef>
// Disponibiliza std::string nas mensagens de erro.
#include <string>
// Disponibiliza std::string_view no helper de validação.
#include <string_view>

namespace modb::object {
namespace {

using storage::load_le;
using storage::Page;
using storage::PageId;
using storage::RecordId;
using storage::store_le;

// Assinaturas das páginas do mapa de identidade.
constexpr std::array<std::byte, 4> idmd_magic{std::byte{'I'}, std::byte{'D'}, std::byte{'M'},
                                              std::byte{'D'}};
constexpr std::array<std::byte, 4> idmp_magic{std::byte{'I'}, std::byte{'D'}, std::byte{'M'},
                                              std::byte{'P'}};
constexpr std::uint16_t idm_version = 1;

// Layout do cabeçalho comum: magic(4) version(2) reservado(2) e um u64 que no
// diretório é o ponteiro para o próximo IDMD e no IDMP fica reservado.
constexpr std::size_t idm_header_size = 16;
constexpr std::size_t next_dir_offset = 8;

// Cada entrada IDMP ocupa 16 bytes: page(8) slot(2) generation(2) flags(4).
constexpr std::size_t idmp_entry_size = 16;
// Cada ponteiro do diretório é um u64.
constexpr std::size_t dir_pointer_size = 8;

// Quantas entradas cabem numa página IDMP e quantos ponteiros num IDMD.
constexpr std::uint64_t entries_per_idmp =
    (storage::page_size - idm_header_size) / idmp_entry_size;
constexpr std::uint64_t pointers_per_idmd =
    (storage::page_size - idm_header_size) / dir_pointer_size;

// Bits de flag de uma entrada.
constexpr std::uint32_t flag_allocated = 1U;
constexpr std::uint32_t flag_removed = 2U;

// Estado decodificado de uma entrada IDMP.
struct Entry {
    std::uint64_t page{};
    std::uint16_t slot{};
    std::uint16_t generation{};
    std::uint32_t flags{};

    [[nodiscard]] bool allocated() const { return (flags & flag_allocated) != 0U; }
    [[nodiscard]] bool removed() const { return (flags & flag_removed) != 0U; }
};

// Offset de uma entrada dentro da página IDMP.
std::size_t entry_offset(std::uint64_t entry_index) {
    return idm_header_size + static_cast<std::size_t>(entry_index) * idmp_entry_size;
}

// Lê uma entrada da página IDMP.
Entry read_entry(const Page& page, std::uint64_t entry_index) {
    const auto offset = entry_offset(entry_index);
    Entry entry;
    entry.page = load_le<std::uint64_t>(page.bytes().subspan(offset, 8));
    entry.slot = load_le<std::uint16_t>(page.bytes().subspan(offset + 8, 2));
    entry.generation = load_le<std::uint16_t>(page.bytes().subspan(offset + 10, 2));
    entry.flags = load_le<std::uint32_t>(page.bytes().subspan(offset + 12, 4));
    return entry;
}

// Escreve uma entrada na página IDMP.
void write_entry(Page& page, std::uint64_t entry_index, const Entry& entry) {
    const auto offset = entry_offset(entry_index);
    store_le<std::uint64_t>(page.bytes().subspan(offset, 8), entry.page);
    store_le<std::uint16_t>(page.bytes().subspan(offset + 8, 2), entry.slot);
    store_le<std::uint16_t>(page.bytes().subspan(offset + 10, 2), entry.generation);
    store_le<std::uint32_t>(page.bytes().subspan(offset + 12, 4), entry.flags);
}

// Monta uma página nova com a assinatura pedida (demais bytes já são zero).
Page make_header_page(const std::array<std::byte, 4>& magic) {
    Page page;
    std::copy(magic.begin(), magic.end(), page.bytes().begin());
    store_le<std::uint16_t>(page.bytes().subspan(4, 2), idm_version);
    return page;
}

// Confere a assinatura de uma página do mapa.
Result<void> check_magic(const Page& page, const std::array<std::byte, 4>& magic,
                         std::string_view what) {
    if (!std::equal(magic.begin(), magic.end(), page.bytes().begin())) {
        return std::unexpected(
            Error{ErrorCode::invalid_page_format, "identity map " + std::string{what} +
                                                      " page is missing its signature"});
    }
    const auto version = load_le<std::uint16_t>(page.bytes().subspan(4, 2));
    if (version != idm_version) {
        return std::unexpected(Error{ErrorCode::incompatible_page_version,
                                     "unsupported identity map page version"});
    }
    return {};
}

} // namespace

Result<IdentityMap> IdentityMap::create(storage::PageFile& file) {
    auto page_id = file.allocate_page();
    if (!page_id) {
        return std::unexpected(page_id.error());
    }
    if (auto written = file.write(*page_id, make_header_page(idmd_magic)); !written) {
        return std::unexpected(written.error());
    }
    return IdentityMap{file, *page_id};
}

Result<IdentityMap> IdentityMap::open(storage::PageFile& file, storage::PageId directory_root) {
    auto page = file.read(directory_root);
    if (!page) {
        return std::unexpected(page.error());
    }
    if (auto valid = check_magic(*page, idmd_magic, "directory"); !valid) {
        return std::unexpected(valid.error());
    }
    return IdentityMap{file, directory_root};
}

Result<std::optional<storage::PageId>> IdentityMap::resolve_idmp(
    std::uint64_t entry_page) const {
    const auto directory_ordinal = entry_page / pointers_per_idmd;
    const auto slot = entry_page % pointers_per_idmd;

    // Caminha pela cadeia de diretórios até o de ordem `directory_ordinal`.
    PageId current = directory_root_;
    for (std::uint64_t step = 0; step < directory_ordinal; ++step) {
        auto page = file_->read(current);
        if (!page) {
            return std::unexpected(page.error());
        }
        const auto next = load_le<std::uint64_t>(page->bytes().subspan(next_dir_offset, 8));
        if (next == 0) {
            // O diretório não é profundo o bastante: a entrada não existe.
            return std::nullopt;
        }
        current = PageId{next};
    }
    auto page = file_->read(current);
    if (!page) {
        return std::unexpected(page.error());
    }
    const auto pointer = load_le<std::uint64_t>(
        page->bytes().subspan(idm_header_size + static_cast<std::size_t>(slot) * dir_pointer_size,
                              dir_pointer_size));
    if (pointer == 0) {
        return std::nullopt;
    }
    return PageId{pointer};
}

Result<storage::PageId> IdentityMap::ensure_idmp(std::uint64_t entry_page) {
    const auto directory_ordinal = entry_page / pointers_per_idmd;
    const auto slot = entry_page % pointers_per_idmd;

    // Caminha (e estende, se preciso) a cadeia de diretórios.
    PageId current = directory_root_;
    for (std::uint64_t step = 0; step < directory_ordinal; ++step) {
        auto page = file_->read(current);
        if (!page) {
            return std::unexpected(page.error());
        }
        auto next = load_le<std::uint64_t>(page->bytes().subspan(next_dir_offset, 8));
        if (next == 0) {
            // Cria o próximo diretório e liga o atual a ele.
            auto new_dir = file_->allocate_page();
            if (!new_dir) {
                return std::unexpected(new_dir.error());
            }
            if (auto written = file_->write(*new_dir, make_header_page(idmd_magic)); !written) {
                return std::unexpected(written.error());
            }
            store_le<std::uint64_t>(page->bytes().subspan(next_dir_offset, 8), new_dir->value);
            if (auto written = file_->write(current, *page); !written) {
                return std::unexpected(written.error());
            }
            next = new_dir->value;
        }
        current = PageId{next};
    }

    auto page = file_->read(current);
    if (!page) {
        return std::unexpected(page.error());
    }
    const auto pointer_offset =
        idm_header_size + static_cast<std::size_t>(slot) * dir_pointer_size;
    auto pointer = load_le<std::uint64_t>(page->bytes().subspan(pointer_offset, dir_pointer_size));
    if (pointer == 0) {
        // Cria a página de entradas e registra seu ponteiro no diretório.
        auto new_idmp = file_->allocate_page();
        if (!new_idmp) {
            return std::unexpected(new_idmp.error());
        }
        if (auto written = file_->write(*new_idmp, make_header_page(idmp_magic)); !written) {
            return std::unexpected(written.error());
        }
        store_le<std::uint64_t>(page->bytes().subspan(pointer_offset, dir_pointer_size),
                                new_idmp->value);
        if (auto written = file_->write(current, *page); !written) {
            return std::unexpected(written.error());
        }
        pointer = new_idmp->value;
    }
    return PageId{pointer};
}

Result<void> IdentityMap::bind(ObjectId id, storage::RecordId record) {
    if (id == invalid_object_id) {
        return std::unexpected(
            Error{ErrorCode::invalid_object_id, "cannot bind the invalid ObjectId 0"});
    }
    const auto entry_page = id.value / entries_per_idmp;
    const auto entry_index = id.value % entries_per_idmp;

    auto idmp_id = ensure_idmp(entry_page);
    if (!idmp_id) {
        return std::unexpected(idmp_id.error());
    }
    auto page = file_->read(*idmp_id);
    if (!page) {
        return std::unexpected(page.error());
    }
    if (auto valid = check_magic(*page, idmp_magic, "entries"); !valid) {
        return std::unexpected(valid.error());
    }
    const auto existing = read_entry(*page, entry_index);
    if (existing.allocated()) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
                                     "ObjectId " + std::to_string(id.value) + " is already bound"});
    }
    write_entry(*page, entry_index,
                Entry{record.page.value, record.slot.value, record.generation, flag_allocated});
    return file_->write(*idmp_id, *page);
}

Result<storage::RecordId> IdentityMap::find(ObjectId id) const {
    const auto entry_page = id.value / entries_per_idmp;
    const auto entry_index = id.value % entries_per_idmp;

    auto idmp_id = resolve_idmp(entry_page);
    if (!idmp_id) {
        return std::unexpected(idmp_id.error());
    }
    if (!*idmp_id) {
        return std::unexpected(
            Error{ErrorCode::record_not_found, "no object with id " + std::to_string(id.value)});
    }
    auto page = file_->read(**idmp_id);
    if (!page) {
        return std::unexpected(page.error());
    }
    if (auto valid = check_magic(*page, idmp_magic, "entries"); !valid) {
        return std::unexpected(valid.error());
    }
    const auto entry = read_entry(*page, entry_index);
    if (!entry.allocated() || entry.removed()) {
        return std::unexpected(
            Error{ErrorCode::record_not_found, "no object with id " + std::to_string(id.value)});
    }
    return RecordId{PageId{entry.page}, storage::SlotId{entry.slot}, entry.generation};
}

Result<void> IdentityMap::rebind(ObjectId id, storage::RecordId record) {
    const auto entry_page = id.value / entries_per_idmp;
    const auto entry_index = id.value % entries_per_idmp;

    auto idmp_id = resolve_idmp(entry_page);
    if (!idmp_id) {
        return std::unexpected(idmp_id.error());
    }
    if (!*idmp_id) {
        return std::unexpected(
            Error{ErrorCode::record_not_found, "no object with id " + std::to_string(id.value)});
    }
    auto page = file_->read(**idmp_id);
    if (!page) {
        return std::unexpected(page.error());
    }
    if (auto valid = check_magic(*page, idmp_magic, "entries"); !valid) {
        return std::unexpected(valid.error());
    }
    auto entry = read_entry(*page, entry_index);
    if (!entry.allocated() || entry.removed()) {
        return std::unexpected(
            Error{ErrorCode::record_not_found, "no object with id " + std::to_string(id.value)});
    }
    write_entry(*page, entry_index,
                Entry{record.page.value, record.slot.value, record.generation, flag_allocated});
    return file_->write(**idmp_id, *page);
}

Result<void> IdentityMap::erase(ObjectId id) {
    const auto entry_page = id.value / entries_per_idmp;
    const auto entry_index = id.value % entries_per_idmp;

    auto idmp_id = resolve_idmp(entry_page);
    if (!idmp_id) {
        return std::unexpected(idmp_id.error());
    }
    if (!*idmp_id) {
        return std::unexpected(
            Error{ErrorCode::record_not_found, "no object with id " + std::to_string(id.value)});
    }
    auto page = file_->read(**idmp_id);
    if (!page) {
        return std::unexpected(page.error());
    }
    if (auto valid = check_magic(*page, idmp_magic, "entries"); !valid) {
        return std::unexpected(valid.error());
    }
    auto entry = read_entry(*page, entry_index);
    if (!entry.allocated() || entry.removed()) {
        return std::unexpected(
            Error{ErrorCode::record_not_found, "no object with id " + std::to_string(id.value)});
    }
    // Preserva a última localização e apenas marca o tombstone.
    entry.flags |= flag_removed;
    write_entry(*page, entry_index, entry);
    return file_->write(**idmp_id, *page);
}

} // namespace modb::object
