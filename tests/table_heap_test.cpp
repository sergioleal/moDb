// Importa Row e Value persistidos pelo teste.
#include "modb/row.hpp"
#include "modb/value.hpp"
// Importa os codecs usados ao redor do heap.
#include "modb/storage/codec.hpp"
// Importa a coleção multipágina testada aqui.
#include "modb/storage/table_heap.hpp"

// Importa as verificações simples dos testes.
#include "test_support.hpp"

// Disponibiliza um número variável para o nome temporário.
#include <chrono>
// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza std::uint16_t usado ao forjar uma geração antiga.
#include <cstdint>
// Disponibiliza caminhos e remoção de arquivos.
#include <filesystem>
// Disponibiliza a corrupção controlada dos bytes da raiz no teste de reparo.
#include <fstream>
// Disponibiliza a impressão do caminho usado.
#include <iostream>
// Disponibiliza textos grandes para preencher várias páginas.
#include <string>
// Disponibiliza std::error_code para limpeza sem exceções.
#include <system_error>
// Disponibiliza std::move.
#include <utility>
// Disponibiliza as coleções esperadas pelo teste.
#include <vector>

namespace {

// Remove automaticamente o banco criado pelo teste.
class TemporaryDatabase {
public:
    // Gera um caminho diferente para cada execução.
    TemporaryDatabase() {
        // Usa um relógio monotônico para produzir um componente variável.
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        // Monta o caminho dentro da pasta temporária do sistema.
        path_ = std::filesystem::temp_directory_path() /
                ("modb-table-heap-" + std::to_string(unique) + ".modb");
    }

    // Remove o arquivo quando o teste termina.
    ~TemporaryDatabase() {
        // Recebe uma possível falha sem lançar no destrutor.
        std::error_code ignored;
        // Tenta apagar o banco temporário.
        std::filesystem::remove(path_, ignored);
    }

    // Retorna o caminho sem copiá-lo.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    // Guarda o caminho completo do banco.
    std::filesystem::path path_;
};

} // namespace

// Executa os cenários do TableHeap.
int main() {
    // Evita repetir o namespace do modelo.
    using namespace modb;
    // Evita repetir o namespace do armazenamento.
    using namespace modb::storage;

    // Acumula e mostra falhas.
    TestSuite suite;
    // Prepara o arquivo temporário.
    TemporaryDatabase database;
    std::cout << "Temporary TableHeap database: " << database.path() << '\n';

    // Guarda a raiz necessária depois da reabertura.
    PageId root{};
    // Guarda cada Row para comparar com a leitura futura.
    std::vector<Row> expected_rows;
    // Guarda os endereços retornados por insert.
    std::vector<RecordId> expected_ids;

    // Cria o heap e força o crescimento para várias páginas.
    {
        // Cria o arquivo com seu superbloco.
        auto file = PageFile::create(database.path());
        suite.check(file.has_value(), "TableHeap database is created");
        if (!file) {
            return suite.finish();
        }
        // Uma configuração sem buffers falha antes de alocar a página raiz.
        suite.check_error(TableHeap::create(*file, 0), ErrorCode::invalid_argument,
                          "TableHeap rejects an empty scratch page pool");
        suite.check(file->page_count() == 1,
                    "invalid scratch capacity does not allocate a root page");
        // Aloca a raiz dedicada sem criar páginas de dados.
        auto heap = TableHeap::create(*file);
        suite.check(heap.has_value(), "TableHeap root is created");
        if (!heap) {
            return suite.finish();
        }
        // Preserva a raiz para as próximas aberturas.
        root = heap->root_page();
        suite.check(root == PageId{1}, "first TableHeap root uses PageId one");
        auto empty_layout = heap->layout();
        suite.check(empty_layout.has_value() && empty_layout->empty() &&
                        !heap->first_page() && !heap->last_page() &&
                        heap->page_count() == 0 && heap->record_count() == 0,
                    "new TableHeap root describes an empty data chain");
        suite.check(file->page_count() == 2,
                    "new empty heap allocates only its dedicated root");

        // Um registro maior que a capacidade precisa falhar sem alocar páginas.
        const std::vector<std::byte> oversized(slotted_page_max_record_size + 1,
                                               std::byte{0x55U});
        suite.check_error(heap->insert(oversized), ErrorCode::record_too_large,
                          "oversized record is rejected before traversal");

        // Escala a carga para continuar atravessando ao menos três páginas.
        constexpr auto record_count =
            static_cast<std::int64_t>((40 * page_size + 4095) / 4096);
        for (std::int64_t index = 0; index < record_count; ++index) {
            // Produz conteúdo identificável com tamanho suficiente para encher páginas.
            auto text = "record-" + std::to_string(index) + "-" + std::string(180, 'x');
            // Mantém uma cópia lógica para comparação.
            Row row{Value{index}, Value{std::move(text)}};
            // Converte a Row para bytes.
            auto encoded = encode_row(row);
            suite.check(encoded.has_value(), "TableHeap test Row is encoded");
            if (!encoded) {
                return suite.finish();
            }
            // Deixa o heap escolher ou criar a página de destino.
            auto id = heap->insert(*encoded);
            suite.check(id.has_value(), "TableHeap inserts encoded Row");
            if (!id) {
                return suite.finish();
            }
            // Guarda resultado e expectativa na mesma ordem.
            expected_rows.push_back(std::move(row));
            expected_ids.push_back(*id);
        }

        // Resume a cadeia criada pelas inserções.
        auto pages = heap->layout();
        suite.check(pages.has_value() && pages->size() >= 3,
                    "TableHeap grows to at least three pages");
        suite.check(heap->page_count() == pages->size() &&
                        heap->record_count() == expected_ids.size(),
                    "dedicated root tracks page and record counts");
        if (pages) {
            // A primeira página de dados precisa usar o sentinela de início.
            suite.check(!pages->front().previous.has_value(),
                        "first heap data page has no previous page");
            // A última página precisa usar o sentinela de fim.
            suite.check(!pages->back().next.has_value(), "last heap page has no next page");
            // Cada par precisa apontar corretamente nas duas direções.
            for (std::size_t index = 1; index < pages->size(); ++index) {
                suite.check((*pages)[index - 1].next == std::optional<PageId>{(*pages)[index].id},
                            "heap pages form a forward chain");
                suite.check((*pages)[index].previous ==
                                std::optional<PageId>{(*pages)[index - 1].id},
                            "heap pages form a backward chain");
            }
        }

        // O scan precisa preservar a ordem das inserções.
        auto scanned = heap->scan();
        suite.check(scanned.has_value() && *scanned == expected_ids,
                    "scan returns every RecordId in insertion order");
        // Garante que os bytes chegaram ao sistema operacional antes do fechamento.
        suite.check(file->flush().has_value(), "TableHeap changes are flushed");
    }

    // Reabre o arquivo e confere todos os registros.
    {
        // Abre e valida o PageFile existente.
        auto file = PageFile::open(database.path());
        suite.check(file.has_value(), "TableHeap database is reopened");
        if (!file) {
            return suite.finish();
        }
        // Reconstrói e valida toda a cadeia a partir da raiz.
        auto heap = TableHeap::open(*file, root);
        suite.check(heap.has_value(), "TableHeap chain is reopened");
        if (!heap) {
            return suite.finish();
        }
        // Recria a sequência de endereços.
        auto scanned = heap->scan();
        suite.check(scanned.has_value() && scanned->size() == expected_rows.size(),
                    "reopened scan returns every record");
        if (scanned) {
            // Lê e compara cada Row persistida.
            for (std::size_t index = 0; index < scanned->size(); ++index) {
                auto record = heap->read((*scanned)[index]);
                suite.check(record.has_value(), "scanned RecordId can be read");
                if (!record) {
                    return suite.finish();
                }
                auto decoded = decode_row(*record);
                suite.check(decoded.has_value() && *decoded == expected_rows[index],
                            "Row survives TableHeap close and reopen");
            }

            // scan_records devolve os mesmos endereços e o conteúdo em uma passada.
            auto with_bytes = heap->scan_records();
            suite.check(with_bytes.has_value() && with_bytes->size() == scanned->size(),
                        "scan_records returns every record");
            if (with_bytes) {
                bool same_addresses = true;
                bool rows_match = true;
                for (std::size_t index = 0; index < with_bytes->size(); ++index) {
                    if ((*with_bytes)[index].id != (*scanned)[index]) {
                        same_addresses = false;
                    }
                    auto decoded = decode_row((*with_bytes)[index].bytes);
                    if (!decoded || *decoded != expected_rows[index]) {
                        rows_match = false;
                    }
                }
                suite.check(same_addresses, "scan_records matches scan address order");
                suite.check(rows_match, "scan_records returns the persisted Row bytes");
            }
        }

        // Um PageId que não pertence à cadeia precisa ser rejeitado.
        suite.check_error(heap->read(RecordId{PageId{999}, SlotId{0}}),
                          ErrorCode::record_not_found,
                          "RecordId outside the heap is rejected");

        // Atualiza o primeiro registro sem abandonar sua capacidade atual.
        const Row updated_row{Value{std::int64_t{1000}}, Value{"updated"}};
        auto updated_bytes = encode_row(updated_row);
        suite.check(updated_bytes.has_value(), "updated Row is encoded");
        if (updated_bytes) {
            auto updated_id = heap->update(expected_ids[0], *updated_bytes);
            suite.check(updated_id == Result<RecordId>{expected_ids[0]},
                        "small update preserves the RecordId");
            auto stored = heap->read(expected_ids[0]);
            if (stored) {
                auto decoded = decode_row(*stored);
                suite.check(decoded.has_value() && *decoded == updated_row,
                            "updated Row is immediately readable");
            }

            // update rejeita um RecordId com geração antiga sem alterar bytes.
            auto stale_id = expected_ids[0];
            stale_id.generation = static_cast<std::uint16_t>(stale_id.generation + 1);
            suite.check_error(heap->update(stale_id, *updated_bytes),
                              ErrorCode::record_not_found,
                              "update rejects a stale generation");
            // update rejeita uma página que não pertence ao heap.
            suite.check_error(
                heap->update(RecordId{PageId{999}, SlotId{0}, std::uint16_t{1}}, *updated_bytes),
                ErrorCode::record_not_found, "update rejects a page outside the heap");
            // O registro original permanece intacto após as rejeições.
            auto still_there = heap->read(expected_ids[0]);
            suite.check(still_there.has_value(), "rejected updates leave the record intact");
        }

        // Remove uma ocupação e confirma que seu RecordId antigo fica inválido.
        const auto removed_id = expected_ids[1];
        suite.check(heap->erase(removed_id).has_value(), "TableHeap erases a record");
        suite.check_error(heap->read(removed_id), ErrorCode::record_not_found,
                          "erased RecordId cannot be read");

        // Uma inserção pequena deve reutilizar o mesmo SlotId com outra geração.
        const Row replacement_row{Value{std::int64_t{2000}}, Value{"replacement"}};
        auto replacement_bytes = encode_row(replacement_row);
        if (replacement_bytes) {
            auto replacement_id = heap->insert(*replacement_bytes);
            suite.check(replacement_id.has_value() &&
                            replacement_id->page == removed_id.page &&
                            replacement_id->slot == removed_id.slot &&
                            replacement_id->generation != removed_id.generation,
                        "TableHeap reuses a slot with a new generation");
        }

        // Esvazia uma página não raiz para confirmar sua retirada da cadeia lógica.
        auto layout_before = heap->layout();
        auto ids_before = heap->scan();
        if (layout_before && ids_before && layout_before->size() > 1) {
            // Prefere a penúltima para exercitar o reparo das duas ligações vizinhas.
            const auto removed_page = layout_before->size() > 2
                                          ? (*layout_before)[layout_before->size() - 2].id
                                          : layout_before->back().id;
            for (const auto& record_id : *ids_before) {
                if (record_id.page == removed_page) {
                    suite.check(heap->erase(record_id).has_value(),
                                "record from a non-root page is erased");
                }
            }
            auto layout_after = heap->layout();
            suite.check(layout_after.has_value() &&
                            layout_after->size() + 1 == layout_before->size(),
                        "empty non-root page is removed from the heap chain");
            if (layout_after) {
                for (std::size_t index = 1; index < layout_after->size(); ++index) {
                    suite.check((*layout_after)[index - 1].next ==
                                        std::optional<PageId>{(*layout_after)[index].id} &&
                                    (*layout_after)[index].previous ==
                                        std::optional<PageId>{(*layout_after)[index - 1].id},
                                "neighbours are relinked in both directions");
                }
            }
        }
        suite.check(file->flush().has_value(), "reuse changes survive a later reopen");
    }

    // Corrompe apenas a ligação final para testar detecção de ciclo.
    {
        // Abre novamente o arquivo para alteração controlada do teste.
        auto file = PageFile::open(database.path());
        suite.check(file.has_value(), "cycle test database is opened");
        if (file) {
            auto heap = TableHeap::open(*file, root);
            suite.check(heap.has_value(), "heap is valid before cycle injection");
            if (heap) {
                auto pages = heap->layout();
                suite.check(pages.has_value(), "cycle test layout is available");
                if (pages) {
                    const auto last = pages->back().id;
                    auto raw = file->read(last);
                    if (raw) {
                        auto page = SlottedPage::from_page(std::move(*raw));
                        if (page) {
                            suite.check(page->set_next_page(pages->front().id).has_value(),
                                        "cycle is injected into the last page");
                            suite.check(file->write(last, page->page()).has_value(),
                                        "cyclic link is persisted");
                            suite.check(file->flush().has_value(), "cyclic link is flushed");
                        }
                    }
                    suite.check_error(TableHeap::open(*file, root), ErrorCode::page_chain_cycle,
                                      "TableHeap rejects a cyclic page chain");
                }
            }
        }
    }

    // Reparo: contadores da raiz divergentes da cadeia deixam de inutilizar o heap.
    {
        TemporaryDatabase repair_db;
        std::cout << "Temporary repair database: " << repair_db.path() << '\n';
        PageId repair_root{};
        std::size_t inserted = 0;
        {
            auto file = PageFile::create(repair_db.path());
            suite.check(file.has_value(), "repair test database is created");
            if (file) {
                auto heap = TableHeap::create(*file);
                suite.check(heap.has_value(), "repair test heap is created");
                if (heap) {
                    repair_root = heap->root_page();
                    for (int value = 0; value < 5; ++value) {
                        const Row row{Value{static_cast<std::int64_t>(value)}, Value{"r"}};
                        auto bytes = encode_row(row);
                        if (bytes && heap->insert(*bytes)) {
                            ++inserted;
                        }
                    }
                    suite.check(inserted == 5, "repair test heap gets five records");
                    suite.check(heap->record_count() == 5,
                                "heap counts five records before corruption");
                    suite.check(file->flush().has_value(), "repair test heap is flushed");
                }
            }
        }
        // Corrompe só o byte menos significativo de record_count na raiz THRP.
        // No formato THRP o record_count é um u64 little-endian no offset 32.
        {
            std::fstream stream{repair_db.path(), std::ios::binary | std::ios::in | std::ios::out};
            const auto record_count_offset =
                static_cast<std::streamoff>(repair_root.value * page_size + 32);
            stream.seekp(record_count_offset);
            stream.put(static_cast<char>(0xffU));
        }
        // Com os contadores divergentes, a abertura recusa o heap inteiro.
        {
            auto file = PageFile::open(repair_db.path());
            suite.check(file.has_value(), "corrupted repair database still opens as a file");
            if (file) {
                suite.check_error(TableHeap::open(*file, repair_root), ErrorCode::corrupt_page,
                                  "divergent root counters make the heap unopenable");
            }
        }
        // O reparo reconstrói os contadores a partir da cadeia autodescritiva.
        {
            auto file = PageFile::open(repair_db.path());
            suite.check(file.has_value(), "repair reopens the database");
            if (file) {
                auto report = repair_table_heap(*file, repair_root);
                suite.check(report.has_value(), "repair succeeds");
                if (report) {
                    suite.check(report->page_count == 1 && report->record_count == 5 &&
                                    report->root_rewritten,
                                "repair rebuilds one page and five records");
                }
                suite.check(file->flush().has_value(), "repaired root is flushed");
            }
        }
        // Depois do reparo o heap abre normalmente e devolve todos os registros.
        {
            auto file = PageFile::open(repair_db.path());
            suite.check(file.has_value(), "database reopens after repair");
            if (file) {
                auto heap = TableHeap::open(*file, repair_root);
                suite.check(heap.has_value(), "repaired heap opens");
                if (heap) {
                    auto ids = heap->scan();
                    suite.check(ids.has_value() && ids->size() == 5,
                                "repaired heap returns every record");
                }
            }
        }
        // Reparar um heap saudável é idempotente e não reescreve a raiz.
        {
            auto file = PageFile::open(repair_db.path());
            if (file) {
                auto report = repair_table_heap(*file, repair_root);
                suite.check(report.has_value() && !report->root_rewritten,
                            "repairing a healthy heap rewrites nothing");
            }
        }
    }

    // Encerra o processo com o resultado acumulado.
    return suite.finish();
}
