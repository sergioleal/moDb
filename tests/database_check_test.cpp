// Importa o verificador de integridade exercitado pelos cenários.
#include "modb/storage/database_check.hpp"
// Importa Page e page_size usados na corrupção física.
#include "modb/storage/page.hpp"
// Importa PageFile para criar e corromper bancos de teste.
#include "modb/storage/page_file.hpp"
// Importa SlottedPage para formatar uma página SLPG.
#include "modb/storage/slotted_page.hpp"
// Importa TableHeap para criar cadeias THRP.
#include "modb/storage/table_heap.hpp"
// Importa a codificação de linha para montar um registro válido e depois corrompê-lo.
#include "modb/storage/codec.hpp"
// Importa o BlobStore para exercitar a classificação de páginas BLBP (Fase 4).
#include "modb/object/blob_store.hpp"
#include "modb/object/object_store.hpp"
// Importa Row e Value usados no registro do teste L4.
#include "modb/row.hpp"
#include "modb/value.hpp"

// Importa as verificações simples compartilhadas.
#include "test_support.hpp"

// Disponibiliza um componente variável no nome do arquivo.
#include <chrono>
// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza caminhos e remoção do banco temporário.
#include <filesystem>
// Disponibiliza leitura e escrita brutas de corrupção.
#include <fstream>
// Disponibiliza a saída do caminho usado.
#include <iostream>
// Disponibiliza textos usados como payloads de teste.
#include <string>
// Disponibiliza visões de texto para montar payloads.
#include <string_view>
// Disponibiliza std::error_code para a limpeza.
#include <system_error>
// Disponibiliza o buffer do payload.
#include <vector>

namespace {

// Remove automaticamente o banco criado pelo teste.
class TemporaryDatabase {
public:
    TemporaryDatabase() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-database-check-" + std::to_string(unique) + ".modb");
    }

    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Sobrescreve um byte físico para forçar um cenário de corrupção.
void overwrite_byte(const std::filesystem::path& path, std::streamoff offset, char value) {
    std::fstream stream{path, std::ios::binary | std::ios::in | std::ios::out};
    stream.seekp(offset);
    stream.put(value);
}

// Conta páginas classificadas com o kind solicitado.
std::size_t count_kind(const modb::storage::DatabaseCheckReport& report,
                       modb::storage::PageKind kind) {
    std::size_t total = 0;
    for (const auto& page : report.pages) {
        if (page.kind == kind) {
            ++total;
        }
    }
    return total;
}

} // namespace

// Exercita L1–L3 do verificador com caminhos felizes e corrompidos.
int main() {
    using namespace modb;
    using namespace modb::storage;

    TestSuite suite;

    // Arquivo novo: somente superbloco, L1–L3 ok.
    {
        TemporaryDatabase database;
        {
            auto file = PageFile::create(database.path());
            suite.check(file.has_value(), "empty database is created");
            if (!file) {
                return suite.finish();
            }
        }

        auto checked = check_database(database.path());
        suite.check(checked.has_value(), "empty database check succeeds");
        if (checked) {
            suite.check(checked->ok(), "empty database report has no errors");
            suite.check(checked->page_count == 1, "empty database has only the superblock");
            suite.check(checked->pages.empty(), "empty database has no data pages");
        }
    }

    // DBRT v2 é identificado sem abrir Database, portanto sem recovery nem
    // migração; a época inicial fica visível no relatório somente leitura.
    {
        TemporaryDatabase database;
        {
            auto file = PageFile::create(database.path());
            suite.check(file.has_value(), "ODB++ database file is created");
            if (!file) {
                return suite.finish();
            }
            auto store = modb::object::ObjectStore::create(*file);
            suite.check(store.has_value(), "ODB++ root is created");
            suite.check(file->flush().has_value(), "ODB++ root is flushed");
        }
        auto checked = check_database(database.path());
        suite.check(checked.has_value() && checked->object_format.has_value(),
                    "check reports the object format");
        if (checked && checked->object_format) {
            const auto& format = *checked->object_format;
            suite.check(format.dbrt_version == 2 && format.epoch == 0,
                        "check reports DBRT v2 and epoch zero");
            suite.check(!format.migration_required(),
                        "a newly created ODB++ database needs no migration");
        }
    }

    // Página zerada permanece unformatted e válida.
    {
        TemporaryDatabase database;
        {
            auto file = PageFile::create(database.path());
            suite.check(file.has_value(), "unformatted database is created");
            if (!file) {
                return suite.finish();
            }
            suite.check(file->allocate_page().has_value(), "unformatted page is allocated");
            suite.check(file->flush().has_value(), "unformatted database is flushed");
        }

        auto checked = check_database(database.path());
        suite.check(checked.has_value(), "unformatted database check succeeds");
        if (checked) {
            suite.check(checked->ok(), "unformatted database report has no errors");
            suite.check(count_kind(*checked, PageKind::unformatted) == 1,
                        "allocated zero page is classified as unformatted");
        }
    }

    // Slotted page + TableHeap preenchido passam em L2 e L3.
    {
        TemporaryDatabase database;
        {
            auto file = PageFile::create(database.path());
            suite.check(file.has_value(), "healthy database is created");
            if (!file) {
                return suite.finish();
            }

            auto slotted_id = file->allocate_page();
            suite.check(slotted_id.has_value(), "slotted page is allocated");
            if (!slotted_id) {
                return suite.finish();
            }
            // Insere um registro qualquer só para a página ter conteúdo válido.
            auto encoded_payload = encode_row(Row{Value{std::int64_t{1}}, Value{"ok"}});
            suite.check(encoded_payload.has_value(), "healthy sample row is encoded");
            if (!encoded_payload) {
                return suite.finish();
            }
            const auto& payload = *encoded_payload;
            auto page = SlottedPage::create();
            suite.check(page.insert(payload).has_value(),
                        "sample record is inserted into slotted page");
            suite.check(file->write(*slotted_id, page.page()).has_value(),
                        "slotted page is written");

            auto heap = TableHeap::create(*file);
            suite.check(heap.has_value(), "TableHeap root is created");
            if (!heap) {
                return suite.finish();
            }
            suite.check(heap->insert(payload).has_value(),
                        "record is inserted into TableHeap");
            suite.check(file->flush().has_value(), "healthy database is flushed");
        }

        auto checked = check_database(database.path());
        suite.check(checked.has_value(), "healthy database check succeeds");
        if (checked) {
            suite.check(checked->ok(), "healthy database report has no errors");
            suite.check(count_kind(*checked, PageKind::slotted) >= 1,
                        "healthy database contains slotted pages");
            suite.check(count_kind(*checked, PageKind::table_heap_root) == 1,
                        "healthy database contains one TableHeap root");
        }
    }

    // Magic inválido em página de dados falha em L2.
    {
        TemporaryDatabase database;
        {
            auto file = PageFile::create(database.path());
            suite.check(file.has_value(), "corrupt-magic database is created");
            if (!file) {
                return suite.finish();
            }
            auto page_id = file->allocate_page();
            suite.check(page_id.has_value(), "page for magic corruption is allocated");
            auto page = SlottedPage::create();
            suite.check(file->write(*page_id, page.page()).has_value(),
                        "slotted page for magic corruption is written");
            suite.check(file->flush().has_value(), "corrupt-magic database is flushed");
        }
        overwrite_byte(database.path(), static_cast<std::streamoff>(page_size), 'X');
        auto checked = check_database(database.path());
        suite.check(checked.has_value(), "corrupt-magic check still opens the file");
        if (checked) {
            suite.check(!checked->ok(), "corrupt-magic report is not ok");
            bool found = false;
            for (const auto& page : checked->pages) {
                if (page.error && page.error->code == ErrorCode::invalid_page_format) {
                    found = true;
                }
            }
            suite.check(found, "corrupt magic yields invalid_page_format");
        }
    }

    // Contadores inconsistentes na raiz THRP falham em L2/L3.
    {
        TemporaryDatabase database;
        PageId root{};
        {
            auto file = PageFile::create(database.path());
            suite.check(file.has_value(), "corrupt-root database is created");
            if (!file) {
                return suite.finish();
            }
            auto heap = TableHeap::create(*file);
            suite.check(heap.has_value(), "corrupt-root TableHeap is created");
            if (!heap) {
                return suite.finish();
            }
            root = heap->root_page();
            suite.check(file->flush().has_value(), "corrupt-root database is flushed");
        }
        // page_count da raiz THRP começa no offset 24.
        const auto page_count_offset =
            static_cast<std::streamoff>(root.value * page_size + 24);
        overwrite_byte(database.path(), page_count_offset, 1);
        auto checked = check_database(database.path());
        suite.check(checked.has_value(), "corrupt-root check still opens the file");
        if (checked) {
            suite.check(!checked->ok(), "corrupt-root report is not ok");
            bool found = false;
            for (const auto& page : checked->pages) {
                if (page.id == root && page.error) {
                    found = true;
                }
            }
            if (!found) {
                found = !checked->heap_errors.empty();
            }
            suite.check(found, "inconsistent TableHeap root is reported");
        }
    }

    // L4: corromper apenas o CONTEÚDO de um registro (sem quebrar sua estrutura)
    // não é acusado pelo verificador de armazenamento. Decodificar o payload é
    // responsabilidade da camada que o possui (linha ou objeto), não do check
    // estrutural — que, de propósito, não conhece o formato do conteúdo.
    {
        TemporaryDatabase database;
        PageId slotted_id{};
        std::size_t payload_size = 0;
        {
            auto file = PageFile::create(database.path());
            suite.check(file.has_value(), "L4 database is created");
            if (!file) {
                return suite.finish();
            }
            auto id = file->allocate_page();
            suite.check(id.has_value(), "L4 slotted page is allocated");
            if (!id) {
                return suite.finish();
            }
            slotted_id = *id;
            // Insere um registro qualquer; seu conteúdo será corrompido depois.
            auto encoded = encode_row(Row{Value{std::int64_t{7}}, Value{"x"}});
            suite.check(encoded.has_value(), "L4 sample record is encoded");
            if (!encoded) {
                return suite.finish();
            }
            payload_size = encoded->size();
            auto page = SlottedPage::create();
            suite.check(page.insert(*encoded).has_value(), "L4 record is inserted");
            suite.check(file->write(slotted_id, page.page()).has_value(), "L4 page is written");
            suite.check(file->flush().has_value(), "L4 database is flushed");
        }
        // Corrompe um byte de conteúdo, deixando slot/offset/tamanho intactos.
        const auto content_offset = static_cast<std::streamoff>(
            slotted_id.value * page_size + (page_size - payload_size) + 2);
        overwrite_byte(database.path(), content_offset, static_cast<char>(0xffU));
        auto checked = check_database(database.path());
        suite.check(checked.has_value(), "L4 check still opens the file");
        if (checked) {
            // A estrutura permanece íntegra e o check permanece OK: conteúdo não
            // é da alçada do verificador de armazenamento.
            suite.check(checked->ok(), "content-only corruption keeps a valid structural check");
            suite.check(checked->record_errors.empty(),
                        "the storage check does not decode record content");
        }
    }

    // Páginas de blob (BLBP) são reconhecidas e validadas estruturalmente; um
    // banco com blobs saudáveis passa no check sem falsos positivos.
    {
        TemporaryDatabase database;
        {
            auto file = PageFile::create(database.path());
            suite.check(file.has_value(), "blob database is created");
            if (!file) {
                return suite.finish();
            }
            modb::object::BlobStore blobs{*file};
            // Blob multi-página para exercitar mais de uma página BLBP.
            std::vector<std::byte> data(10 * 1024, std::byte{0xAB});
            suite.check(blobs.create(data).has_value(), "blob is written");
            suite.check(file->flush().has_value(), "blob database is flushed");
        }
        auto checked = check_database(database.path());
        suite.check(checked.has_value(), "blob database check succeeds");
        if (checked) {
            suite.check(checked->ok(), "healthy blob database report has no errors");
            suite.check(count_kind(*checked, PageKind::blob) >= 2,
                        "blob database contains multiple BLBP pages");
        }
    }

    // Uma versão inválida no cabeçalho BLBP é acusada em L2.
    {
        TemporaryDatabase database;
        PageId blob_page{};
        {
            auto file = PageFile::create(database.path());
            suite.check(file.has_value(), "corrupt-blob database is created");
            if (!file) {
                return suite.finish();
            }
            modb::object::BlobStore blobs{*file};
            auto id = blobs.create(std::vector<std::byte>(50, std::byte{0x01}));
            suite.check(id.has_value(), "corrupt-blob base is written");
            if (!id) {
                return suite.finish();
            }
            blob_page = PageId{id->value};
            suite.check(file->flush().has_value(), "corrupt-blob database is flushed");
        }
        // Versão está no offset 4 do cabeçalho: grava um valor não suportado.
        overwrite_byte(database.path(),
                       static_cast<std::streamoff>(blob_page.value * page_size + 4), 9);
        auto checked = check_database(database.path());
        suite.check(checked.has_value(), "corrupt-blob check still opens the file");
        if (checked) {
            suite.check(!checked->ok(), "corrupt-blob report is not ok");
            bool found = false;
            for (const auto& page : checked->pages) {
                if (page.id == blob_page && page.error &&
                    page.error->code == ErrorCode::incompatible_page_version) {
                    found = true;
                }
            }
            suite.check(found, "an unsupported blob version is reported");
        }
    }

    return suite.finish();
}
