#include "modb/object/attribute_value.hpp"
#include "modb/object/object_store.hpp"
#include "modb/object/type_definition.hpp"
#include "modb/object/type_registry.hpp"
#include "modb/row.hpp"
#include "modb/text_escape.hpp"
#include "modb/value.hpp"
#include "modb/storage/codec.hpp"
#include "modb/storage/page.hpp"
#include "modb/storage/page_file.hpp"
#include "modb/storage/slotted_page.hpp"
#include "modb/storage/table_heap.hpp"
#include "modb/storage/database_check.hpp"
#include "modb/version.hpp"

#include <charconv>
// Disponibiliza std::isfinite para validar valores REAL.
#include <cmath>
// Disponibiliza std::byte, std::size_t e std::to_integer.
#include <cstddef>
// Disponibiliza inteiros com largura definida.
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
// Disponibiliza std::reference_wrapper usado pelo visitor.
#include <functional>
#include <limits>
// Disponibiliza o armazenamento de mensagens e nomes.
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
// Disponibiliza std::variant e std::visit.
#include <variant>
#include <vector>

namespace {

template <typename... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};

// Permite deduzir automaticamente os tipos das lambdas de Overloaded.
template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

// Forward declarations keep the implementation order aligned with the public CLI.
void print_help();
void print_demo_help();
void print_db_help();
void print_page_help();
void print_record_help();
void print_heap_help();
void print_codec_help();
void print_types_help();
void print_type_help();
void print_object_help();

int command_demo();
int command_demo_run(bool force);
int run_demo_step(std::vector<std::string> arguments);
int command_db_create(const std::filesystem::path& path);
int command_db_info(const std::filesystem::path& path);
int command_db_check(const std::filesystem::path& path);
int command_db_repair(const std::filesystem::path& path);
int command_db_delete(const std::filesystem::path& path);
int command_page_create(const std::filesystem::path& path);
int command_page_info(const std::filesystem::path& path, modb::storage::PageId id);
int command_record_page_create(const std::filesystem::path& path);
int command_record_insert_values(const std::filesystem::path& path, modb::storage::PageId page_id,
                       const modb::Row& row);
int command_record_insert(const std::filesystem::path& path, modb::storage::PageId page_id,
                          std::int64_t id, std::string_view name);
int command_record_read(const std::filesystem::path& path, modb::storage::PageId page_id,
                        modb::storage::SlotId slot_id);
int command_record_list(const std::filesystem::path& path, modb::storage::PageId page_id);
int command_record_page_layout(const std::filesystem::path& path,
                               modb::storage::PageId page_id);
int command_heap_create(const std::filesystem::path& path);
int command_heap_insert_values(const std::filesystem::path& path,
                        modb::storage::PageId root_page,
                        const modb::Row& row);
int command_heap_scan(const std::filesystem::path& path, modb::storage::PageId root_page);
int command_heap_layout(const std::filesystem::path& path,
                        modb::storage::PageId root_page);
int command_heap_update_values(const std::filesystem::path& path,
                        modb::storage::PageId root_page,
                        modb::storage::RecordId record_id,
                        const modb::Row& row);
int command_heap_delete(const std::filesystem::path& path,
                        modb::storage::PageId root_page,
                        modb::storage::RecordId record_id);
int command_heap_repair(const std::filesystem::path& path, modb::storage::PageId root_page);
int command_codec_run();
int command_types_run();
int run_type_command(int argc, char* argv[]);
int run_object_command(int argc, char* argv[]);
int run_db_command(int argc, char* argv[]);
int run_page_command(int argc, char* argv[]);
int run_record_command(int argc, char* argv[]);
int run_heap_command(int argc, char* argv[]);
int run(int argc, char* argv[]);

int print_error(const modb::Error& error);
int print_usage_error(std::string_view usage);
bool is_help_argument(std::string_view argument);
void print_command_help(std::string_view usage, std::string_view description);
void print_database_info(const modb::storage::PageFile& file);
modb::Result<modb::storage::PageId> parse_page_id(std::string_view text);
modb::Result<modb::storage::SlotId> parse_slot_id(std::string_view text);
modb::Result<std::uint16_t> parse_generation(std::string_view text);
modb::Result<std::int64_t> parse_integer(std::string_view text);
modb::Result<double> parse_real(std::string_view text);
modb::Result<modb::Value> parse_typed_value(std::string_view argument);
modb::Result<modb::Row> parse_typed_row(int argc, char* argv[], int first_value);
modb::Result<modb::storage::SlottedPage>
load_record_page(const std::filesystem::path& path, modb::storage::PageId page_id);
void print_value(const modb::Value& value);
void print_row(const modb::Row& row);
void print_attribute_value(const modb::object::AttributeValue& value);
void print_field_values(const modb::object::FieldValues& fields);

// Help shown in the same order as the public command groups.
void print_help() {
    std::cout
        << "Usage:\n"
           "  modb <command> [arguments]\n"
           "\n"
           "Commands:\n"
           "  demo     Print a step-by-step tour of the CLI.\n"
           "  db       Manage database files.\n"
           "  page     Manage individual database pages.\n"
           "  record   Manage records stored in one page.\n"
           "  heap     Manage multi-page table heaps.\n"
           "  codec    Encode and decode a row in memory.\n"
           "  types    Exercise the in-memory object model (ODB++).\n"
           "  type     Define and list persistent object types (ODB++).\n"
           "  object   Create, read and remove persistent objects (ODB++).\n"
           "\n"
           "Options:\n"
           "  -h, --help     Show this help.\n"
           "  -v, --version  Show the moDb version.\n"
           "\n"
           "Run 'modb <command> --help' for command-specific usage.\n";
}

void print_demo_help() {
    std::cout << "Usage:\n"
                 "  modb demo\n"
                 "  modb demo run [-force]\n";
}

void print_db_help() {
    std::cout
        << "Usage:\n"
           "  modb db create <file>\n"
           "  modb db info <file>\n"
           "  modb db check <file>\n"
           "  modb db repair <file>\n"
           "  modb db delete <file>\n";
}

void print_page_help() {
    std::cout
        << "Usage:\n"
           "  modb page create <file>\n"
           "  modb page info <file> <page-id>\n";
}

void print_record_help() {
    std::cout
        << "Usage:\n"
           "  modb record page-create <file>\n"
           "  modb record insert <file> <page-id> <id> <name>\n"
           "  modb record insert-values <file> <page-id> <typed-value>...\n"
           "  modb record read <file> <page-id> <slot-id>\n"
           "  modb record list <file> <page-id>\n"
           "  modb record page-layout <file> <page-id>\n";
}

void print_heap_help() {
    std::cout
        << "Usage:\n"
           "  modb heap create <file>\n"
           "  modb heap insert-values <file> <root-page-id> <typed-value>...\n"
           "  modb heap scan <file> <root-page-id>\n"
           "  modb heap layout <file> <root-page-id>\n"
           "  modb heap update-values <file> <root> <page> <slot> <generation> "
           "<typed-value>...\n"
           "  modb heap delete <file> <root> <page> <slot> <generation>\n"
           "  modb heap repair <file> <root-page-id>\n";
}

void print_codec_help() {
    std::cout << "Usage:\n"
                 "  modb codec\n";
}

void print_types_help() {
    std::cout << "Usage:\n"
                 "  modb types\n";
}

void print_type_help() {
    std::cout << "Usage:\n"
                 "  modb type define <file> <name> <attr:type[:null]>...\n"
                 "  modb type list <file>\n"
                 "\n"
                 "Types: boolean, int64, float64, string, bytes\n";
}

void print_object_help() {
    std::cout << "Usage:\n"
                 "  modb object create <file> <type> <attr=value>...\n"
                 "  modb object get <file> <object-id>\n"
                 "  modb object remove <file> <object-id>\n";
}

// Command group: demo.
// Mostra um roteiro reproduzivel para explorar a CLI sem alterar arquivos.
int command_demo() {
    std::cout
        << "moDb guided tour\n"
           "\n"
           "Run the commands below in order. They assume demo.modb does not exist.\n"
           "\n"
           "[1/7] Discover the CLI\n"
           "  modb --version\n"
           "  modb --help\n"
           "\n"
           "[2/7] Create and inspect the database\n"
           "  modb db create demo.modb\n"
           "  modb db info demo.modb\n"
           "\n"
           "[3/7] Create and inspect a raw page (page 1)\n"
           "  modb page create demo.modb\n"
           "  modb page info demo.modb 1\n"
           "\n"
           "[4/7] Work with records in one slotted page (page 2)\n"
           "  modb record page-create demo.modb\n"
           "  modb record insert demo.modb 2 1 \"Ana\"\n"
           "  modb record insert-values demo.modb 2 integer:2 text:Beatriz boolean:true\n"
           "  modb record read demo.modb 2 0\n"
           "  modb record list demo.modb 2\n"
           "  modb record page-layout demo.modb 2\n"
           "\n"
           "[5/7] Work with a multi-page heap (root page 3)\n"
           "  modb heap create demo.modb\n"
           "  modb heap insert-values demo.modb 3 integer:10 text:Ana\n"
           "  modb heap insert-values demo.modb 3 integer:20 text:Beatriz\n"
           "  modb heap scan demo.modb 3\n"
           "  modb heap layout demo.modb 3\n"
           "  modb heap update-values demo.modb 3 4 0 1 integer:10 text:Ana-Maria\n"
           "  modb heap delete demo.modb 3 4 1 1\n"
           "  modb heap scan demo.modb 3\n"
           "\n"
           "[6/7] Try the in-memory tools\n"
           "  modb codec\n"
           "  modb types\n"
           "\n"
           "[7/7] Validate, repair and remove the demonstration database\n"
           "  modb db check demo.modb\n"
           "  modb db repair demo.modb\n"
           "  modb db delete demo.modb\n";
    return 0;
}

// Executa o roteiro pelo despachante real para validar o comportamento publico.
int command_demo_run(bool force) {
    const std::filesystem::path database{"demo.modb"};
    std::error_code filesystem_error;
    const bool database_exists = std::filesystem::exists(database, filesystem_error);
    if (filesystem_error) {
        std::cerr << "Error: cannot inspect demo.modb: " << filesystem_error.message()
                  << '\n';
        return 1;
    }
    if (database_exists && !force) {
        std::cerr << "Error: demo.modb already exists; use -force to replace it.\n";
        return 1;
    }
    if (database_exists) {
        if (!std::filesystem::remove(database, filesystem_error) || filesystem_error) {
            std::cerr << "Error: cannot remove demo.modb: "
                      << filesystem_error.message() << '\n';
            return 1;
        }
    }

    const std::vector<std::vector<std::string>> steps{
        {"modb", "--version"},
        {"modb", "--help"},
        {"modb", "db", "create", "demo.modb"},
        {"modb", "db", "info", "demo.modb"},
        {"modb", "page", "create", "demo.modb"},
        {"modb", "page", "info", "demo.modb", "1"},
        {"modb", "record", "page-create", "demo.modb"},
        {"modb", "record", "insert", "demo.modb", "2", "1", "Ana"},
        {"modb", "record", "insert-values", "demo.modb", "2", "integer:2",
         "text:Beatriz", "boolean:true"},
        {"modb", "record", "read", "demo.modb", "2", "0"},
        {"modb", "record", "list", "demo.modb", "2"},
        {"modb", "record", "page-layout", "demo.modb", "2"},
        {"modb", "heap", "create", "demo.modb"},
        {"modb", "heap", "insert-values", "demo.modb", "3", "integer:10",
         "text:Ana"},
        {"modb", "heap", "insert-values", "demo.modb", "3", "integer:20",
         "text:Beatriz"},
        {"modb", "heap", "scan", "demo.modb", "3"},
        {"modb", "heap", "layout", "demo.modb", "3"},
        {"modb", "heap", "update-values", "demo.modb", "3", "4", "0", "1",
         "integer:10", "text:Ana-Maria"},
        {"modb", "heap", "delete", "demo.modb", "3", "4", "1", "1"},
        {"modb", "heap", "scan", "demo.modb", "3"},
        {"modb", "codec"},
        {"modb", "types"},
        {"modb", "db", "check", "demo.modb"},
        {"modb", "db", "repair", "demo.modb"},
        {"modb", "db", "delete", "demo.modb"},
    };

    for (const auto& step : steps) {
        if (const int result = run_demo_step(step); result != 0) {
            return result;
        }
    }
    return 0;
}

// Command group: db.

// Cria o arquivo pela camada de storage e mostra o cabecalho ja validado.
int command_db_create(const std::filesystem::path& path) {
    // PageFile::create protege arquivos existentes.
    auto created = modb::storage::PageFile::create(path);
    // Mostra a falha produzida pela camada de armazenamento.
    if (!created) {
        return print_error(created.error());
    }
    std::cout << "Database created\n";
    print_database_info(*created);
    // Encerra o comando com sucesso.
    return 0;
}

// Abre o banco apenas o suficiente para expor os metadados persistidos.
int command_db_info(const std::filesystem::path& path) {
    auto opened = modb::storage::PageFile::open(path);
    // Mostra qualquer erro de formato ou de I/O.
    if (!opened) {
        return print_error(opened.error());
    }
    // Mostra os metadados validados.
    print_database_info(*opened);
    // Encerra o comando com sucesso.
    return 0;
}

// Executa o verificador de integridade em camadas e resume o resultado.
int command_db_check(const std::filesystem::path& path) {
    auto checked = modb::storage::check_database(path);
    if (!checked) {
        return print_error(checked.error());
    }

    const auto& report = *checked;
    std::size_t unformatted = 0;
    std::size_t slotted = 0;
    std::size_t table_heap_roots = 0;
    std::size_t object_pages = 0;
    std::size_t unknown = 0;
    for (const auto& page : report.pages) {
        switch (page.kind) {
        case modb::storage::PageKind::unformatted:
            ++unformatted;
            break;
        case modb::storage::PageKind::slotted:
            ++slotted;
            break;
        case modb::storage::PageKind::table_heap_root:
            ++table_heap_roots;
            break;
        case modb::storage::PageKind::database_root:
        case modb::storage::PageKind::identity_directory:
        case modb::storage::PageKind::identity_entries:
            ++object_pages;
            break;
        case modb::storage::PageKind::unknown:
            ++unknown;
            break;
        case modb::storage::PageKind::superblock:
            break;
        }
    }

    std::cout << "Database check: " << path << '\n';
    std::cout << "Page count: " << report.page_count << '\n';
    std::cout << "Unformatted pages: " << unformatted << '\n';
    std::cout << "Slotted pages: " << slotted << '\n';
    std::cout << "TableHeap roots: " << table_heap_roots << '\n';
    if (object_pages != 0) {
        std::cout << "Object store pages (DBRT/IDMD/IDMP): " << object_pages << '\n';
    }
    if (unknown != 0) {
        std::cout << "Unrecognized pages: " << unknown << '\n';
    }

    bool failed = false;
    for (const auto& page : report.pages) {
        if (!page.error) {
            continue;
        }
        failed = true;
        std::cerr << "page " << page.id.value << ": " << page.error->message << '\n';
    }
    for (const auto& error : report.heap_errors) {
        failed = true;
        std::cerr << error.message << '\n';
    }

    if (failed || !report.ok()) {
        std::cerr << "Database check failed\n";
        return 1;
    }

    std::cout << "Database is valid\n";
    return 0;
}

// Reconstroi a raiz de todos os TableHeaps do arquivo a partir da cadeia de
// dados autodescritiva, tornando abriveis os heaps cujos contadores divergiram
// apos uma falha parcial. Este e o reparo estrutural (S11); a recuperacao
// baseada em WAL e um mecanismo separado, previsto para a Fase 5.
int command_db_repair(const std::filesystem::path& path) {
    // Abre o banco; o superbloco ja tolera paginas orfas de cauda (S1).
    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }

    // Localiza todas as raizes THRP percorrendo o inventario de paginas.
    // classify_page usa apenas os magic bytes, entao acha as raizes mesmo que
    // seus contadores estejam corrompidos a ponto de recusar a abertura.
    std::vector<modb::storage::PageId> roots;
    for (std::uint64_t id = 1; id < file->page_count(); ++id) {
        const modb::storage::PageId page_id{id};
        auto page = file->read(page_id);
        if (!page) {
            // Uma pagina ilegivel nao impede reparar as demais; registra e segue.
            std::cerr << "page " << id << ": " << page.error().message << '\n';
            continue;
        }
        if (modb::storage::classify_page(*page) ==
            modb::storage::PageKind::table_heap_root) {
            roots.push_back(page_id);
        }
    }

    std::cout << "Database repair: " << path << '\n';
    std::cout << "TableHeap roots found: " << roots.size() << '\n';

    // Repara cada raiz; uma raiz irreparavel (ciclo ou pagina de dados
    // corrompida) nao impede o reparo das demais.
    std::size_t rewritten = 0;
    std::size_t failed = 0;
    for (const auto root : roots) {
        auto report = modb::storage::repair_table_heap(*file, root);
        if (!report) {
            ++failed;
            std::cerr << "root " << root.value << ": " << report.error().message << '\n';
            continue;
        }
        std::cout << "  root " << root.value << ": pages=" << report->page_count
                  << " records=" << report->record_count
                  << (report->root_rewritten ? " (rewritten)" : " (already consistent)")
                  << '\n';
        if (report->root_rewritten) {
            ++rewritten;
        }
    }

    // Garante que as raizes reescritas cheguem ao disco.
    if (auto flushed = file->flush(); !flushed) {
        return print_error(flushed.error());
    }

    std::cout << "Roots rewritten: " << rewritten << '\n';
    if (failed != 0) {
        std::cerr << "Roots that could not be repaired: " << failed << '\n';
        return 1;
    }
    std::cout << "Database repair complete\n";
    return 0;
}

// Remove somente arquivos que passam pela validacao do formato moDb.
int command_db_delete(const std::filesystem::path& path) {
    {
        auto opened = modb::storage::PageFile::open(path);
        if (!opened) {
            return print_error(opened.error());
        }
    }

    std::error_code filesystem_error;
    // Tenta remover somente o caminho que acabou de ser validado.
    const auto removed = std::filesystem::remove(path, filesystem_error);
    if (filesystem_error) {
        return print_error(modb::Error{
            modb::ErrorCode::io_error,
            "could not delete database file: " + filesystem_error.message(),
        });
    }
    if (!removed) {
        return print_error(modb::Error{
            modb::ErrorCode::file_not_found,
            "database file not found: " + path.string(),
        });
    }
    // Confirma qual arquivo foi removido.
    std::cout << "Database deleted: " << path << '\n';
    // Encerra o comando com sucesso.
    return 0;
}

// Command group: page.

// Acrescenta uma pagina bruta para permitir inspecao fisica de storage.
int command_page_create(const std::filesystem::path& path) {
    auto opened = modb::storage::PageFile::open(path);
    if (!opened) {
        return print_error(opened.error());
    }
    auto allocated = opened->allocate_page();
    // Mostra uma falha de escrita ou de limite.
    if (!allocated) {
        return print_error(allocated.error());
    }
    if (auto flushed = opened->flush(); !flushed) {
        return print_error(flushed.error());
    }
    std::cout << "Allocated page: " << allocated->value << '\n';
    // Mostra a nova quantidade total.
    std::cout << "Page count: " << opened->page_count() << '\n';
    // Encerra o comando com sucesso.
    return 0;
}

// Mostra os bytes fisicos da pagina para depurar layout e persistencia.
int command_page_info(const std::filesystem::path& path, modb::storage::PageId id) {
    auto opened = modb::storage::PageFile::open(path);
    if (!opened) {
        return print_error(opened.error());
    }
    auto page = opened->read(id);
    if (!page) {
        return print_error(page.error());
    }

    // Cada linha do dump possui dezesseis bytes.
    constexpr std::size_t bytes_per_line = 16;
    // Percorre os offsets em blocos de dezesseis.
    for (std::size_t offset = 0; offset < modb::storage::page_size; offset += bytes_per_line) {
        std::cout << std::hex << std::setfill('0') << std::setw(8) << offset << "  ";
        // Mostra os dezesseis bytes da linha atual.
        for (std::size_t index = 0; index < bytes_per_line; ++index) {
            const auto byte = std::to_integer<unsigned int>((*page)[offset + index]);
            std::cout << std::setw(2) << byte << ' ';
        }
        // Encerra a linha do dump.
        std::cout << '\n';
    }
    std::cout << std::dec;
    // Encerra o comando com sucesso.
    return 0;
}

// Command group: record.

// Prepara uma pagina com o layout de slots usado pelos comandos de registro.
int command_record_page_create(const std::filesystem::path& path) {
    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    auto page_id = file->allocate_page();
    if (!page_id) {
        return print_error(page_id.error());
    }
    auto record_page = modb::storage::SlottedPage::create();
    if (auto written = file->write(*page_id, record_page.page()); !written) {
        return print_error(written.error());
    }
    if (auto flushed = file->flush(); !flushed) {
        return print_error(flushed.error());
    }
    std::cout << "Record page created: " << page_id->value << '\n';
    std::cout << "Free space: " << record_page.free_space() << " bytes\n";
    // Encerra o comando com sucesso.
    return 0;
}

// Persiste uma Row ja convertida em uma slotted page e informa seu endereco.
int command_record_insert_values(const std::filesystem::path& path, modb::storage::PageId page_id,
                       const modb::Row& row) {
    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    auto page = file->read(page_id);
    // Propaga PageId inexistente ou falha de leitura.
    if (!page) {
        return print_error(page.error());
    }
    // Valida o layout antes de inserir qualquer byte.
    auto record_page = modb::storage::SlottedPage::from_page(std::move(*page));
    if (!record_page) {
        return print_error(record_page.error());
    }

    auto encoded = modb::storage::encode_row(row);
    if (!encoded) {
        return print_error(encoded.error());
    }
    auto slot = record_page->insert(*encoded);
    if (!slot) {
        return print_error(slot.error());
    }
    auto generation = record_page->generation(*slot);
    if (!generation) {
        return print_error(generation.error());
    }
    if (auto written = file->write(page_id, record_page->page()); !written) {
        return print_error(written.error());
    }
    if (auto flushed = file->flush(); !flushed) {
        return print_error(flushed.error());
    }
    std::cout << "Record inserted: page " << page_id.value << ", slot " << slot->value
              << ", generation " << *generation << '\n';
    std::cout << "Free space: " << record_page->free_space() << " bytes\n";
    // Encerra o comando com sucesso.
    return 0;
}

// Mantem o atalho de duas colunas como wrapper da insercao tipada.
int command_record_insert(const std::filesystem::path& path, modb::storage::PageId page_id,
                          std::int64_t id, std::string_view name) {
    // Monta os dois valores aceitos por esse atalho.
    const modb::Row row{modb::Value{id}, modb::Value{name}};
    return command_record_insert_values(path, page_id, row);
}

// Le um slot ocupado e decodifica seus bytes de volta para valores relacionais.
int command_record_read(const std::filesystem::path& path, modb::storage::PageId page_id,
                        modb::storage::SlotId slot_id) {
    // Carrega uma slotted page completamente validada.
    auto record_page = load_record_page(path, page_id);
    if (!record_page) {
        return print_error(record_page.error());
    }
    // Copia os bytes apontados pelo slot.
    auto record = record_page->read(slot_id);
    if (!record) {
        return print_error(record.error());
    }
    auto row = modb::storage::decode_row(*record);
    if (!row) {
        return print_error(row.error());
    }
    std::cout << "Record " << page_id.value << ':' << slot_id.value << ": ";
    // Mostra os valores separados por barra vertical.
    print_row(*row);
    // Encerra o comando com sucesso.
    return 0;
}

// Lista os registros visiveis de uma pagina ignorando slots livres reutilizaveis.
int command_record_list(const std::filesystem::path& path, modb::storage::PageId page_id) {
    // Carrega uma slotted page completamente validada.
    auto record_page = load_record_page(path, page_id);
    if (!record_page) {
        return print_error(record_page.error());
    }
    // Mostra a quantidade antes de percorrer os slots.
    std::cout << "Records in page " << page_id.value << ": " << record_page->record_count() << '\n';
    for (const auto& info : record_page->slots()) {
        if (!info.occupied()) {
            continue;
        }
        const auto slot = info.id;
        // Copia os bytes do registro atual.
        auto record = record_page->read(slot);
        if (!record) {
            return print_error(record.error());
        }
        auto row = modb::storage::decode_row(*record);
        if (!row) {
            return print_error(row.error());
        }
        std::cout << "Slot " << slot.value << " (generation " << info.generation << "): ";
        // Mostra os valores relacionais.
        print_row(*row);
    }
    std::cout << "Free space: " << record_page->free_space() << " bytes\n";
    // Encerra o comando com sucesso.
    return 0;
}

// Explica offsets do diretorio de slots e da area de registros da pagina.
int command_record_page_layout(const std::filesystem::path& path,
                               modb::storage::PageId page_id) {
    auto record_page = load_record_page(path, page_id);
    // Mostra erros de arquivo, PageId ou layout corrompido.
    if (!record_page) {
        return print_error(record_page.error());
    }

    const auto free_start = static_cast<std::size_t>(record_page->free_start());
    const auto free_end = static_cast<std::size_t>(record_page->free_end());
    const auto slot_directory_start = modb::storage::slotted_page_header_size;
    // Seu tamanho termina exatamente em free_start.
    const auto slot_directory_size = free_start - slot_directory_start;
    const auto records_size = modb::storage::page_size - free_end;

    // Mostra identidade e propriedades gerais.
    std::cout << "Slotted page layout\n";
    std::cout << "PageId: " << page_id.value << '\n';
    std::cout << "Format version: "
              << static_cast<unsigned int>(modb::storage::slotted_page_format_version) << '\n';
    std::cout << "Total size: " << modb::storage::page_size << " bytes\n";
    std::cout << "Record count: " << record_page->record_count() << "\n\n";
    std::cout << "Slot count: " << record_page->slot_count() << '\n';
    std::cout << "Record capacity policy: exact logical size\n";
    std::cout << "Previous page: ";
    if (const auto previous = record_page->previous_page()) {
        std::cout << previous->value;
    } else {
        std::cout << "start";
    }
    std::cout << '\n';
    std::cout << "Next page: ";
    if (const auto next = record_page->next_page()) {
        std::cout << next->value;
    } else {
        std::cout << "end";
    }
    std::cout << "\n\n";

    std::cout << "Areas:\n";
    std::cout << "  Header:         [0, " << modb::storage::slotted_page_header_size
              << ") = " << modb::storage::slotted_page_header_size << " bytes\n";
    std::cout << "  Slot directory: [" << slot_directory_start << ", " << free_start
              << ") = " << slot_directory_size << " bytes\n";
    std::cout << "  Free space:     [" << free_start << ", " << free_end
              << ") = " << record_page->free_space() << " bytes\n";
    std::cout << "  Records:        [" << free_end << ", " << modb::storage::page_size
              << ") = " << records_size << " bytes\n\n";

    std::cout << "Logical layout:\n";
    std::cout << "  [ HEADER " << modb::storage::slotted_page_header_size << " B ]"
              << "[ SLOTS " << slot_directory_size << " B ]"
              << "[ FREE " << record_page->free_space() << " B ]"
              << "[ RECORDS " << records_size << " B ]\n\n";

    const auto slots = record_page->slots();
    if (slots.empty()) {
        std::cout << "Slots: none\n";
        return 0;
    }

    // Mostra o significado das colunas da tabela.
    std::cout << "Slots:\n";
    std::cout << "  Id  State  Gen  Directory entry  Record range       Size/Capacity\n";
    for (const auto& slot : slots) {
        const auto entry_start = modb::storage::slotted_page_header_size +
                                 static_cast<std::size_t>(slot.id.value) *
                                     modb::storage::slotted_page_slot_size;
        // Calcula o primeiro byte depois da entrada.
        const auto entry_end = entry_start + modb::storage::slotted_page_slot_size;
        std::cout << "  " << std::setw(3) << slot.id.value << "  "
                  << (slot.occupied() ? "used" : "free") << "  " << std::setw(3)
                  << slot.generation << "  [" << std::setw(4) << entry_start << ", "
                  << std::setw(4) << entry_end << ")    ";
        if (!slot.occupied()) {
            std::cout << "-                  0/0 bytes\n";
            continue;
        }
        // A faixa usa toda a capacidade, inclusive a margem reservada.
        const auto record_end =
            static_cast<std::size_t>(slot.record_offset) + slot.record_capacity;
        std::cout << "[" << std::setw(4) << slot.record_offset << ", " << std::setw(4)
                  << record_end << ")    " << slot.record_size << '/'
                  << slot.record_capacity << " bytes\n";
    }
    // Encerra o comando com sucesso.
    return 0;
}

// Command group: heap.

// Cria a raiz de metadados de um heap que pode crescer por varias paginas.
int command_heap_create(const std::filesystem::path& path) {
    // Abre e valida o arquivo de banco existente.
    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    auto heap = modb::storage::TableHeap::create(*file);
    if (!heap) {
        return print_error(heap.error());
    }
    if (auto flushed = file->flush(); !flushed) {
        return print_error(flushed.error());
    }
    std::cout << "TableHeap created\n";
    std::cout << "Root page: " << heap->root_page().value << '\n';
    // Encerra o comando com sucesso.
    return 0;
}

// Insere uma Row pelo TableHeap para encapsular escolha e crescimento de paginas.
int command_heap_insert_values(const std::filesystem::path& path,
                        modb::storage::PageId root_page, const modb::Row& row) {
    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    auto heap = modb::storage::TableHeap::open(*file, root_page);
    // Rejeita uma raiz comum, inexistente ou uma cadeia corrompida.
    if (!heap) {
        return print_error(heap.error());
    }
    auto encoded = modb::storage::encode_row(row);
    if (!encoded) {
        return print_error(encoded.error());
    }
    auto record_id = heap->insert(*encoded);
    // Mostra erros como registro grande demais ou falha de I/O.
    if (!record_id) {
        return print_error(record_id.error());
    }
    if (auto flushed = file->flush(); !flushed) {
        return print_error(flushed.error());
    }
    std::cout << "Record inserted: page " << record_id->page.value << ", slot "
              << record_id->slot.value << ", generation " << record_id->generation << '\n';
    // Encerra o comando com sucesso.
    return 0;
}

// Percorre a cadeia do heap em ordem fisica e decodifica cada registro vivo.
int command_heap_scan(const std::filesystem::path& path,
                      modb::storage::PageId root_page) {
    // Abre e valida o arquivo paginado.
    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    // Abre o heap e valida sua cadeia inteira.
    auto heap = modb::storage::TableHeap::open(*file, root_page);
    if (!heap) {
        return print_error(heap.error());
    }
    // Lê cada página uma única vez, trazendo endereço e bytes de cada registro.
    auto records = heap->scan_records();
    if (!records) {
        return print_error(records.error());
    }
    // Mostra a quantidade antes das linhas.
    std::cout << "Records in TableHeap " << root_page.value << ": " << records->size()
              << '\n';
    for (const auto& record : *records) {
        auto row = modb::storage::decode_row(record.bytes);
        if (!row) {
            return print_error(row.error());
        }
        std::cout << record.id.page.value << ':' << record.id.slot.value << ':'
                  << record.id.generation << " | ";
        print_row(*row);
    }
    // Encerra o comando com sucesso.
    return 0;
}

// Resume as paginas encadeadas do heap sem imprimir o conteudo das linhas.
int command_heap_layout(const std::filesystem::path& path,
                        modb::storage::PageId root_page) {
    // Abre e valida o banco antes de ler a cadeia.
    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    // Valida o heap a partir da raiz informada.
    auto heap = modb::storage::TableHeap::open(*file, root_page);
    if (!heap) {
        return print_error(heap.error());
    }
    auto pages = heap->layout();
    if (!pages) {
        return print_error(pages.error());
    }
    std::size_t total_records = 0;
    std::cout << "TableHeap layout\n";
    std::cout << "Root page: " << root_page.value << '\n';
    std::cout << "Page  Records  Free bytes  Next\n";
    for (const auto& page : *pages) {
        total_records += page.record_count;
        std::cout << page.id.value << "     " << page.record_count << "        "
                  << page.free_space << "        ";
        if (page.next) {
            std::cout << page.next->value;
        } else {
            std::cout << "end";
        }
        std::cout << '\n';
    }
    std::cout << "Pages: " << pages->size() << '\n';
    std::cout << "Records: " << total_records << '\n';
    // Encerra o comando com sucesso.
    return 0;
}

// Reconstroi os contadores da raiz a partir da cadeia apos uma falha parcial.
int command_heap_repair(const std::filesystem::path& path,
                        modb::storage::PageId root_page) {
    // Abre o banco; o superbloco ja tolera paginas orfas de cauda.
    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    // Reconstroi extremos e contadores percorrendo a cadeia de dados.
    auto report = modb::storage::repair_table_heap(*file, root_page);
    if (!report) {
        return print_error(report.error());
    }
    // Garante que a raiz reescrita chegue ao disco.
    if (auto flushed = file->flush(); !flushed) {
        return print_error(flushed.error());
    }
    std::cout << "TableHeap repair\n";
    std::cout << "Root page: " << root_page.value << '\n';
    std::cout << "Pages: " << report->page_count << '\n';
    std::cout << "Records: " << report->record_count << '\n';
    std::cout << "Root rewritten: " << (report->root_rewritten ? "yes" : "no") << '\n';
    // Encerra o comando com sucesso.
    return 0;
}

// Atualiza um registro deixando o TableHeap preservar ou mover o endereco.
int command_heap_update_values(const std::filesystem::path& path,
                        modb::storage::PageId root_page, modb::storage::RecordId id,
                        const modb::Row& row) {
    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    // Valida toda a cadeia antes de alterar o registro.
    auto heap = modb::storage::TableHeap::open(*file, root_page);
    if (!heap) {
        return print_error(heap.error());
    }
    // Codifica os novos valores relacionais.
    auto encoded = modb::storage::encode_row(row);
    if (!encoded) {
        return print_error(encoded.error());
    }
    auto updated = heap->update(id, *encoded);
    if (!updated) {
        return print_error(updated.error());
    }
    if (auto flushed = file->flush(); !flushed) {
        return print_error(flushed.error());
    }
    std::cout << "Record updated: page " << updated->page.value << ", slot "
              << updated->slot.value << ", generation " << updated->generation << '\n';
    return 0;
}

// Remove um registro validado por geracao e libera seu espaco reservado.
int command_heap_delete(const std::filesystem::path& path,
                        modb::storage::PageId root_page, modb::storage::RecordId id) {
    // Abre e valida o banco paginado.
    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    // Abre a cadeia indicada pela raiz.
    auto heap = modb::storage::TableHeap::open(*file, root_page);
    if (!heap) {
        return print_error(heap.error());
    }
    if (auto removed = heap->erase(id); !removed) {
        return print_error(removed.error());
    }
    if (auto flushed = file->flush(); !flushed) {
        return print_error(flushed.error());
    }
    std::cout << "Record deleted: " << id.page.value << ':' << id.slot.value << ':'
              << id.generation << '\n';
    return 0;
}

// Command group: codec.

// Demonstra o codec de Row com round-trip em memoria, sem arquivo de banco.
int command_codec_run() {
    // Cria uma linha representativa com INTEGER e TEXT.
    const modb::Row original{modb::Value{std::int64_t{1}}, modb::Value{"Ana"}};
    std::cout << "Original row: ";
    print_row(original);

    // Converte a linha completa para bytes.
    auto encoded = modb::storage::encode_row(original);
    if (!encoded) {
        return print_error(encoded.error());
    }
    std::cout << "Encoded size: " << encoded->size() << " bytes\n";

    auto decoded = modb::storage::decode_row(*encoded);
    // Mostra qualquer erro de leitura ou formato.
    if (!decoded) {
        return print_error(decoded.error());
    }
    std::cout << "Decoded row: ";
    print_row(*decoded);
    std::cout << "Round-trip: " << (*decoded == original ? "OK" : "FAILED") << '\n';
    return *decoded == original ? 0 : 1;
}

// Command group: types.

// Exercita o modelo de objetos do ODB++ (TypeDefinition/TypeRegistry/
// validate_object) inteiramente em memoria, sem tocar em armazenamento.
// A persistencia real chega na Fase 2 do plano ODB++ (docs/PLANO_ODB.md).
int command_types_run() {
    using modb::object::AttributeDefinition;
    using modb::object::AttributeType;
    using modb::object::AttributeValue;
    using modb::object::FieldId;
    using modb::object::FieldValues;
    using modb::object::TypeDefinition;
    using modb::object::TypeRegistry;

    // O atributo country e opcional e tem um default, ao contrario de name e
    // salary, que sao obrigatorios.
    AttributeDefinition country{
        .id = FieldId{3}, .name = "country", .type = AttributeType::string, .nullable = true};
    country.default_value = AttributeValue{"BR"};

    auto employee = TypeDefinition::create(
        "Employee",
        std::vector<AttributeDefinition>{
            AttributeDefinition{.id = FieldId{1}, .name = "name",
                               .type = AttributeType::string, .nullable = false},
            AttributeDefinition{.id = FieldId{2}, .name = "salary",
                               .type = AttributeType::float64, .nullable = false},
            country,
        });
    if (!employee) {
        return print_error(employee.error());
    }

    // O registro atribui o TypeDefinitionId; um TypeDefinition criado por
    // create() sozinho ainda nao tem identidade (id() == 0).
    TypeRegistry registry;
    auto type_id = registry.register_type(*employee);
    if (!type_id) {
        return print_error(type_id.error());
    }
    std::cout << "Registered type: Employee (id " << type_id->value << ")\n";

    auto registered = registry.find(*type_id);
    if (!registered) {
        return print_error(registered.error());
    }
    std::cout << "Attributes:\n";
    for (const auto& attribute : registered->get().attributes()) {
        std::cout << "  " << attribute.id.value << ": " << attribute.name << " ("
                  << modb::object::attribute_type_name(attribute.type)
                  << (attribute.nullable ? ", nullable" : ", not null");
        if (attribute.default_value) {
            std::cout << ", default: ";
            print_attribute_value(*attribute.default_value);
        }
        std::cout << ")\n";
    }

    // Um payload completo e compativel com o tipo.
    const FieldValues complete{
        {FieldId{1}, AttributeValue{"Ana"}},
        {FieldId{2}, AttributeValue{15000.0}},
        {FieldId{3}, AttributeValue{"US"}},
    };
    if (auto valid = modb::object::validate_object(registered->get(), complete); !valid) {
        return print_error(valid.error());
    }
    std::cout << "Valid object: ";
    print_field_values(complete);

    // country fica de fora do payload e e completado pelo default do tipo.
    const FieldValues using_default{
        {FieldId{1}, AttributeValue{"Beatriz"}},
        {FieldId{2}, AttributeValue{12000.0}},
    };
    if (auto valid = modb::object::validate_object(registered->get(), using_default); !valid) {
        return print_error(valid.error());
    }
    std::cout << "Valid object (country omitted, covered by its default): ";
    print_field_values(using_default);

    std::cout << "Note: this type registry exists only in memory; persistence arrives in "
                 "Fase 2 (see docs/PLANO_ODB.md).\n";
    return 0;
}

// Command groups: type and object (persistent ODB++ object store).

// Converte o nome textual de um tipo de atributo do CLI para a tag.
modb::Result<modb::object::AttributeType> parse_attribute_type(std::string_view name) {
    using modb::object::AttributeType;
    if (name == "boolean") {
        return AttributeType::boolean;
    }
    if (name == "int64") {
        return AttributeType::int64;
    }
    if (name == "float64") {
        return AttributeType::float64;
    }
    if (name == "string") {
        return AttributeType::string;
    }
    if (name == "bytes") {
        return AttributeType::bytes;
    }
    return std::unexpected(modb::Error{
        modb::ErrorCode::invalid_argument,
        "unknown attribute type; use boolean, int64, float64, string or bytes"});
}

// Converte um valor textual do CLI conforme o tipo declarado do atributo.
modb::Result<modb::object::AttributeValue> parse_attribute_value(
    modb::object::AttributeType type, std::string_view text) {
    using modb::object::AttributeType;
    using modb::object::AttributeValue;
    switch (type) {
    case AttributeType::boolean:
        if (text == "true") {
            return AttributeValue{true};
        }
        if (text == "false") {
            return AttributeValue{false};
        }
        return std::unexpected(
            modb::Error{modb::ErrorCode::invalid_argument, "boolean must be true or false"});
    case AttributeType::int64: {
        auto value = parse_integer(text);
        if (!value) {
            return std::unexpected(value.error());
        }
        return AttributeValue{*value};
    }
    case AttributeType::float64: {
        auto value = parse_real(text);
        if (!value) {
            return std::unexpected(value.error());
        }
        return AttributeValue{*value};
    }
    case AttributeType::string:
        return AttributeValue{text};
    case AttributeType::bytes:
    case AttributeType::null:
    case AttributeType::ref:
    case AttributeType::blob:
    case AttributeType::embedded:
        break;
    }
    return std::unexpected(modb::Error{modb::ErrorCode::invalid_argument,
                                       "this attribute type cannot be entered on the CLI yet"});
}

// Abre o ObjectStore de um arquivo existente, criando a hierarquia OO na
// primeira vez (um arquivo recém-criado por `db create` ainda não tem DBRT).
modb::Result<modb::object::ObjectStore> open_or_init_object_store(
    modb::storage::PageFile& file) {
    auto opened = modb::object::ObjectStore::open(file);
    if (opened) {
        return opened;
    }
    // Sem DBRT ainda: inicializa a hierarquia de objetos neste arquivo.
    if (opened.error().code == modb::ErrorCode::invalid_file_format) {
        return modb::object::ObjectStore::create(file);
    }
    return opened;
}

// Mostra os campos de um objeto decodificado.
void print_decoded_object(const modb::object::DecodedObject& object) {
    std::cout << "ObjectId: " << object.id.value << '\n';
    std::cout << "TypeDefinitionId: " << object.type.value << '\n';
    for (const auto& [field_id, value] : object.fields) {
        std::cout << "  " << field_id.value << " = ";
        print_attribute_value(value);
        std::cout << '\n';
    }
}

// modb type define <file> <name> <attr:type[:null]>...
int command_type_define(int argc, char* argv[]) {
    const std::filesystem::path path{argv[3]};
    const std::string type_name{argv[4]};

    // Cada argumento extra descreve um atributo; o FieldId é a posição (1-based).
    std::vector<modb::object::AttributeDefinition> attributes;
    for (int index = 5; index < argc; ++index) {
        const std::string_view spec{argv[index]};
        const auto first = spec.find(':');
        if (first == std::string_view::npos) {
            return print_usage_error("modb type define <file> <name> <attr:type[:null]>...");
        }
        const auto attr_name = spec.substr(0, first);
        auto rest = spec.substr(first + 1);
        bool nullable = false;
        const auto second = rest.find(':');
        if (second != std::string_view::npos) {
            if (rest.substr(second + 1) != "null") {
                return print_usage_error("modb type define <file> <name> <attr:type[:null]>...");
            }
            nullable = true;
            rest = rest.substr(0, second);
        }
        auto type = parse_attribute_type(rest);
        if (!type) {
            return print_error(type.error());
        }
        attributes.push_back(modb::object::AttributeDefinition{
            .id = modb::object::FieldId{static_cast<std::uint16_t>(index - 4)},
            .name = std::string{attr_name},
            .type = *type,
            .nullable = nullable});
    }

    auto definition = modb::object::TypeDefinition::create(type_name, std::move(attributes));
    if (!definition) {
        return print_error(definition.error());
    }

    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    auto store = open_or_init_object_store(*file);
    if (!store) {
        return print_error(store.error());
    }
    auto type_id = store->register_type(std::move(*definition));
    if (!type_id) {
        return print_error(type_id.error());
    }
    if (auto flushed = file->flush(); !flushed) {
        return print_error(flushed.error());
    }
    std::cout << "Type defined: " << type_name << " (id " << type_id->value << ")\n";
    return 0;
}

// modb type list <file>
int command_type_list(const std::filesystem::path& path) {
    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    auto store = modb::object::ObjectStore::open(*file);
    if (!store) {
        return print_error(store.error());
    }
    // A baseline corrente lista os tipos registrados.
    const auto& baseline = store->current_baseline();
    if (!baseline) {
        std::cout << "No types defined.\n";
        return 0;
    }
    for (const auto type_id : baseline->types()) {
        auto type = store->find_type(type_id);
        if (!type) {
            return print_error(type.error());
        }
        std::cout << type->get().name() << " (id " << type_id.value << ")\n";
        for (const auto& attribute : type->get().attributes()) {
            std::cout << "  " << attribute.id.value << ": " << attribute.name << " ("
                      << modb::object::attribute_type_name(attribute.type)
                      << (attribute.nullable ? ", nullable" : ", not null") << ")\n";
        }
    }
    return 0;
}

// modb object create <file> <type> <attr=value>...
int command_object_create(int argc, char* argv[]) {
    const std::filesystem::path path{argv[3]};
    const std::string type_name{argv[4]};

    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    auto store = modb::object::ObjectStore::open(*file);
    if (!store) {
        return print_error(store.error());
    }
    auto type = store->find_type(type_name);
    if (!type) {
        return print_error(type.error());
    }

    // Cada argumento extra é um par attr=value; atributos ausentes usam default
    // ou null (validado por create_object).
    modb::object::FieldValues fields;
    for (int index = 5; index < argc; ++index) {
        const std::string_view pair{argv[index]};
        const auto equals = pair.find('=');
        if (equals == std::string_view::npos) {
            return print_usage_error("modb object create <file> <type> <attr=value>...");
        }
        const auto attr_name = pair.substr(0, equals);
        const auto text = pair.substr(equals + 1);
        const auto* attribute = type->get().find(attr_name);
        if (attribute == nullptr) {
            return print_error(modb::Error{modb::ErrorCode::field_not_found,
                                           "type has no attribute named " + std::string{attr_name}});
        }
        auto value = parse_attribute_value(attribute->type, text);
        if (!value) {
            return print_error(value.error());
        }
        fields.emplace_back(attribute->id, std::move(*value));
    }

    auto id = store->create_object(type->get(), std::move(fields));
    if (!id) {
        return print_error(id.error());
    }
    if (auto flushed = file->flush(); !flushed) {
        return print_error(flushed.error());
    }
    std::cout << "Object created: id " << id->value << '\n';
    return 0;
}

// modb object get <file> <object-id>
int command_object_get(const std::filesystem::path& path, std::uint64_t object_id) {
    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    auto store = modb::object::ObjectStore::open(*file);
    if (!store) {
        return print_error(store.error());
    }
    auto object = store->get(modb::object::ObjectId{object_id});
    if (!object) {
        return print_error(object.error());
    }
    print_decoded_object(*object);
    return 0;
}

// modb object remove <file> <object-id>
int command_object_remove(const std::filesystem::path& path, std::uint64_t object_id) {
    auto file = modb::storage::PageFile::open(path);
    if (!file) {
        return print_error(file.error());
    }
    auto store = modb::object::ObjectStore::open(*file);
    if (!store) {
        return print_error(store.error());
    }
    if (auto removed = store->remove(modb::object::ObjectId{object_id}); !removed) {
        return print_error(removed.error());
    }
    if (auto flushed = file->flush(); !flushed) {
        return print_error(flushed.error());
    }
    std::cout << "Object removed: id " << object_id << '\n';
    return 0;
}

// Interpreta um ObjectId (u64 positivo) vindo da linha de comando.
modb::Result<std::uint64_t> parse_object_id(std::string_view text) {
    auto value = parse_integer(text);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (*value < 0) {
        return std::unexpected(
            modb::Error{modb::ErrorCode::invalid_argument, "object id must not be negative"});
    }
    return static_cast<std::uint64_t>(*value);
}

int run_type_command(int argc, char* argv[]) {
    if (argc == 2 || (argc == 3 && is_help_argument(argv[2]))) {
        print_type_help();
        return 0;
    }
    const std::string_view subcommand{argv[2]};
    if (subcommand == "define") {
        if (argc < 5) {
            return print_usage_error("modb type define <file> <name> <attr:type[:null]>...");
        }
        return command_type_define(argc, argv);
    }
    if (subcommand == "list") {
        if (argc != 4) {
            return print_usage_error("modb type list <file>");
        }
        return command_type_list(argv[3]);
    }
    std::cerr << "Unknown type command: " << subcommand << '\n';
    return 2;
}

int run_object_command(int argc, char* argv[]) {
    if (argc == 2 || (argc == 3 && is_help_argument(argv[2]))) {
        print_object_help();
        return 0;
    }
    const std::string_view subcommand{argv[2]};
    if (subcommand == "create") {
        if (argc < 5) {
            return print_usage_error("modb object create <file> <type> <attr=value>...");
        }
        return command_object_create(argc, argv);
    }
    if (subcommand == "get" || subcommand == "remove") {
        if (argc != 5) {
            return print_usage_error("modb object " + std::string{subcommand} +
                                     " <file> <object-id>");
        }
        auto object_id = parse_object_id(argv[4]);
        if (!object_id) {
            return print_error(object_id.error());
        }
        return subcommand == "get" ? command_object_get(argv[3], *object_id)
                                   : command_object_remove(argv[3], *object_id);
    }
    std::cerr << "Unknown object command: " << subcommand << '\n';
    return 2;
}

// Group dispatchers, kept after the command implementations.
int run_db_command(int argc, char* argv[]) {
    if (argc == 2 || (argc == 3 && is_help_argument(argv[2]))) {
        print_db_help();
        return 0;
    }
    const std::string_view subcommand{argv[2]};
    if (argc != 4) {
        return print_usage_error("modb db <create|info|check|repair|delete> <file>");
    }
    if (subcommand == "create") {
        return command_db_create(argv[3]);
    }
    if (subcommand == "info") {
        return command_db_info(argv[3]);
    }
    if (subcommand == "check") {
        return command_db_check(argv[3]);
    }
    if (subcommand == "repair") {
        return command_db_repair(argv[3]);
    }
    if (subcommand == "delete") {
        return command_db_delete(argv[3]);
    }
    std::cerr << "Unknown db command: " << subcommand << '\n';
    return 2;
}

int run_page_command(int argc, char* argv[]) {
    if (argc == 2 || (argc == 3 && is_help_argument(argv[2]))) {
        print_page_help();
        return 0;
    }
    const std::string_view subcommand{argv[2]};
    if (subcommand == "create") {
        if (argc != 4) {
            return print_usage_error("modb page create <file>");
        }
        return command_page_create(argv[3]);
    }
    if (subcommand == "info") {
        if (argc != 5) {
            return print_usage_error("modb page info <file> <page-id>");
        }
        auto page_id = parse_page_id(argv[4]);
        if (!page_id) {
            return print_error(page_id.error());
        }
        return command_page_info(argv[3], *page_id);
    }
    std::cerr << "Unknown page command: " << subcommand << '\n';
    return 2;
}

int run_record_command(int argc, char* argv[]) {
    if (argc == 2 || (argc == 3 && is_help_argument(argv[2]))) {
        print_record_help();
        return 0;
    }
    const std::string_view subcommand{argv[2]};
    if (subcommand == "page-create") {
        if (argc != 4) {
            return print_usage_error("modb record page-create <file>");
        }
        return command_record_page_create(argv[3]);
    }
    if (subcommand == "insert") {
        if (argc != 7) {
            return print_usage_error("modb record insert <file> <page-id> <id> <name>");
        }
        auto page_id = parse_page_id(argv[4]);
        auto id = parse_integer(argv[5]);
        if (!page_id) {
            return print_error(page_id.error());
        }
        if (!id) {
            return print_error(id.error());
        }
        return command_record_insert(argv[3], *page_id, *id, argv[6]);
    }
    if (subcommand == "insert-values") {
        if (argc < 6) {
            return print_usage_error(
                "modb record insert-values <file> <page-id> <typed-value>...");
        }
        auto page_id = parse_page_id(argv[4]);
        if (!page_id) {
            return print_error(page_id.error());
        }
        auto row = parse_typed_row(argc, argv, 5);
        if (!row) {
            return print_error(row.error());
        }
        return command_record_insert_values(argv[3], *page_id, *row);
    }
    if (subcommand == "read") {
        if (argc != 6) {
            return print_usage_error("modb record read <file> <page-id> <slot-id>");
        }
        auto page_id = parse_page_id(argv[4]);
        auto slot_id = parse_slot_id(argv[5]);
        if (!page_id) {
            return print_error(page_id.error());
        }
        if (!slot_id) {
            return print_error(slot_id.error());
        }
        return command_record_read(argv[3], *page_id, *slot_id);
    }
    if (subcommand == "list" || subcommand == "page-layout") {
        if (argc != 5) {
            return print_usage_error("modb record <list|page-layout> <file> <page-id>");
        }
        auto page_id = parse_page_id(argv[4]);
        if (!page_id) {
            return print_error(page_id.error());
        }
        if (subcommand == "list") {
            return command_record_list(argv[3], *page_id);
        }
        return command_record_page_layout(argv[3], *page_id);
    }
    std::cerr << "Unknown record command: " << subcommand << '\n';
    return 2;
}

int run_heap_command(int argc, char* argv[]) {
    if (argc == 2 || (argc == 3 && is_help_argument(argv[2]))) {
        print_heap_help();
        return 0;
    }
    const std::string_view subcommand{argv[2]};
    if (subcommand == "create") {
        if (argc != 4) {
            return print_usage_error("modb heap create <file>");
        }
        return command_heap_create(argv[3]);
    }
    if (subcommand == "insert-values") {
        if (argc < 6) {
            return print_usage_error(
                "modb heap insert-values <file> <root-page-id> <typed-value>...");
        }
        auto root_page = parse_page_id(argv[4]);
        if (!root_page) {
            return print_error(root_page.error());
        }
        auto row = parse_typed_row(argc, argv, 5);
        if (!row) {
            return print_error(row.error());
        }
        return command_heap_insert_values(argv[3], *root_page, *row);
    }
    if (subcommand == "scan" || subcommand == "layout") {
        if (argc != 5) {
            return print_usage_error("modb heap <scan|layout> <file> <root-page-id>");
        }
        auto root_page = parse_page_id(argv[4]);
        if (!root_page) {
            return print_error(root_page.error());
        }
        if (subcommand == "scan") {
            return command_heap_scan(argv[3], *root_page);
        }
        return command_heap_layout(argv[3], *root_page);
    }
    if (subcommand == "repair") {
        if (argc != 5) {
            return print_usage_error("modb heap repair <file> <root-page-id>");
        }
        auto root_page = parse_page_id(argv[4]);
        if (!root_page) {
            return print_error(root_page.error());
        }
        return command_heap_repair(argv[3], *root_page);
    }
    if (subcommand == "update-values") {
        if (argc < 9) {
            return print_usage_error(
                "modb heap update-values <file> <root> <page> <slot> <generation> "
                "<typed-value>...");
        }
        auto root = parse_page_id(argv[4]);
        auto page = parse_page_id(argv[5]);
        auto slot = parse_slot_id(argv[6]);
        auto generation = parse_generation(argv[7]);
        if (!root) {
            return print_error(root.error());
        }
        if (!page) {
            return print_error(page.error());
        }
        if (!slot) {
            return print_error(slot.error());
        }
        if (!generation) {
            return print_error(generation.error());
        }
        auto row = parse_typed_row(argc, argv, 8);
        if (!row) {
            return print_error(row.error());
        }
        return command_heap_update_values(
            argv[3], *root, modb::storage::RecordId{*page, *slot, *generation}, *row);
    }
    if (subcommand == "delete") {
        if (argc != 8) {
            return print_usage_error(
                "modb heap delete <file> <root> <page> <slot> <generation>");
        }
        auto root = parse_page_id(argv[4]);
        auto page = parse_page_id(argv[5]);
        auto slot = parse_slot_id(argv[6]);
        auto generation = parse_generation(argv[7]);
        if (!root) {
            return print_error(root.error());
        }
        if (!page) {
            return print_error(page.error());
        }
        if (!slot) {
            return print_error(slot.error());
        }
        if (!generation) {
            return print_error(generation.error());
        }
        return command_heap_delete(
            argv[3], *root, modb::storage::RecordId{*page, *slot, *generation});
    }
    std::cerr << "Unknown heap command: " << subcommand << '\n';
    return 2;
}

// Main dispatcher, after the group dispatchers.
int run(int argc, char* argv[]) {
    if (argc == 1) {
        print_help();
        return 0;
    }

    const std::string_view command{argv[1]};
    if (is_help_argument(command)) {
        if (argc != 2) {
            return print_usage_error("modb --help");
        }
        print_help();
        return 0;
    }
    if (command == "--version" || command == "-v") {
        if (argc != 2) {
            return print_usage_error("modb --version");
        }
        std::cout << modb::project_name() << ' ' << modb::project_version() << '\n';
        return 0;
    }
    if (command == "demo") {
        if (argc == 2) {
            return command_demo();
        }
        if (argc == 3 && is_help_argument(argv[2])) {
            print_demo_help();
            return 0;
        }
        if (std::string_view{argv[2]} == "run") {
            if (argc == 4 && is_help_argument(argv[3])) {
                print_command_help(
                    "modb demo run [-force]",
                    "Execute the complete guided tour using demo.modb. Use -force to "
                    "remove an existing demo.modb first.");
                return 0;
            }
            if (argc == 3) {
                return command_demo_run(false);
            }
            if (argc == 4 && (std::string_view{argv[3]} == "-force" ||
                              std::string_view{argv[3]} == "--force")) {
                return command_demo_run(true);
            }
            return print_usage_error("modb demo run [-force]");
        }
        std::cerr << "Unknown demo command: " << argv[2] << '\n';
        return 2;
    }
    if (command == "db") {
        return run_db_command(argc, argv);
    }
    if (command == "page") {
        return run_page_command(argc, argv);
    }
    if (command == "record") {
        return run_record_command(argc, argv);
    }
    if (command == "heap") {
        return run_heap_command(argc, argv);
    }
    if (command == "codec") {
        if (argc == 3 && is_help_argument(argv[2])) {
            print_codec_help();
            return 0;
        }
        if (argc != 2) {
            return print_usage_error("modb codec");
        }
        return command_codec_run();
    }
    if (command == "types") {
        if (argc == 3 && is_help_argument(argv[2])) {
            print_types_help();
            return 0;
        }
        if (argc != 2) {
            return print_usage_error("modb types");
        }
        return command_types_run();
    }
    if (command == "type") {
        return run_type_command(argc, argv);
    }
    if (command == "object") {
        return run_object_command(argc, argv);
    }
    std::cerr << "Unknown command: " << command << "\nUse --help for usage.\n";
    return 2;
}

// Helper used only by `demo run`.
int run_demo_step(std::vector<std::string> arguments) {
    std::cout << "\n>";
    for (const auto& argument : arguments) {
        std::cout << ' ' << argument;
    }
    std::cout << "\n\n";

    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    return run(static_cast<int>(argv.size()), argv.data());
}

// More generic helpers live at the end of the namespace.
bool is_help_argument(std::string_view argument) {
    return argument == "--help" || argument == "-h";
}

void print_command_help(std::string_view usage, std::string_view description) {
    std::cout << "Usage:\n  " << usage << "\n\n" << description << '\n';
}

int print_error(const modb::Error& error) {
    std::cerr << "Error: " << error.message << '\n';
    // Um informa ao sistema operacional que o comando falhou.
    return 1;
}

int print_usage_error(std::string_view usage) {
    std::cerr << "Usage: " << usage << '\n';
    // Dois representa uso incorreto da linha de comando.
    return 2;
}

void print_database_info(const modb::storage::PageFile& file) {
    // Mostra o caminho associado ao fluxo aberto.
    std::cout << "Path: " << file.path() << '\n';
    std::cout << "Format version: " << modb::storage::current_format_version << '\n';
    // Mostra o tamanho fixo usado pelo formato atual.
    std::cout << "Page size: " << modb::storage::page_size << '\n';
    // Mostra a quantidade que inclui o superbloco.
    std::cout << "Page count: " << file.page_count() << '\n';
}

// Converte o texto decimal de PageId para uint64_t.
modb::Result<modb::storage::PageId> parse_page_id(std::string_view text) {
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::unexpected(modb::Error{
            modb::ErrorCode::invalid_argument,
            "page-id must be an unsigned decimal integer",
        });
    }
    // Retorna o identificador fortemente tipado.
    return modb::storage::PageId{value};
}

// Converte o texto decimal de SlotId para uint16_t.
modb::Result<modb::storage::SlotId> parse_slot_id(std::string_view text) {
    auto parsed = parse_page_id(text);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (parsed->value > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(modb::Error{
            modb::ErrorCode::invalid_argument,
            "slot-id is greater than 65535",
        });
    }
    // Converte o valor validado para o tipo do slot.
    return modb::storage::SlotId{static_cast<std::uint16_t>(parsed->value)};
}

modb::Result<std::uint16_t> parse_generation(std::string_view text) {
    auto parsed = parse_slot_id(text);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (parsed->value == 0) {
        return std::unexpected(modb::Error{
            modb::ErrorCode::invalid_argument,
            "generation must be between 1 and 65535",
        });
    }
    return parsed->value;
}

modb::Result<std::int64_t> parse_integer(std::string_view text) {
    std::int64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::unexpected(modb::Error{
            modb::ErrorCode::invalid_argument,
            "id must be a signed 64-bit decimal integer",
        });
    }
    // Retorna o INTEGER pronto para formar a Row.
    return value;
}

// Converte um texto decimal para o tipo REAL usado por Value.
modb::Result<double> parse_real(std::string_view text) {
    double value = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::unexpected(modb::Error{
            modb::ErrorCode::invalid_argument,
            "REAL must contain a valid decimal number",
        });
    }
    // O formato inicial rejeita infinito e NaN.
    if (!std::isfinite(value)) {
        return std::unexpected(modb::Error{
            modb::ErrorCode::invalid_argument,
            "REAL must be finite",
        });
    }
    return value;
}

// Converte a sintaxe tipo:conteudo para um Value relacional.
modb::Result<modb::Value> parse_typed_value(std::string_view argument) {
    if (argument == "null") {
        return modb::Value{modb::Null{}};
    }

    const auto separator = argument.find(':');
    // Todos os tipos diferentes de NULL precisam do separador.
    if (separator == std::string_view::npos || separator == 0) {
        return std::unexpected(modb::Error{
            modb::ErrorCode::invalid_argument,
            "typed value must use type:value or null",
        });
    }
    // A parte anterior aos dois-pontos identifica o tipo.
    const auto type = argument.substr(0, separator);
    const auto content = argument.substr(separator + 1);

    if (type == "boolean") {
        if (content == "true") {
            return modb::Value{true};
        }
        if (content == "false") {
            return modb::Value{false};
        }
        return std::unexpected(modb::Error{
            modb::ErrorCode::invalid_argument,
            "BOOLEAN must be boolean:true or boolean:false",
        });
    }

    if (type == "integer") {
        auto integer = parse_integer(content);
        if (!integer) {
            return std::unexpected(integer.error());
        }
        return modb::Value{*integer};
    }

    if (type == "real") {
        auto real = parse_real(content);
        if (!real) {
            return std::unexpected(real.error());
        }
        return modb::Value{*real};
    }

    // TEXT preserva todos os bytes depois do primeiro separador.
    if (type == "text") {
        return modb::Value{content};
    }

    // Informa quais prefixos a CLI reconhece atualmente.
    return std::unexpected(modb::Error{
        modb::ErrorCode::invalid_argument,
        "unknown value type; use null, boolean, integer, real or text",
    });
}

modb::Result<modb::Row> parse_typed_row(int argc, char* argv[], int first_value) {
    // Reserva exatamente a quantidade de valores recebidos.
    std::vector<modb::Value> values;
    values.reserve(static_cast<std::size_t>(argc - first_value));
    // Converte cada argumento sem alterar a ordem das colunas.
    for (int index = first_value; index < argc; ++index) {
        auto value = parse_typed_value(argv[index]);
        // Propaga imediatamente um valor malformado.
        if (!value) {
            return std::unexpected(value.error());
        }
        // Transfere o valor validado para a linha.
        values.push_back(std::move(*value));
    }
    return modb::Row{std::move(values)};
}

modb::Result<modb::storage::SlottedPage>
load_record_page(const std::filesystem::path& path, modb::storage::PageId page_id) {
    // Abre e valida o arquivo moDb.
    auto file = modb::storage::PageFile::open(path);
    // Propaga erros de arquivo ou superbloco.
    if (!file) {
        return std::unexpected(file.error());
    }
    auto page = file->read(page_id);
    // Propaga PageId inexistente ou falha de I/O.
    if (!page) {
        return std::unexpected(page.error());
    }
    return modb::storage::SlottedPage::from_page(std::move(*page));
}

void print_value(const modb::Value& value) {
    // Visita a alternativa atualmente armazenada no variant.
    std::visit(
        Overloaded{
            [](modb::Null) { std::cout << "NULL"; },
            // Mostra booleanos com palavras em vez de zero e um.
            [](bool boolean) { std::cout << (boolean ? "true" : "false"); },
            // Mostra o inteiro diretamente.
            [](std::int64_t integer) { std::cout << integer; },
            [](double real) { std::cout << real; },
            // Escapa bytes de controle antes de exibir texto de um .db não confiável.
            [](const std::string& text) { std::cout << modb::escape_for_terminal(text); },
        },
        value.storage());
}

// Mostra todos os Values de uma linha separados por uma barra vertical.
void print_row(const modb::Row& row) {
    bool first = true;
    // Percorre os valores na ordem das colunas.
    for (const auto& value : row.values()) {
        // Evita escrever um separador antes do primeiro valor.
        if (!first) {
            std::cout << " | ";
        }
        // Mostra o valor atual conforme seu tipo.
        print_value(value);
        first = false;
    }
    // Encerra a linha exibida no console.
    std::cout << '\n';
}

// Mostra um AttributeValue conforme sua tag ativa (modb::object::AttributeType).
void print_attribute_value(const modb::object::AttributeValue& value) {
    std::visit(
        Overloaded{
            [](modb::object::AttributeNull) { std::cout << "NULL"; },
            [](bool boolean) { std::cout << (boolean ? "true" : "false"); },
            [](std::int64_t integer) { std::cout << integer; },
            [](double real) { std::cout << real; },
            // Escapa bytes de controle antes de exibir texto de origem não confiável.
            [](const std::string& text) { std::cout << modb::escape_for_terminal(text); },
            [](const std::vector<std::byte>& bytes) {
                std::cout << bytes.size() << " bytes: " << std::hex << std::setfill('0');
                for (const auto byte : bytes) {
                    std::cout << std::setw(2)
                              << static_cast<unsigned>(std::to_integer<unsigned char>(byte));
                }
                std::cout << std::dec;
            },
            [](modb::object::ObjectId id) { std::cout << "ObjectId(" << id.value << ")"; },
            [](modb::object::BlobId id) { std::cout << "BlobId(" << id.value << ")"; },
        },
        value.storage());
}

// Mostra todos os valores de um FieldValues separados por uma barra vertical.
void print_field_values(const modb::object::FieldValues& fields) {
    bool first = true;
    // Percorre os campos na ordem em que foram fornecidos.
    for (const auto& [field_id, value] : fields) {
        if (!first) {
            std::cout << " | ";
        }
        std::cout << field_id.value << '=';
        print_attribute_value(value);
        first = false;
    }
    // Encerra a linha exibida no console.
    std::cout << '\n';
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        // Os erros operacionais normais continuam sendo tratados por Result.
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Fatal error: unknown exception\n";
        return 1;
    }
}
