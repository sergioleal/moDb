// Importa a API pública do verificador de integridade.
#include "modb/storage/database_check.hpp"
// Importa PageFile para a abertura em L1.
#include "modb/storage/page_file.hpp"
// Importa SlottedPage para validar páginas SLPG em L2.
#include "modb/storage/slotted_page.hpp"
// Importa TableHeap para validar cadeias THRP em L3.
#include "modb/storage/table_heap.hpp"

// Disponibiliza comparação dos magic bytes.
#include <algorithm>
// Disponibiliza blocos fixos das assinaturas conhecidas.
#include <array>
// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza std::to_string nas mensagens de L3.
#include <string>
// Disponibiliza o conjunto de páginas reivindicadas por cada heap.
#include <unordered_set>
// Disponibiliza std::move.
#include <utility>

namespace modb::storage {
namespace {

// Assinatura das slotted pages documentada no formato.
constexpr std::array<std::byte, 4> slotted_magic{
    std::byte{'S'}, std::byte{'L'}, std::byte{'P'}, std::byte{'G'}};
// Assinatura das raízes dedicadas de TableHeap.
constexpr std::array<std::byte, 4> table_heap_root_magic{
    std::byte{'T'}, std::byte{'H'}, std::byte{'R'}, std::byte{'P'}};
// Assinaturas das páginas do modelo de objetos (ADR-004/ADR-005).
constexpr std::array<std::byte, 4> database_root_magic{
    std::byte{'D'}, std::byte{'B'}, std::byte{'R'}, std::byte{'T'}};
constexpr std::array<std::byte, 4> identity_directory_magic{
    std::byte{'I'}, std::byte{'D'}, std::byte{'M'}, std::byte{'D'}};
constexpr std::array<std::byte, 4> identity_entries_magic{
    std::byte{'I'}, std::byte{'D'}, std::byte{'M'}, std::byte{'P'}};
// Assinatura das páginas de blob (Fase 4). O formato do blob pertence à camada
// de objetos, mas o verificador de armazenamento reconhece a página pelos
// mesmos magic bytes que já usa para DBRT/IDMD/IDMP.
constexpr std::array<std::byte, 4> blob_magic{
    std::byte{'B'}, std::byte{'L'}, std::byte{'B'}, std::byte{'P'}};

// Campos do cabeçalho de blob validáveis sem conhecer o conteúdo.
constexpr std::uint16_t blob_page_version = 1;
constexpr std::size_t blob_header_bytes = 24;
constexpr std::size_t blob_payload_capacity = page_size - blob_header_bytes;

// Compara os quatro primeiros bytes com um magic conhecido.
bool starts_with_magic(const Page& page, const std::array<std::byte, 4>& magic) noexcept {
    return std::equal(magic.begin(), magic.end(), page.bytes().begin());
}

// Lê um u16/u32 little-endian a partir de um offset já validado do cabeçalho.
std::uint16_t read_u16_at(const Page& page, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(page[offset]) |
                                      (std::to_integer<std::uint16_t>(page[offset + 1]) << 8));
}
std::uint32_t read_u32_at(const Page& page, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= std::to_integer<std::uint32_t>(page[offset + i]) << (8 * i);
    }
    return value;
}
std::uint64_t read_u64_at(const Page& page, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= std::to_integer<std::uint64_t>(page[offset + i]) << (8 * i);
    }
    return value;
}

// Valida os campos do cabeçalho BLBP que não dependem do conteúdo: versão e
// comprimento declarado. A cadeia (next/ciclo) e as refs órfãs são semânticas
// e ficam para o verificador da camada de objetos (ver nota da Fase 4).
Result<void> validate_blob_page(const Page& page) {
    const auto version = read_u16_at(page, 4);
    if (version != blob_page_version) {
        return std::unexpected(Error{ErrorCode::incompatible_page_version,
                                     "blob page has an unsupported version"});
    }
    const auto payload_length = read_u32_at(page, 16);
    if (payload_length > blob_payload_capacity) {
        return std::unexpected(
            Error{ErrorCode::corrupt_page, "blob page payload length exceeds page capacity"});
    }
    return {};
}

// Identifica páginas alocadas que ainda não receberam formato.
bool is_all_zeros(const Page& page) noexcept {
    return std::all_of(page.bytes().begin(), page.bytes().end(),
                       [](std::byte value) { return value == std::byte{0}; });
}

} // namespace

// Classifica pelos magic bytes ou pelo padrão zerado completo.
PageKind classify_page(const Page& page) noexcept {
    if (is_all_zeros(page)) {
        return PageKind::unformatted;
    }
    if (starts_with_magic(page, slotted_magic)) {
        return PageKind::slotted;
    }
    if (starts_with_magic(page, table_heap_root_magic)) {
        return PageKind::table_heap_root;
    }
    if (starts_with_magic(page, database_root_magic)) {
        return PageKind::database_root;
    }
    if (starts_with_magic(page, identity_directory_magic)) {
        return PageKind::identity_directory;
    }
    if (starts_with_magic(page, identity_entries_magic)) {
        return PageKind::identity_entries;
    }
    if (starts_with_magic(page, blob_magic)) {
        return PageKind::blob;
    }
    return PageKind::unknown;
}

// Percorre L1–L3 e devolve o inventário completo do arquivo.
Result<DatabaseCheckReport> check_database(const std::filesystem::path& path) {
    // L1: rejeita imediatamente arquivos com superbloco inválido.
    auto file = PageFile::open(path);
    if (!file) {
        return std::unexpected(file.error());
    }

    DatabaseCheckReport report;
    report.page_count = file->page_count();
    report.pages.reserve(report.page_count > 0 ? report.page_count - 1 : 0);

    // L2: classifica e valida cada página de dados.
    for (std::uint64_t value = 1; value < report.page_count; ++value) {
        const PageId id{value};
        auto page = file->read(id);
        if (!page) {
            report.pages.push_back(PageCheckResult{id, PageKind::unknown, page.error()});
            continue;
        }

        const auto kind = classify_page(*page);
        PageCheckResult entry{id, kind, std::nullopt};
        switch (kind) {
        case PageKind::unformatted:
            break;
        case PageKind::slotted: {
            auto slotted = SlottedPage::from_page(std::move(*page));
            if (!slotted) {
                entry.error = slotted.error();
            }
            break;
        }
        case PageKind::table_heap_root: {
            if (auto validated = validate_table_heap_root(*page); !validated) {
                entry.error = validated.error();
            }
            break;
        }
        case PageKind::database_root:
            {
            const auto version = read_u16_at(*page, 4);
            if (version != 1 && version != 2) {
                entry.error = Error{ErrorCode::incompatible_page_version,
                                    "database root page has an unsupported version"};
                break;
            }
            ObjectFormatCheck format;
            format.dbrt_version = version;
            if (version == 2) {
                format.epoch = read_u64_at(*page, 48);
            }
            report.object_format = format;
            break;
            }
        case PageKind::identity_directory:
            // Páginas do modelo de objetos são reconhecidas estruturalmente;
            // a validação profunda das suas cadeias fica para um nível futuro.
            break;
        case PageKind::identity_entries: {
            const auto version = read_u16_at(*page, 4);
            if (version != 1 && version != 2) {
                entry.error = Error{ErrorCode::incompatible_page_version,
                                    "identity map entries page has an unsupported version"};
                break;
            }
            if (report.object_format) {
                report.object_format->idmp_version = version;
            }
            break;
        }
        case PageKind::blob:
            // Valida o cabeçalho BLBP (versão e comprimento); a cadeia e as
            // refs órfãs são semânticas (verificador da camada de objetos).
            if (auto validated = validate_blob_page(*page); !validated) {
                entry.error = validated.error();
            }
            break;
        case PageKind::unknown:
            entry.error = Error{
                ErrorCode::invalid_page_format,
                "page " + std::to_string(id.value) + " has an unrecognized format",
            };
            break;
        case PageKind::superblock:
            entry.error = Error{
                ErrorCode::corrupt_page,
                "page " + std::to_string(id.value) + " was classified as a superblock",
            };
            break;
        }
        report.pages.push_back(std::move(entry));
    }

    // L3: abre cada raiz THRP válida, valida a cadeia e detecta compartilhamento.
    // Uma página de dados só pode pertencer a um heap; se duas raízes a reivindicam,
    // escrever por um heap corromperia o outro.
    std::unordered_set<std::uint64_t> claimed_pages;
    for (const auto& entry : report.pages) {
        if (entry.kind != PageKind::table_heap_root || entry.error) {
            continue;
        }
        auto heap = TableHeap::open(*file, entry.id);
        if (!heap) {
            report.heap_errors.push_back(Error{
                heap.error().code,
                "TableHeap root page " + std::to_string(entry.id.value) + ": " +
                    heap.error().message,
            });
            continue;
        }
        // Reúne as páginas de dados desta cadeia para verificar sobreposições.
        auto pages = heap->layout();
        if (!pages) {
            report.heap_errors.push_back(Error{
                pages.error().code,
                "TableHeap root page " + std::to_string(entry.id.value) + ": " +
                    pages.error().message,
            });
            continue;
        }
        for (const auto& page : *pages) {
            if (!claimed_pages.insert(page.id.value).second) {
                report.heap_errors.push_back(Error{
                    ErrorCode::corrupt_page,
                    "data page " + std::to_string(page.id.value) +
                        " is shared by more than one TableHeap chain",
                });
            }
        }
    }

    // L4: confere que cada registro vivo é legível dentro dos limites da sua
    // página. A validação semântica do conteúdo (é uma linha? um objeto?)
    // pertence à camada que possui os registros, não ao verificador de
    // armazenamento — que não conhece, de propósito, o formato do payload.
    for (const auto& entry : report.pages) {
        if (entry.kind != PageKind::slotted || entry.error) {
            continue;
        }
        auto page = file->read(entry.id);
        if (!page) {
            continue;
        }
        auto slotted = SlottedPage::from_page(std::move(*page));
        if (!slotted) {
            continue;
        }
        for (const auto& slot : slotted->slots()) {
            if (!slot.occupied()) {
                continue;
            }
            if (auto bytes = slotted->read(slot.id); !bytes) {
                report.record_errors.push_back(RecordCheckError{entry.id, slot.id, bytes.error()});
            }
        }
    }

    return report;
}

} // namespace modb::storage
