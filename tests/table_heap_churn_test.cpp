// Importa Row e Value usados para produzir registros relacionais.
#include "modb/row.hpp"
#include "modb/value.hpp"
// Importa o codec e o TableHeap exercitados pelo teste.
#include "modb/storage/codec.hpp"
#include "modb/storage/table_heap.hpp"

// Importa as verificações simples compartilhadas.
#include "test_support.hpp"

// Disponibiliza o relógio monotônico usado nas medições.
#include <chrono>
// Disponibiliza std::byte e std::size_t.
#include <cstddef>
// Disponibiliza std::int64_t.
#include <cstdint>
// Disponibiliza caminhos e remoção do arquivo temporário.
#include <filesystem>
// Disponibiliza formatação dos tempos e taxas.
#include <iomanip>
// Disponibiliza a saída do relatório.
#include <iostream>
// Disponibiliza textos únicos para as linhas.
#include <string>
// Disponibiliza std::error_code para o destrutor.
#include <system_error>
// Disponibiliza std::move para os bytes codificados.
#include <utility>
// Disponibiliza coleções de bytes e RecordIds.
#include <vector>

namespace {

// Permite aumentar a carga sem alterar o cenário padrão de dez mil registros.
#ifndef MODB_CHURN_RECORD_TOTAL
#define MODB_CHURN_RECORD_TOTAL 10000
#endif

constexpr std::size_t record_total = MODB_CHURN_RECORD_TOTAL;
static_assert(record_total > 0, "MODB_CHURN_RECORD_TOTAL must be positive");

// O segundo alvo CMake ativa a variante que percorre os RecordIds ao contrário.
#if defined(MODB_DELETE_REVERSE)
constexpr bool reverse_removal = true;
#else
constexpr bool reverse_removal = false;
#endif

// Remove automaticamente o banco temporário ao final do processo.
class TemporaryDatabase {
public:
    // Produz um nome único dentro da pasta temporária do Windows.
    TemporaryDatabase() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        // O nome deixa visível qual ordem está sendo medida.
        const auto prefix = reverse_removal ? "modb-table-heap-reverse-churn-"
                                            : "modb-table-heap-churn-";
        path_ = std::filesystem::temp_directory_path() /
                (prefix + std::to_string(unique) + ".modb");
    }

    // Evita deixar o arquivo de carga no computador.
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    // Entrega o caminho completo sem copiá-lo.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    // Guarda o caminho enquanto o teste estiver vivo.
    std::filesystem::path path_;
};

// Converte uma duração para segundos com casas decimais.
double seconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double>{duration}.count();
}

} // namespace

// Insere e remove a quantidade configurada de registros, medindo cada fase.
int main() {
    // Evita repetir os namespaces do modelo e do armazenamento.
    using namespace modb;
    using namespace modb::storage;

    // Acumula todas as verificações funcionais.
    TestSuite suite;
    // Cria um caminho temporário isolado.
    TemporaryDatabase database;
    std::cout << "Temporary churn database: " << database.path() << '\n';

    // Prepara os bytes antes do cronômetro para medir somente o armazenamento.
    std::vector<std::vector<std::byte>> encoded_rows;
    encoded_rows.reserve(record_total);
    for (std::size_t index = 0; index < record_total; ++index) {
        // Cada linha possui um INTEGER e um TEXT identificáveis.
        const Row row{
            Value{static_cast<std::int64_t>(index)},
            Value{"record-" + std::to_string(index)},
        };
        auto encoded = encode_row(row);
        suite.check(encoded.has_value(), "load-test Row is encoded");
        if (!encoded) {
            return suite.finish();
        }
        encoded_rows.push_back(std::move(*encoded));
    }

    // Guarda os endereços necessários para apagar exatamente as ocupações criadas.
    std::vector<RecordId> ids;
    ids.reserve(record_total);
    // Conserva a raiz para a validação após a reabertura.
    PageId root{};
    // Recebe os resultados medidos.
    double insert_seconds = 0.0;
    double delete_seconds = 0.0;
    std::size_t allocated_pages = 0;

    // Cria, preenche e esvazia o heap na mesma abertura.
    {
        auto file = PageFile::create(database.path());
        suite.check(file.has_value(), "churn database is created");
        if (!file) {
            return suite.finish();
        }
        auto heap = TableHeap::create(*file);
        suite.check(heap.has_value(), "churn TableHeap is created");
        if (!heap) {
            return suite.finish();
        }
        root = heap->root_page();

        // Mede as inserções configuradas e sua sincronização final.
        const auto insert_start = std::chrono::steady_clock::now();
        for (const auto& row : encoded_rows) {
            auto id = heap->insert(row);
            suite.check(id.has_value(), "load-test record is inserted");
            if (!id) {
                return suite.finish();
            }
            ids.push_back(*id);
        }
        suite.check(file->flush().has_value(), "inserted records are flushed");
        const auto insert_end = std::chrono::steady_clock::now();
        insert_seconds = seconds(insert_end - insert_start);
        allocated_pages = file->page_count();

        // Confirma a quantidade antes da remoção.
        auto inserted = heap->scan();
        suite.check(inserted.has_value() && inserted->size() == record_total,
                    "scan finds all inserted records");

        // Mede as remoções na ordem escolhida pelo alvo CMake.
        const auto delete_start = std::chrono::steady_clock::now();
        // Centraliza a operação e suas verificações para as duas ordens.
        const auto erase_record = [&](RecordId id) {
            auto removed = heap->erase(id);
            suite.check(removed.has_value(), "load-test record is erased");
            return removed.has_value();
        };
        // A variante inversa começa no último RecordId inserido.
        if constexpr (reverse_removal) {
            for (auto iterator = ids.rbegin(); iterator != ids.rend(); ++iterator) {
                if (!erase_record(*iterator)) {
                    return suite.finish();
                }
            }
        } else {
            // A variante original esvazia as páginas da frente para trás.
            for (const auto id : ids) {
                if (!erase_record(id)) {
                    return suite.finish();
                }
            }
        }
        suite.check(file->flush().has_value(), "record deletions are flushed");
        const auto delete_end = std::chrono::steady_clock::now();
        delete_seconds = seconds(delete_end - delete_start);

        // A raiz dedicada permanece, mas a cadeia de páginas de dados fica vazia.
        auto remaining = heap->scan();
        suite.check(remaining.has_value() && remaining->empty(),
                    "heap is empty after all deletions");
        auto layout = heap->layout();
        suite.check(layout.has_value() && layout->empty(),
                    "empty heap has no data pages after its dedicated root");
    }

    // Reabre o arquivo para provar que o estado vazio foi persistido.
    {
        auto file = PageFile::open(database.path());
        suite.check(file.has_value(), "churn database is reopened");
        if (file) {
            auto heap = TableHeap::open(*file, root);
            suite.check(heap.has_value(), "empty churn heap is reopened");
            if (heap) {
                auto remaining = heap->scan();
                suite.check(remaining.has_value() && remaining->empty(),
                            "reopened churn heap remains empty");
            }
        }
    }

    // Mostra tempos, taxas e crescimento físico para diagnóstico de desempenho.
    const auto total_seconds = insert_seconds + delete_seconds;
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Records: " << record_total << '\n';
    std::cout << "Delete order: " << (reverse_removal ? "reverse" : "forward") << '\n';
    std::cout << "Physical pages after insert: " << allocated_pages << '\n';
    std::cout << "Insert time: " << insert_seconds << " s ("
              << static_cast<double>(record_total) / insert_seconds << " records/s)\n";
    std::cout << "Delete time: " << delete_seconds << " s ("
              << static_cast<double>(record_total) / delete_seconds << " records/s)\n";
    std::cout << "Total measured time: " << total_seconds << " s\n";

    // Converte as verificações acumuladas em código de saída.
    return suite.finish();
}
