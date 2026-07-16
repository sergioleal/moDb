// Importa o arquivo paginado e o TableHeap exercitados pelo cenário.
#include "modb/storage/page_file.hpp"
#include "modb/storage/table_heap.hpp"

// Importa as verificações simples compartilhadas.
#include "test_support.hpp"

// Disponibiliza o registro binário com tamanho fixado pelo teste.
#include <array>
// Disponibiliza o relógio monotônico das medições.
#include <chrono>
// Disponibiliza std::byte e std::size_t.
#include <cstddef>
// Disponibiliza inteiros com largura definida.
#include <cstdint>
// Disponibiliza caminhos e remoção do banco temporário.
#include <filesystem>
// Disponibiliza formatação decimal dos tempos.
#include <iomanip>
// Disponibiliza a saída do relatório.
#include <iostream>
// Disponibiliza o mapa dos espaços liberados e suas gerações.
#include <map>
// Disponibiliza pares usados como chave de PageId e SlotId.
#include <utility>
// Disponibiliza std::error_code para a limpeza.
#include <system_error>
// Disponibiliza as coleções de RecordIds.
#include <vector>

namespace {

// Define a quantidade inicial solicitada para o cenário.
constexpr std::size_t initial_record_count = 10'000;
// Metade dos IDs iniciais é par e será removida.
constexpr std::size_t replacement_record_count = initial_record_count / 2;
// Os dez mil registros originais sempre possuem exatamente 63 bytes.
constexpr std::size_t initial_record_size = 63;
// O alvo original usa 63; a nova variante CMake define somente os substitutos como 65.
#if defined(MODB_REPLACEMENT_RECORD_SIZE)
constexpr std::size_t replacement_record_size = MODB_REPLACEMENT_RECORD_SIZE;
#else
constexpr std::size_t replacement_record_size = 63;
#endif
// A capacidade física é igual ao tamanho lógico.
constexpr std::size_t capacity_for_test(std::size_t size) {
    return size;
}
// Somente substitutos do mesmo tamanho cabem necessariamente em todos os slots liberados.
constexpr bool replacements_match_original_size =
    replacement_record_size == initial_record_size;
// Identifica um espaço físico sem incluir sua geração.
using RecordLocation = std::pair<std::uint64_t, std::uint16_t>;

// Remove automaticamente o banco temporário ao final do teste.
class TemporaryDatabase {
public:
    // Gera um caminho único na pasta temporária do sistema.
    TemporaryDatabase() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-table-heap-space-reuse-" +
                 std::to_string(replacement_record_size) + "-" + std::to_string(unique) +
                 ".modb");
    }

    // Evita deixar o arquivo de carga depois da execução.
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    // Retorna o caminho completo sem copiá-lo.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    // Mantém o caminho enquanto o teste estiver em execução.
    std::filesystem::path path_;
};

// Cria um registro de tamanho conhecido que contém seu ID nos oito primeiros bytes.
template <std::size_t Size>
std::array<std::byte, Size> make_record(std::uint64_t id) {
    // O ID precisa caber no registro usado por estes cenários.
    static_assert(Size >= sizeof(id));
    // O preenchimento facilita reconhecer dados diferentes em um dump.
    std::array<std::byte, Size> record;
    record.fill(std::byte{static_cast<std::uint8_t>(id & 0xffU)});
    // Persiste o ID em little-endian sem depender da ABI do C++.
    auto remaining = id;
    for (std::size_t index = 0; index < sizeof(id); ++index) {
        record[index] = std::byte{static_cast<std::uint8_t>(remaining & 0xffU)};
        remaining >>= 8U;
    }
    // O tamanho do tipo garante exatamente 63 bytes.
    return record;
}

// Forma a chave que ignora geração e identifica somente o espaço reutilizável.
RecordLocation location_of(modb::storage::RecordId id) {
    return {id.page.value, id.slot.value};
}

// Converte uma duração monotônica para segundos.
double seconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double>{duration}.count();
}

// Soma o espaço livre central de todas as páginas de um layout.
std::size_t total_free_space(const std::vector<modb::storage::TableHeapPageInfo>& layout) {
    // Começa sem nenhum byte acumulado.
    std::size_t total = 0;
    // Cada página já informa sua quantidade de bytes livres.
    for (const auto& page : layout) {
        total += page.free_space;
    }
    return total;
}

} // namespace

// Executa inserção, remoção dos pares e reutilização dos espaços.
int main() {
    // Evita repetir o namespace de armazenamento.
    using namespace modb::storage;

    // Acumula todas as verificações funcionais.
    TestSuite suite;
    // Prepara um banco isolado para esta execução.
    TemporaryDatabase database;
    std::cout << "Temporary space-reuse database: " << database.path() << '\n';

    // Guarda os endereços dos dez mil registros originais.
    std::vector<RecordId> original_ids;
    original_ids.reserve(initial_record_count);
    // Conserva a raiz para a validação após reabrir o arquivo.
    PageId root{};
    // Recebe os tempos de cada fase.
    double initial_insert_seconds = 0.0;
    double even_delete_seconds = 0.0;
    double replacement_insert_seconds = 0.0;
    // Conserva a quantidade física para provar que nenhuma página foi alocada novamente.
    std::size_t pages_after_initial_insert = 0;
    // Recebe a quantidade depois das reinserções para calcular páginas novas.
    std::size_t pages_after_replacement = 0;
    // Compara o espaço livre antes e depois dos registros maiores.
    std::size_t free_space_before_replacement = 0;
    std::size_t free_space_after_replacement = 0;

    // Cria o heap, executa as três fases e valida a reutilização.
    {
        auto file = PageFile::create(database.path());
        suite.check(file.has_value(), "space-reuse database is created");
        if (!file) {
            return suite.finish();
        }
        auto heap = TableHeap::create(*file);
        suite.check(heap.has_value(), "space-reuse TableHeap is created");
        if (!heap) {
            return suite.finish();
        }
        root = heap->root_page();

        // Insere dez mil registros de exatamente 63 bytes.
        const auto initial_insert_start = std::chrono::steady_clock::now();
        for (std::size_t id = 0; id < initial_record_count; ++id) {
            const auto record = make_record<initial_record_size>(id);
            auto inserted = heap->insert(record);
            suite.check(inserted.has_value(), "63-byte original record is inserted");
            if (!inserted) {
                return suite.finish();
            }
            original_ids.push_back(*inserted);
        }
        suite.check(file->flush().has_value(), "original records are flushed");
        initial_insert_seconds =
            seconds(std::chrono::steady_clock::now() - initial_insert_start);
        pages_after_initial_insert = file->page_count();

        // Guarda o layout completo que deve ser restaurado pelas reinserções.
        auto original_layout = heap->layout();
        suite.check(original_layout.has_value(), "original heap layout is available");
        if (!original_layout) {
            return suite.finish();
        }
        free_space_before_replacement = total_free_space(*original_layout);

        // Confere diretamente que cada registro usa somente seu tamanho lógico.
        for (const auto& page_info : *original_layout) {
            auto raw_page = file->read(page_info.id);
            suite.check(raw_page.has_value(), "original data page is read");
            if (!raw_page) {
                return suite.finish();
            }
            auto page = SlottedPage::from_page(std::move(*raw_page));
            suite.check(page.has_value(), "original data page is a valid slotted page");
            if (!page) {
                return suite.finish();
            }
            for (const auto& slot : page->slots()) {
                if (slot.occupied()) {
                    suite.check(slot.record_size == initial_record_size &&
                                    slot.record_capacity == initial_record_size,
                                "63-byte record reserves exactly 63 bytes");
                }
            }
        }

        // Mapeia cada espaço removido para a geração que não poderá ser reutilizada.
        std::map<RecordLocation, std::uint16_t> freed_locations;
        const auto even_delete_start = std::chrono::steady_clock::now();
        for (std::size_t id = 0; id < initial_record_count; id += 2) {
            const auto record_id = original_ids[id];
            freed_locations.emplace(location_of(record_id), record_id.generation);
            auto removed = heap->erase(record_id);
            suite.check(removed.has_value(), "even-ID record is erased");
            if (!removed) {
                return suite.finish();
            }
        }
        suite.check(file->flush().has_value(), "even-ID deletions are flushed");
        even_delete_seconds = seconds(std::chrono::steady_clock::now() - even_delete_start);
        suite.check(freed_locations.size() == replacement_record_count,
                    "exactly five thousand locations were freed");

        // O scan intermediário deve conter somente os cinco mil IDs ímpares.
        auto survivors = heap->scan();
        suite.check(survivors.has_value() && survivors->size() == replacement_record_count,
                    "five thousand odd-ID records survive");
        suite.check(file->page_count() == pages_after_initial_insert,
                    "deletions do not change the physical page count");

        // Insere outros cinco mil registros, reutilizando slots quando houver espaço na página.
        const auto replacement_insert_start = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < replacement_record_count; ++index) {
            const auto record =
                make_record<replacement_record_size>(initial_record_count + index);
            auto inserted = heap->insert(record);
            suite.check(inserted.has_value(), "replacement record is inserted");
            if (!inserted) {
                return suite.finish();
            }
            const auto location = location_of(*inserted);
            const auto freed = freed_locations.find(location);
            if (freed != freed_locations.end()) {
                suite.check(inserted->generation ==
                                static_cast<std::uint16_t>(freed->second + 1),
                            "reused slot advances its generation");
                freed_locations.erase(freed);
            } else if constexpr (replacements_match_original_size) {
                suite.check(false,
                            "same-size replacement uses a slot freed by an even ID");
            }
        }
        suite.check(file->flush().has_value(), "replacement records are flushed");
        replacement_insert_seconds =
            seconds(std::chrono::steady_clock::now() - replacement_insert_start);
        pages_after_replacement = file->page_count();

        // Registros do mesmo tamanho devem caber exatamente nas localizações liberadas.
        if constexpr (replacements_match_original_size) {
            suite.check(freed_locations.empty(), "all freed locations are reused exactly once");
            suite.check(file->page_count() == pages_after_initial_insert,
                        "same-size replacements allocate no new physical page");
        }
        auto restored_layout = heap->layout();
        suite.check(restored_layout.has_value(), "replacement heap layout is available");
        if (restored_layout) {
            // Substitutos de mesmo tamanho restauram exatamente o layout original.
            if constexpr (replacements_match_original_size) {
                suite.check(restored_layout->size() == original_layout->size(),
                            "same-size replacements preserve the number of heap pages");
                if (restored_layout->size() == original_layout->size()) {
                    for (std::size_t index = 0; index < restored_layout->size(); ++index) {
                        const auto& before = (*original_layout)[index];
                        const auto& after = (*restored_layout)[index];
                        suite.check(after.id == before.id && after.previous == before.previous &&
                                        after.next == before.next &&
                                        after.record_count == before.record_count,
                                    "replacement records preserve page identity and record count");
                    }
                }
            }
            free_space_after_replacement = total_free_space(*restored_layout);

            // Soma as capacidades persistidas para provar que não existe folga por registro.
            std::size_t observed_capacity = 0;
            std::size_t observed_records = 0;
            for (const auto& page_info : *restored_layout) {
                auto raw_page = file->read(page_info.id);
                suite.check(raw_page.has_value(), "replacement data page is read");
                if (!raw_page) {
                    return suite.finish();
                }
                auto page = SlottedPage::from_page(std::move(*raw_page));
                suite.check(page.has_value(), "replacement data page is valid");
                if (!page) {
                    return suite.finish();
                }
                for (const auto& slot : page->slots()) {
                    if (slot.occupied()) {
                        observed_capacity += slot.record_capacity;
                        ++observed_records;
                    }
                }
            }
            const auto expected_capacity =
                replacement_record_count * initial_record_size +
                replacement_record_count * replacement_record_size;
            suite.check(observed_records == initial_record_count &&
                            observed_capacity == expected_capacity,
                        "persisted capacity equals the total logical record size");
        }
        auto final_records = heap->scan();
        suite.check(final_records.has_value() && final_records->size() == initial_record_count,
                    "heap contains ten thousand records again");
    }

    // Reabre o banco para comprovar a persistência do layout reutilizado.
    {
        auto file = PageFile::open(database.path());
        suite.check(file.has_value(), "space-reuse database is reopened");
        if (file) {
            auto heap = TableHeap::open(*file, root);
            suite.check(heap.has_value(), "space-reuse heap is reopened");
            if (heap) {
                auto records = heap->scan();
                suite.check(records.has_value() && records->size() == initial_record_count,
                            "reopened heap still contains ten thousand records");
            }
            suite.check(file->page_count() == pages_after_replacement,
                        "reopened database preserves its physical page count");
        }
    }

    // Imprime tempos e provas quantitativas do cenário.
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Initial records: " << initial_record_count << '\n';
    std::cout << "Initial logical record size: " << initial_record_size << " bytes\n";
    std::cout << "Initial reserved capacity: " << capacity_for_test(initial_record_size)
              << " bytes\n";
    std::cout << "Deleted even IDs: " << replacement_record_count << '\n';
    std::cout << "Replacement records: " << replacement_record_count << '\n';
    std::cout << "Replacement logical record size: " << replacement_record_size
              << " bytes\n";
    std::cout << "Replacement reserved capacity: "
              << capacity_for_test(replacement_record_size) << " bytes\n";
    std::cout << "Physical pages: " << pages_after_initial_insert << '\n';
    std::cout << "Initial insert time: " << initial_insert_seconds << " s\n";
    std::cout << "Even-ID delete time: " << even_delete_seconds << " s\n";
    std::cout << "Replacement insert time: " << replacement_insert_seconds << " s\n";
    std::cout << "Free space before replacements: " << free_space_before_replacement
              << " bytes\n";
    std::cout << "Free space after replacements: " << free_space_after_replacement
              << " bytes\n";
    const auto net_free_space_change = static_cast<std::int64_t>(free_space_after_replacement) -
                                       static_cast<std::int64_t>(free_space_before_replacement);
    std::cout << "Net free-space change: " << net_free_space_change << " bytes\n";
    std::cout << "New physical pages during reuse: "
              << pages_after_replacement - pages_after_initial_insert << '\n';

    // Converte as verificações em código de saída do processo.
    return suite.finish();
}
