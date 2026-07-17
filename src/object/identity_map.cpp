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
constexpr std::uint16_t idmd_version = 1;
constexpr std::uint16_t legacy_idmp_version = 1;
constexpr std::uint16_t idmp_version = 2;

// Layout do cabeçalho comum: magic(4) version(2) reservado(2) e um u64 que no
// diretório é o ponteiro para o próximo IDMD e no IDMP fica reservado.
constexpr std::size_t idm_header_size = 16;
constexpr std::size_t next_dir_offset = 8;

// IDMP v2: localização atual (16), época atual (8), localização anterior
// (16) e época anterior (8). Os 48 bytes preservam PageId u64 e flags
// explícitas sem comprimir estado de tombstone.
constexpr std::size_t idmp_entry_size = 48;
constexpr std::size_t legacy_idmp_entry_size = 16;
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
    std::uint64_t current_epoch{};
    std::uint64_t previous_page{};
    std::uint16_t previous_slot{};
    std::uint16_t previous_generation{};
    std::uint32_t previous_flags{};
    std::uint64_t previous_epoch{};

    [[nodiscard]] bool allocated() const { return (flags & flag_allocated) != 0U; }
    [[nodiscard]] bool removed() const { return (flags & flag_removed) != 0U; }
};

struct LegacyEntry {
    std::uint64_t page{};
    std::uint16_t slot{};
    std::uint16_t generation{};
    std::uint32_t flags{};
    [[nodiscard]] bool allocated() const { return (flags & flag_allocated) != 0U; }
};

// Offset de uma entrada dentro da página IDMP.
std::size_t entry_offset(std::uint64_t entry_index) {
    return idm_header_size + static_cast<std::size_t>(entry_index) * idmp_entry_size;
}

std::size_t legacy_entry_offset(std::uint64_t entry_index) {
    return idm_header_size + static_cast<std::size_t>(entry_index) * legacy_idmp_entry_size;
}

LegacyEntry read_legacy_entry(const Page& page, std::uint64_t entry_index) {
    const auto offset = legacy_entry_offset(entry_index);
    return LegacyEntry{load_le<std::uint64_t>(page.bytes().subspan(offset, 8)),
                       load_le<std::uint16_t>(page.bytes().subspan(offset + 8, 2)),
                       load_le<std::uint16_t>(page.bytes().subspan(offset + 10, 2)),
                       load_le<std::uint32_t>(page.bytes().subspan(offset + 12, 4))};
}

// Lê uma entrada da página IDMP.
Entry read_entry(const Page& page, std::uint64_t entry_index) {
    const auto offset = entry_offset(entry_index);
    Entry entry;
    entry.page = load_le<std::uint64_t>(page.bytes().subspan(offset, 8));
    entry.slot = load_le<std::uint16_t>(page.bytes().subspan(offset + 8, 2));
    entry.generation = load_le<std::uint16_t>(page.bytes().subspan(offset + 10, 2));
    entry.flags = load_le<std::uint32_t>(page.bytes().subspan(offset + 12, 4));
    entry.current_epoch = load_le<std::uint64_t>(page.bytes().subspan(offset + 16, 8));
    entry.previous_page = load_le<std::uint64_t>(page.bytes().subspan(offset + 24, 8));
    entry.previous_slot = load_le<std::uint16_t>(page.bytes().subspan(offset + 32, 2));
    entry.previous_generation = load_le<std::uint16_t>(page.bytes().subspan(offset + 34, 2));
    entry.previous_flags = load_le<std::uint32_t>(page.bytes().subspan(offset + 36, 4));
    entry.previous_epoch = load_le<std::uint64_t>(page.bytes().subspan(offset + 40, 8));
    return entry;
}

// Escreve uma entrada na página IDMP.
void write_entry(Page& page, std::uint64_t entry_index, const Entry& entry) {
    const auto offset = entry_offset(entry_index);
    store_le<std::uint64_t>(page.bytes().subspan(offset, 8), entry.page);
    store_le<std::uint16_t>(page.bytes().subspan(offset + 8, 2), entry.slot);
    store_le<std::uint16_t>(page.bytes().subspan(offset + 10, 2), entry.generation);
    store_le<std::uint32_t>(page.bytes().subspan(offset + 12, 4), entry.flags);
    store_le<std::uint64_t>(page.bytes().subspan(offset + 16, 8), entry.current_epoch);
    store_le<std::uint64_t>(page.bytes().subspan(offset + 24, 8), entry.previous_page);
    store_le<std::uint16_t>(page.bytes().subspan(offset + 32, 2), entry.previous_slot);
    store_le<std::uint16_t>(page.bytes().subspan(offset + 34, 2), entry.previous_generation);
    store_le<std::uint32_t>(page.bytes().subspan(offset + 36, 4), entry.previous_flags);
    store_le<std::uint64_t>(page.bytes().subspan(offset + 40, 8), entry.previous_epoch);
}

// Monta uma página nova com a assinatura pedida (demais bytes já são zero).
Page make_header_page(const std::array<std::byte, 4>& magic, std::uint16_t version) {
    Page page;
    std::copy(magic.begin(), magic.end(), page.bytes().begin());
    store_le<std::uint16_t>(page.bytes().subspan(4, 2), version);
    return page;
}

// Confere a assinatura de uma página do mapa.
Result<void> check_magic(const Page& page, const std::array<std::byte, 4>& magic,
                         std::uint16_t expected_version, std::string_view what) {
    if (!std::equal(magic.begin(), magic.end(), page.bytes().begin())) {
        return std::unexpected(
            Error{ErrorCode::invalid_page_format, "identity map " + std::string{what} +
                                                      " page is missing its signature"});
    }
    const auto version = load_le<std::uint16_t>(page.bytes().subspan(4, 2));
    if (version != expected_version) {
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
    if (auto written = file.write(*page_id, make_header_page(idmd_magic, idmd_version)); !written) {
        return std::unexpected(written.error());
    }
    return IdentityMap{file, *page_id};
}

Result<IdentityMap> IdentityMap::open(storage::PageFile& file, storage::PageId directory_root) {
    auto page = file.read(directory_root);
    if (!page) {
        return std::unexpected(page.error());
    }
    if (auto valid = check_magic(*page, idmd_magic, idmd_version, "directory"); !valid) {
        return std::unexpected(valid.error());
    }
    // O diretório mantém o layout v1; a versão relevante está em cada IDMP.
    // Um mapa vazio já é compatível: a primeira página de entradas será v2.
    PageId current = directory_root;
    for (std::uint64_t directory_ordinal = 0; directory_ordinal < file.page_count();
         ++directory_ordinal) {
        auto directory = file.read(current);
        if (!directory) {
            return std::unexpected(directory.error());
        }
        if (auto valid = check_magic(*directory, idmd_magic, idmd_version, "directory"); !valid) {
            return std::unexpected(valid.error());
        }
        for (std::uint64_t slot = 0; slot < pointers_per_idmd; ++slot) {
            const auto pointer = load_le<std::uint64_t>(
                directory->bytes().subspan(idm_header_size + static_cast<std::size_t>(slot) *
                                                               dir_pointer_size,
                                           dir_pointer_size));
            if (pointer == 0) {
                continue;
            }
            auto idmp = file.read(PageId{pointer});
            if (!idmp) {
                return std::unexpected(idmp.error());
            }
            if (!std::equal(idmp_magic.begin(), idmp_magic.end(), idmp->bytes().begin())) {
                return std::unexpected(Error{ErrorCode::invalid_page_format,
                                             "identity map entries page is missing its signature"});
            }
            const auto version = load_le<std::uint16_t>(idmp->bytes().subspan(4, 2));
            if (version == legacy_idmp_version) {
                return migrate_v1(file, directory_root);
            }
            if (version != idmp_version) {
                return std::unexpected(Error{ErrorCode::incompatible_page_version,
                                             "unsupported identity map entries page version"});
            }
        }
        const auto next = load_le<std::uint64_t>(directory->bytes().subspan(next_dir_offset, 8));
        if (next == 0) {
            return IdentityMap{file, directory_root};
        }
        current = PageId{next};
    }
    return std::unexpected(Error{ErrorCode::corrupt_page, "identity map directory cycle detected"});
}

Result<IdentityMap> IdentityMap::migrate_v1(storage::PageFile& file,
                                             storage::PageId directory_root) {
    auto migrated = IdentityMap::create(file);
    if (!migrated) {
        return std::unexpected(migrated.error());
    }
    constexpr std::uint64_t legacy_entries_per_idmp =
        (storage::page_size - idm_header_size) / legacy_idmp_entry_size;
    PageId current = directory_root;
    for (std::uint64_t directory_ordinal = 0; directory_ordinal < file.page_count();
         ++directory_ordinal) {
        auto directory = file.read(current);
        if (!directory) {
            return std::unexpected(directory.error());
        }
        if (auto valid = check_magic(*directory, idmd_magic, idmd_version, "directory"); !valid) {
            return std::unexpected(valid.error());
        }
        for (std::uint64_t slot = 0; slot < pointers_per_idmd; ++slot) {
            const auto pointer = load_le<std::uint64_t>(
                directory->bytes().subspan(idm_header_size + static_cast<std::size_t>(slot) *
                                                               dir_pointer_size,
                                           dir_pointer_size));
            if (pointer == 0) {
                continue;
            }
            auto legacy = file.read(PageId{pointer});
            if (!legacy) {
                return std::unexpected(legacy.error());
            }
            if (auto valid = check_magic(*legacy, idmp_magic, legacy_idmp_version, "legacy entries");
                !valid) {
                return std::unexpected(valid.error());
            }
            const auto legacy_page_ordinal = directory_ordinal * pointers_per_idmd + slot;
            for (std::uint64_t index = 0; index < legacy_entries_per_idmp; ++index) {
                const auto old = read_legacy_entry(*legacy, index);
                if (!old.allocated()) {
                    continue;
                }
                const ObjectId id{legacy_page_ordinal * legacy_entries_per_idmp + index};
                const auto new_entry_page = id.value / entries_per_idmp;
                const auto new_entry_index = id.value % entries_per_idmp;
                auto idmp_id = migrated->ensure_idmp(new_entry_page);
                if (!idmp_id) {
                    return std::unexpected(idmp_id.error());
                }
                auto page = file.read(*idmp_id);
                if (!page) {
                    return std::unexpected(page.error());
                }
                write_entry(*page, new_entry_index,
                            Entry{old.page, old.slot, old.generation, old.flags});
                if (auto written = file.write(*idmp_id, *page); !written) {
                    return std::unexpected(written.error());
                }
            }
        }
        const auto next = load_le<std::uint64_t>(directory->bytes().subspan(next_dir_offset, 8));
        if (next == 0) {
            return *migrated;
        }
        current = PageId{next};
    }
    return std::unexpected(Error{ErrorCode::corrupt_page, "identity map directory cycle detected"});
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
            if (auto written = file_->write(*new_dir, make_header_page(idmd_magic, idmd_version)); !written) {
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
        if (auto written = file_->write(*new_idmp, make_header_page(idmp_magic, idmp_version)); !written) {
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

Result<void> IdentityMap::bind(ObjectId id, storage::RecordId record, std::uint64_t epoch) {
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
    if (auto valid = check_magic(*page, idmp_magic, idmp_version, "entries"); !valid) {
        return std::unexpected(valid.error());
    }
    const auto existing = read_entry(*page, entry_index);
    if (existing.allocated()) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
                                     "ObjectId " + std::to_string(id.value) + " is already bound"});
    }
    // Um objeto novo não tem versão anterior: previous fica zerado/ausente.
    Entry entry{record.page.value, record.slot.value, record.generation, flag_allocated};
    entry.current_epoch = epoch;
    write_entry(*page, entry_index, entry);
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
    if (auto valid = check_magic(*page, idmp_magic, idmp_version, "entries"); !valid) {
        return std::unexpected(valid.error());
    }
    const auto entry = read_entry(*page, entry_index);
    if (!entry.allocated() || entry.removed()) {
        return std::unexpected(
            Error{ErrorCode::record_not_found, "no object with id " + std::to_string(id.value)});
    }
    return RecordId{PageId{entry.page}, storage::SlotId{entry.slot}, entry.generation};
}

Result<storage::RecordId> IdentityMap::find_at(ObjectId id, std::uint64_t snapshot_epoch) const {
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
    if (auto valid = check_magic(*page, idmp_magic, idmp_version, "entries"); !valid) {
        return std::unexpected(valid.error());
    }
    const auto entry = read_entry(*page, entry_index);
    if (!entry.allocated()) {
        return std::unexpected(
            Error{ErrorCode::record_not_found, "no object with id " + std::to_string(id.value)});
    }
    // A regra do snapshot (doc §Fase 6B): current serve se já valia na época
    // pedida; senão cai para previous, se este também já valia; senão ausente.
    if (entry.current_epoch <= snapshot_epoch) {
        if (entry.removed()) {
            return std::unexpected(Error{
                ErrorCode::record_not_found,
                "object " + std::to_string(id.value) + " was already removed at this epoch"});
        }
        return RecordId{PageId{entry.page}, storage::SlotId{entry.slot}, entry.generation};
    }
    if ((entry.previous_flags & flag_allocated) != 0U && entry.previous_epoch <= snapshot_epoch) {
        return RecordId{PageId{entry.previous_page}, storage::SlotId{entry.previous_slot},
                        entry.previous_generation};
    }
    return std::unexpected(Error{
        ErrorCode::record_not_found,
        "object " + std::to_string(id.value) + " did not exist yet at this epoch"});
}

Result<std::uint64_t> IdentityMap::current_epoch(ObjectId id) const {
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
    if (auto valid = check_magic(*page, idmp_magic, idmp_version, "entries"); !valid) {
        return std::unexpected(valid.error());
    }
    const auto entry = read_entry(*page, entry_index);
    if (!entry.allocated() || entry.removed()) {
        return std::unexpected(
            Error{ErrorCode::record_not_found, "no object with id " + std::to_string(id.value)});
    }
    return entry.current_epoch;
}

Result<bool> IdentityMap::has_previous(ObjectId id) const {
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
    if (auto valid = check_magic(*page, idmp_magic, idmp_version, "entries"); !valid) {
        return std::unexpected(valid.error());
    }
    const auto entry = read_entry(*page, entry_index);
    if (!entry.allocated() || entry.removed()) {
        return std::unexpected(
            Error{ErrorCode::record_not_found, "no object with id " + std::to_string(id.value)});
    }
    return (entry.previous_flags & flag_allocated) != 0U;
}

Result<void> IdentityMap::rebind(ObjectId id, storage::RecordId record, std::uint64_t epoch) {
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
    if (auto valid = check_magic(*page, idmp_magic, idmp_version, "entries"); !valid) {
        return std::unexpected(valid.error());
    }
    auto entry = read_entry(*page, entry_index);
    if (!entry.allocated() || entry.removed()) {
        return std::unexpected(
            Error{ErrorCode::record_not_found, "no object with id " + std::to_string(id.value)});
    }
    // Move a versão current (mecânico: quem decidiu que isto é seguro é o
    // chamador) para previous, e grava a nova localização como current.
    Entry next;
    next.page = record.page.value;
    next.slot = record.slot.value;
    next.generation = record.generation;
    next.flags = flag_allocated;
    next.current_epoch = epoch;
    next.previous_page = entry.page;
    next.previous_slot = entry.slot;
    next.previous_generation = entry.generation;
    next.previous_flags = flag_allocated;
    next.previous_epoch = entry.current_epoch;
    write_entry(*page, entry_index, next);
    return file_->write(**idmp_id, *page);
}

Result<void> IdentityMap::erase(ObjectId id, std::uint64_t epoch) {
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
    if (auto valid = check_magic(*page, idmp_magic, idmp_version, "entries"); !valid) {
        return std::unexpected(valid.error());
    }
    auto entry = read_entry(*page, entry_index);
    if (!entry.allocated() || entry.removed()) {
        return std::unexpected(
            Error{ErrorCode::record_not_found, "no object with id " + std::to_string(id.value)});
    }
    // Move current -> previous (o registro físico antigo continua alcançável
    // por quem tiver um snapshot anterior a esta época) e marca current como
    // removido, sem localização física própria.
    Entry next;
    next.page = 0;
    next.slot = 0;
    next.generation = 0;
    next.flags = flag_allocated | flag_removed;
    next.current_epoch = epoch;
    next.previous_page = entry.page;
    next.previous_slot = entry.slot;
    next.previous_generation = entry.generation;
    next.previous_flags = flag_allocated;
    next.previous_epoch = entry.current_epoch;
    write_entry(*page, entry_index, next);
    return file_->write(**idmp_id, *page);
}

} // namespace modb::object
