// Importa a API pública do verificador de integridade.
#include "modb/storage/database_check.hpp"
// Importa a decodificação de linha usada no nível L4.
#include "modb/storage/codec.hpp"
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

// Compara os quatro primeiros bytes com um magic conhecido.
bool starts_with_magic(const Page& page, const std::array<std::byte, 4>& magic) noexcept {
    return std::equal(magic.begin(), magic.end(), page.bytes().begin());
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

    // L4: decodifica cada registro vivo para detectar payloads corrompidos que
    // passam pela validação estrutural mas explodiriam na leitura pelo usuário.
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
            auto bytes = slotted->read(slot.id);
            if (!bytes) {
                report.record_errors.push_back(RecordCheckError{entry.id, slot.id, bytes.error()});
                continue;
            }
            if (auto row = decode_row(*bytes); !row) {
                report.record_errors.push_back(RecordCheckError{entry.id, slot.id, row.error()});
            }
        }
    }

    return report;
}

} // namespace modb::storage
