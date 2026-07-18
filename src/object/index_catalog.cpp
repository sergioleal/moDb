// Implementação do diretório de índices (Fase 7B).
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
constexpr std::size_t header_size = 16;  // magic(4) version(2) count(2) reservado(8)

} // namespace

Result<IndexCatalog> IndexCatalog::create(storage::PageFile& file) {
    auto page_id = file.allocate_page();
    if (!page_id) {
        return std::unexpected(page_id.error());
    }
    IndexCatalog catalog{file, *page_id, {}};
    if (auto persisted = catalog.persist(); !persisted) {
        return std::unexpected(persisted.error());
    }
    return catalog;
}

Result<IndexCatalog> IndexCatalog::open(storage::PageFile& file, storage::PageId directory) {
    auto page = file.read(directory);
    if (!page) {
        return std::unexpected(page.error());
    }
    if (!std::equal(ixdr_magic.begin(), ixdr_magic.end(), page->bytes().begin())) {
        return std::unexpected(
            Error{ErrorCode::invalid_page_format, "index directory page is missing its signature"});
    }
    const auto version = load_le<std::uint16_t>(page->bytes().subspan(4, 2));
    if (version != ixdr_version) {
        return std::unexpected(
            Error{ErrorCode::incompatible_page_version, "unsupported index directory version"});
    }
    const auto count = load_le<std::uint16_t>(page->bytes().subspan(6, 2));
    std::vector<IndexInfo> indexes;
    indexes.reserve(count);
    std::size_t offset = header_size;
    const auto bytes = page->bytes();
    for (std::uint16_t i = 0; i < count; ++i) {
        if (offset + 2 > storage::page_size) {
            return std::unexpected(Error{ErrorCode::corrupt_page, "index directory is truncated"});
        }
        const auto name_len = load_le<std::uint16_t>(bytes.subspan(offset, 2));
        offset += 2;
        if (offset + name_len + 2 + 8 > storage::page_size) {
            return std::unexpected(Error{ErrorCode::corrupt_page, "index directory entry overruns"});
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
    return IndexCatalog{file, directory, std::move(indexes)};
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

Result<void> IndexCatalog::set_root(std::size_t slot, storage::PageId root) {
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
    // Confere que tudo cabe em uma página antes de escrever qualquer coisa.
    std::size_t needed = header_size;
    for (const auto& info : indexes_) {
        needed += 2 + info.type_name.size() + 2 + 8;
    }
    if (needed > storage::page_size) {
        return std::unexpected(
            Error{ErrorCode::value_too_large, "too many indexes for a single directory page"});
    }

    Page page;
    std::copy(ixdr_magic.begin(), ixdr_magic.end(), page.bytes().begin());
    store_le<std::uint16_t>(page.bytes().subspan(4, 2), ixdr_version);
    store_le<std::uint16_t>(page.bytes().subspan(6, 2),
                            static_cast<std::uint16_t>(indexes_.size()));
    std::size_t offset = header_size;
    auto bytes = page.bytes();
    for (const auto& info : indexes_) {
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
    return file_->write(directory_, page);
}

} // namespace modb::object
