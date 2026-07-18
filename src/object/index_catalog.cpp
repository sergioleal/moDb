// Implementação do diretório de índices (Fase 7B): cadeia de páginas IXDR.
#include "modb/object/index_catalog.hpp"

// Importa store_le/load_le para os campos da página.
#include "modb/storage/endian.hpp"

// Disponibiliza std::equal ao validar o magic.
#include <algorithm>
// Disponibiliza a assinatura fixa.
#include <array>
// Disponibiliza std::byte / std::size_t.
#include <cstddef>
// Disponibiliza std::memcpy.
#include <cstring>
// Disponibiliza std::move.
#include <utility>

namespace modb::object {
namespace {

using storage::load_le;
using storage::Page;
using storage::PageId;
using storage::store_le;

constexpr std::array<std::byte, 4> ixdr_magic{std::byte{'I'}, std::byte{'X'}, std::byte{'D'},
                                              std::byte{'R'}};
constexpr std::uint16_t ixdr_version = 1;
constexpr std::size_t header_size = 16;  // magic(4) version(2) count(2) next(8)
constexpr std::size_t off_next = 8;

std::size_t entry_bytes(const IndexInfo& info) {
    return 2 + info.type_name.size() + 2 + 8;
}

Result<void> write_directory_page(storage::PageFile& file, PageId id,
                                  const std::vector<IndexInfo>& entries, PageId next) {
    Page page;
    std::copy(ixdr_magic.begin(), ixdr_magic.end(), page.bytes().begin());
    store_le<std::uint16_t>(page.bytes().subspan(4, 2), ixdr_version);
    store_le<std::uint16_t>(page.bytes().subspan(6, 2), static_cast<std::uint16_t>(entries.size()));
    store_le<std::uint64_t>(page.bytes().subspan(off_next, 8), next.value);
    std::size_t offset = header_size;
    auto bytes = page.bytes();
    for (const auto& info : entries) {
        store_le<std::uint16_t>(bytes.subspan(offset, 2),
                                static_cast<std::uint16_t>(info.type_name.size()));
        offset += 2;
        if (!info.type_name.empty()) {
            std::memcpy(&bytes[offset], info.type_name.data(), info.type_name.size());
        }
        offset += info.type_name.size();
        store_le<std::uint16_t>(bytes.subspan(offset, 2), info.field_id);
        offset += 2;
        store_le<std::uint64_t>(bytes.subspan(offset, 8), info.root.value);
        offset += 8;
    }
    return file.write(id, page);
}

} // namespace

Result<IndexCatalog> IndexCatalog::create(storage::PageFile& file) {
    auto page_id = file.allocate_page();
    if (!page_id) {
        return std::unexpected(page_id.error());
    }
    IndexCatalog catalog{file, *page_id, {}, {}};
    if (auto persisted = catalog.persist(); !persisted) {
        return std::unexpected(persisted.error());
    }
    return catalog;
}

Result<IndexCatalog> IndexCatalog::open(storage::PageFile& file, PageId directory) {
    std::vector<IndexInfo> indexes;
    std::vector<PageId> overflow;
    PageId current = directory;
    while (current.value != 0) {
        auto page = file.read(current);
        if (!page) {
            return std::unexpected(page.error());
        }
        if (!std::equal(ixdr_magic.begin(), ixdr_magic.end(), page->bytes().begin())) {
            return std::unexpected(Error{ErrorCode::invalid_page_format,
                                         "index directory page is missing its signature"});
        }
        const auto version = load_le<std::uint16_t>(page->bytes().subspan(4, 2));
        if (version != ixdr_version) {
            return std::unexpected(
                Error{ErrorCode::incompatible_page_version, "unsupported index directory version"});
        }
        const auto count = load_le<std::uint16_t>(page->bytes().subspan(6, 2));
        const auto next = PageId{load_le<std::uint64_t>(page->bytes().subspan(off_next, 8))};
        std::size_t offset = header_size;
        const auto bytes = page->bytes();
        for (std::uint16_t i = 0; i < count; ++i) {
            if (offset + 2 > storage::page_size) {
                return std::unexpected(
                    Error{ErrorCode::corrupt_page, "index directory is truncated"});
            }
            const auto name_len = load_le<std::uint16_t>(bytes.subspan(offset, 2));
            offset += 2;
            if (offset + name_len + 2 + 8 > storage::page_size) {
                return std::unexpected(
                    Error{ErrorCode::corrupt_page, "index directory entry overruns"});
            }
            IndexInfo info;
            info.type_name.assign(reinterpret_cast<const char*>(&bytes[offset]), name_len);
            offset += name_len;
            info.field_id = load_le<std::uint16_t>(bytes.subspan(offset, 2));
            offset += 2;
            info.root = PageId{load_le<std::uint64_t>(bytes.subspan(offset, 8))};
            offset += 8;
            indexes.push_back(std::move(info));
        }
        if (current != directory) {
            overflow.push_back(current);
        }
        current = next;
    }
    return IndexCatalog{file, directory, std::move(indexes), std::move(overflow)};
}

int IndexCatalog::find(std::string_view type_name, std::uint16_t field_id) const noexcept {
    for (std::size_t i = 0; i < indexes_.size(); ++i) {
        if (indexes_[i].type_name == type_name && indexes_[i].field_id == field_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

Result<void> IndexCatalog::add(IndexInfo info) {
    if (find(info.type_name, info.field_id) >= 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "index already exists"});
    }
    indexes_.push_back(std::move(info));
    if (auto persisted = persist(); !persisted) {
        indexes_.pop_back();
        return std::unexpected(persisted.error());
    }
    return {};
}

Result<void> IndexCatalog::set_root(std::size_t slot, PageId root) {
    if (slot >= indexes_.size()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "index slot out of range"});
    }
    const auto previous = indexes_[slot].root;
    indexes_[slot].root = root;
    if (auto persisted = persist(); !persisted) {
        indexes_[slot].root = previous;
        return std::unexpected(persisted.error());
    }
    return {};
}

Result<void> IndexCatalog::persist() {
    // Empacota os índices em páginas encadeadas; a primeira é sempre
    // `directory_` (apontada pelo DBRT). Páginas extras são reaproveitadas da
    // cadeia anterior ou alocadas; as sobras são zeradas (órfãs no MVP).
    std::vector<std::vector<IndexInfo>> pages;
    pages.emplace_back();
    std::size_t used = header_size;
    for (const auto& info : indexes_) {
        const auto need = entry_bytes(info);
        if (need > storage::page_size - header_size) {
            return std::unexpected(
                Error{ErrorCode::value_too_large, "index directory entry does not fit in a page"});
        }
        if (used + need > storage::page_size) {
            pages.emplace_back();
            used = header_size;
        }
        pages.back().push_back(info);
        used += need;
    }

    std::vector<PageId> page_ids;
    page_ids.reserve(pages.size());
    page_ids.push_back(directory_);
    for (std::size_t i = 1; i < pages.size(); ++i) {
        if (i - 1 < overflow_.size()) {
            page_ids.push_back(overflow_[i - 1]);
        } else {
            auto allocated = file_->allocate_page();
            if (!allocated) {
                return std::unexpected(allocated.error());
            }
            page_ids.push_back(*allocated);
        }
    }

    for (std::size_t i = 0; i < pages.size(); ++i) {
        const PageId next = (i + 1 < page_ids.size()) ? page_ids[i + 1] : PageId{0};
        if (auto written = write_directory_page(*file_, page_ids[i], pages[i], next); !written) {
            return written;
        }
    }

    // Zera páginas de overflow que sobraram da cadeia anterior.
    for (std::size_t i = pages.size() - 1; i < overflow_.size(); ++i) {
        if (auto zeroed = file_->write(overflow_[i], Page{}); !zeroed) {
            return zeroed;
        }
    }

    overflow_.assign(page_ids.begin() + 1, page_ids.end());
    return {};
}

} // namespace modb::object
