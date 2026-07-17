// Importa a interface do BlobStore.
#include "modb/object/blob_store.hpp"

// Importa BinaryWriter/BinaryReader para montar e ler o cabeçalho de página.
#include "modb/storage/binary.hpp"

// Disponibiliza std::copy ao materializar o cabeçalho na página.
#include <algorithm>
// Disponibiliza os blocos fixos da assinatura.
#include <array>
// Disponibiliza std::to_string em diagnósticos.
#include <string>
// Disponibiliza o conjunto de páginas visitadas (detecção de ciclo).
#include <unordered_set>
// Disponibiliza std::move.
#include <utility>

namespace modb::object {
namespace {

// Assinatura das páginas de blob.
constexpr std::array<std::byte, 4> blob_magic{
    std::byte{'B'}, std::byte{'L'}, std::byte{'B'}, std::byte{'P'}};

// Resultado da leitura do cabeçalho de uma página de blob.
struct BlobHeader {
    storage::PageId next;
    std::uint32_t length;
};

// Valida a assinatura, a versão e o comprimento declarado de uma página.
Result<BlobHeader> parse_header(const storage::Page& page) {
    if (!std::equal(blob_magic.begin(), blob_magic.end(), page.bytes().begin())) {
        return std::unexpected(
            Error{ErrorCode::invalid_page_format, "page is not a blob page (bad magic)"});
    }
    // Lê os campos do cabeçalho a partir do byte logo após o magic.
    storage::BinaryReader reader{
        std::span<const std::byte>{page.bytes()}.subspan(blob_magic.size())};
    auto version = reader.read_u16();
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version != blob_page_version) {
        return std::unexpected(Error{ErrorCode::incompatible_page_version,
                                     "unsupported blob page version: " + std::to_string(*version)});
    }
    if (auto reserved = reader.read_u16(); !reserved) { // reservado u16
        return std::unexpected(reserved.error());
    }
    auto next = reader.read_u64();
    if (!next) {
        return std::unexpected(next.error());
    }
    auto length = reader.read_u32();
    if (!length) {
        return std::unexpected(length.error());
    }
    if (*length > blob_page_capacity) {
        return std::unexpected(Error{ErrorCode::corrupt_page,
                                     "blob page payload length exceeds page capacity"});
    }
    return BlobHeader{storage::PageId{*next}, *length};
}

// Monta uma página de blob com o cabeçalho e a fatia de dados fornecidos.
storage::Page build_page(storage::PageId next, std::span<const std::byte> chunk) {
    storage::BinaryWriter writer;
    writer.write_bytes(blob_magic);
    writer.write_u16(blob_page_version);
    writer.write_u16(0); // reservado
    writer.write_u64(next.value);
    writer.write_u32(static_cast<std::uint32_t>(chunk.size()));
    writer.write_u32(0); // reservado
    const auto header = std::move(writer).take();

    storage::Page page;
    std::copy(header.begin(), header.end(), page.bytes().begin());
    std::copy(chunk.begin(), chunk.end(), page.bytes().begin() + blob_header_size);
    return page;
}

} // namespace

Result<void> BlobStore::require_write_transaction() const {
    if (transaction_required_ && !file_->in_transaction()) {
        return std::unexpected(
            Error{ErrorCode::transaction_required, "blob writes require an active transaction"});
    }
    return {};
}

Result<BlobId> BlobStore::create(std::span<const std::byte> data) {
    if (auto ready = require_write_transaction(); !ready) {
        return std::unexpected(ready.error());
    }
    // Um blob sempre ocupa ao menos uma página, mesmo vazio.
    const std::size_t pages_needed =
        data.empty() ? 1 : (data.size() + blob_page_capacity - 1) / blob_page_capacity;

    // Aloca todas as páginas primeiro para conhecer os ids dos ponteiros next.
    std::vector<storage::PageId> ids;
    ids.reserve(pages_needed);
    for (std::size_t index = 0; index < pages_needed; ++index) {
        auto id = file_->allocate_page();
        if (!id) {
            return std::unexpected(id.error());
        }
        ids.push_back(*id);
    }

    for (std::size_t index = 0; index < pages_needed; ++index) {
        const std::size_t offset = index * blob_page_capacity;
        const std::size_t remaining = data.size() - std::min(offset, data.size());
        const std::size_t take = std::min(blob_page_capacity, remaining);
        const auto chunk = data.subspan(offset, take);
        const storage::PageId next =
            (index + 1 < pages_needed) ? ids[index + 1] : storage::PageId{0};
        if (auto written = file_->write(ids[index], build_page(next, chunk)); !written) {
            return std::unexpected(written.error());
        }
    }
    return BlobId{ids.front().value};
}

Result<void> BlobStore::read_chunks(
    BlobId id, const std::function<Result<void>(std::span<const std::byte>)>& visitor) const {
    if (id.value == 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "null blob id"});
    }
    std::unordered_set<std::uint64_t> visited;
    storage::PageId current{id.value};
    while (current.value != 0) {
        if (!visited.insert(current.value).second) {
            return std::unexpected(
                Error{ErrorCode::page_chain_cycle, "blob chain revisits a page"});
        }
        auto page = file_->read(current);
        if (!page) {
            return std::unexpected(page.error());
        }
        auto header = parse_header(*page);
        if (!header) {
            return std::unexpected(header.error());
        }
        const auto chunk =
            std::span<const std::byte>{page->bytes()}.subspan(blob_header_size, header->length);
        if (auto visited_result = visitor(chunk); !visited_result) {
            return std::unexpected(visited_result.error());
        }
        current = header->next;
    }
    return {};
}

Result<std::vector<std::byte>> BlobStore::read(BlobId id) const {
    std::vector<std::byte> out;
    auto result = read_chunks(id, [&out](std::span<const std::byte> chunk) -> Result<void> {
        out.insert(out.end(), chunk.begin(), chunk.end());
        return {};
    });
    if (!result) {
        return std::unexpected(result.error());
    }
    return out;
}

Result<BlobId> BlobStore::rewrite(BlobId id, std::span<const std::byte> data) {
    if (auto ready = require_write_transaction(); !ready) {
        return std::unexpected(ready.error());
    }
    if (id.value == 0) {
        return create(data);
    }
    // Coleta a cadeia antiga (validando magic/versão e detectando ciclos) para
    // reaproveitar as páginas e manter a primeira estável.
    std::vector<storage::PageId> old_ids;
    {
        std::unordered_set<std::uint64_t> visited;
        storage::PageId current{id.value};
        while (current.value != 0) {
            if (!visited.insert(current.value).second) {
                return std::unexpected(
                    Error{ErrorCode::page_chain_cycle, "blob chain revisits a page"});
            }
            auto page = file_->read(current);
            if (!page) {
                return std::unexpected(page.error());
            }
            auto header = parse_header(*page);
            if (!header) {
                return std::unexpected(header.error());
            }
            old_ids.push_back(current);
            current = header->next;
        }
    }

    const std::size_t pages_needed =
        data.empty() ? 1 : (data.size() + blob_page_capacity - 1) / blob_page_capacity;

    // Reaproveita as páginas antigas e aloca as que faltarem.
    std::vector<storage::PageId> ids;
    ids.reserve(pages_needed);
    for (std::size_t index = 0; index < pages_needed; ++index) {
        if (index < old_ids.size()) {
            ids.push_back(old_ids[index]);
        } else {
            auto allocated = file_->allocate_page();
            if (!allocated) {
                return std::unexpected(allocated.error());
            }
            ids.push_back(*allocated);
        }
    }

    for (std::size_t index = 0; index < pages_needed; ++index) {
        const std::size_t offset = index * blob_page_capacity;
        const std::size_t remaining = data.size() - std::min(offset, data.size());
        const std::size_t take = std::min(blob_page_capacity, remaining);
        const auto chunk = data.subspan(offset, take);
        const storage::PageId next =
            (index + 1 < pages_needed) ? ids[index + 1] : storage::PageId{0};
        if (auto written = file_->write(ids[index], build_page(next, chunk)); !written) {
            return std::unexpected(written.error());
        }
    }

    // Zera as páginas antigas que sobraram (órfãs no MVP: sem free list).
    for (std::size_t index = pages_needed; index < old_ids.size(); ++index) {
        if (auto zeroed = file_->write(old_ids[index], storage::Page{}); !zeroed) {
            return std::unexpected(zeroed.error());
        }
    }
    return BlobId{ids.front().value};
}

Result<void> BlobStore::remove(BlobId id) {
    if (auto ready = require_write_transaction(); !ready) {
        return std::unexpected(ready.error());
    }
    if (id.value == 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "null blob id"});
    }
    std::unordered_set<std::uint64_t> visited;
    storage::PageId current{id.value};
    while (current.value != 0) {
        if (!visited.insert(current.value).second) {
            return std::unexpected(
                Error{ErrorCode::page_chain_cycle, "blob chain revisits a page"});
        }
        auto page = file_->read(current);
        if (!page) {
            return std::unexpected(page.error());
        }
        auto header = parse_header(*page);
        if (!header) {
            return std::unexpected(header.error());
        }
        const storage::PageId next = header->next;
        if (auto zeroed = file_->write(current, storage::Page{}); !zeroed) {
            return std::unexpected(zeroed.error());
        }
        current = next;
    }
    return {};
}

} // namespace modb::object
