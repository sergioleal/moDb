// Importa códigos de erro verificados neste teste.
#include "modb/error.hpp"
// Importa Page, PageId e page_size.
#include "modb/storage/page.hpp"
// Importa a classe que cria, abre, lê e escreve arquivos paginados.
#include "modb/storage/page_file.hpp"
// Importa a área fixa de buffers reutilizáveis.
#include "modb/storage/scratch_page_pool.hpp"

// Importa as funções simples de verificação dos testes.
#include "test_support.hpp"

// Disponibiliza um valor variável usado para gerar nomes únicos.
#include <chrono>
// Disponibiliza std::byte e std::size_t.
#include <cstddef>
// Disponibiliza caminhos, arquivos temporários e operações de arquivo.
#include <filesystem>
// Disponibiliza a alteração direta de bytes nos testes de corrupção.
#include <fstream>
// Disponibiliza a impressão do caminho no console.
#include <iostream>
// Disponibiliza o PageId opcional comparado nos testes de catalog root.
#include <optional>
// Disponibiliza a exceção usada para rejeitar capacidade zero.
#include <stdexcept>
// Disponibiliza a montagem dos nomes temporários.
#include <string>
// Disponibiliza std::error_code para limpeza sem exceções.
#include <system_error>

namespace {

// Cria um caminho temporário único e o remove ao final do teste.
class TemporaryDatabase {
public:
    // Monta o nome do arquivo usando um sufixo descritivo.
    explicit TemporaryDatabase(std::string_view suffix) {
        // Usa o relógio monotônico para reduzir colisões entre execuções.
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        // Combina a pasta temporária do sistema com o nome do teste.
        path_ = std::filesystem::temp_directory_path() /
                ("modb-storage-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }

    // Remove o arquivo temporário automaticamente.
    ~TemporaryDatabase() {
        // Recebe um possível erro sem permitir que o destrutor lance exceção.
        std::error_code ignored;
        // Tenta remover o arquivo e ignora uma falha durante a limpeza.
        std::filesystem::remove(path_, ignored);
    }

    // Retorna o caminho usado pelo teste sem copiá-lo.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    // Guarda o caminho completo do arquivo temporário.
    std::filesystem::path path_;
};

// Altera um byte específico para simular um arquivo corrompido.
void overwrite_byte(const std::filesystem::path& path, std::streamoff offset, char value) {
    // Abre o mesmo arquivo para leitura e escrita binária.
    std::fstream stream{path, std::ios::binary | std::ios::in | std::ios::out};
    // Posiciona a escrita no byte solicitado.
    stream.seekp(offset);
    // Sobrescreve somente esse byte.
    stream.put(value);
}

} // namespace

// Executa todos os cenários do armazenamento paginado.
int main() {
    // Evita repetir modb:: nos códigos de erro.
    using namespace modb;
    // Evita repetir modb::storage:: nas classes de página.
    using namespace modb::storage;

    // Acumula e mostra as falhas encontradas.
    TestSuite suite;

    // O pool rejeita configurações que nunca poderiam entregar um buffer.
    bool zero_capacity_rejected = false;
    try {
        ScratchPagePool invalid_pool{0};
    } catch (const std::invalid_argument&) {
        zero_capacity_rejected = true;
    }
    suite.check(zero_capacity_rejected, "scratch page pool rejects zero capacity");

    // Um buffer liberado volta ao pool sem criar outra área Page.
    ScratchPagePool scratch_pool{1};
    Page* first_address = nullptr;
    {
        auto handle = scratch_pool.acquire();
        first_address = &handle.get();
        handle.get()[0] = std::byte{0x5aU};
        suite.check(!scratch_pool.try_acquire().has_value(),
                    "checked-out scratch page is unavailable");
    }
    {
        auto handle = scratch_pool.acquire();
        suite.check(&handle.get() == first_address && handle.get()[0] == std::byte{0x5aU},
                    "released scratch page reuses the same storage");
    }
    // Reserva um caminho temporário para o teste principal de reabertura.
    TemporaryDatabase database{"roundtrip"};
    // Mostra no console qual arquivo está sendo usado.
    std::cout << "Temporary database: " << database.path() << '\n';
    // Prepara a página cujo conteúdo será persistido.
    Page expected;
    // Preenche toda a página com um padrão conhecido e repetível.
    for (std::size_t index = 0; index < page_size; ++index) {
        // O módulo mantém o número dentro de um byte e cria variação no conteúdo.
        expected[index] = std::byte{static_cast<unsigned char>(index % 251U)};
    }

    // Limita a vida do primeiro PageFile para forçar seu fechamento.
    {
        // Tenta criar um banco novo no caminho temporário.
        auto created = PageFile::create(database.path());
        // Confere se o arquivo foi criado.
        suite.check(created.has_value(), "database file is created");
        // Interrompe os cenários dependentes se a criação falhar.
        if (!created) {
            return suite.finish();
        }
        // Move o PageFile para uma variável que possui o arquivo aberto.
        auto file = std::move(*created);
        // Um banco novo deve possuir apenas a página zero.
        suite.check(file.page_count() == 1, "new file contains only the superblock");

        // Aloca a primeira página disponível para dados.
        auto allocated = file.allocate_page();
        // A primeira página de dados deve receber o identificador um.
        suite.check(allocated == Result<PageId>{PageId{1}}, "first data page has id 1");
        // Só tenta escrever quando a alocação funcionou.
        if (allocated) {
            // Grava o padrão conhecido na página recém-alocada.
            suite.check(file.write(*allocated, expected).has_value(), "allocated page is writable");
        }
        // Aloca uma segunda página para verificar a sequência dos identificadores.
        auto second_page = file.allocate_page();
        // A segunda página de dados deve receber o identificador dois.
        suite.check(second_page == Result<PageId>{PageId{2}}, "pages receive sequential ids");
        // Envia todas as alterações pendentes ao sistema operacional.
        suite.check(file.flush().has_value(), "database changes are flushed");
        // Confere que uma página nunca alocada não pode ser lida.
        suite.check_error(file.read(PageId{99}), ErrorCode::page_not_found,
                          "unknown page cannot be read");
        // Confere que a API pública protege o superbloco.
        suite.check_error(file.write(superblock_page_id, Page{}), ErrorCode::reserved_page,
                          "superblock cannot be overwritten through the public API");
    }

    // Abre um novo PageFile depois que o objeto anterior foi destruído.
    {
        // Tenta abrir e validar o mesmo arquivo.
        auto opened = PageFile::open(database.path());
        // Confere se a reabertura funcionou.
        suite.check(opened.has_value(), "database file is reopened");
        // Só consulta o arquivo quando ele foi aberto.
        if (opened) {
            // Superbloco e duas páginas de dados totalizam três páginas.
            suite.check(opened->page_count() == 3, "multiple pages survive reopening");
            // Lê novamente a página que recebeu o padrão conhecido.
            auto actual = opened->read(PageId{1});
            // Compara todos os bytes lidos com os bytes gravados.
            suite.check(actual.has_value() && *actual == expected,
                        "page bytes survive closing and reopening");
            // A sobrecarga reutilizável preenche diretamente o buffer fornecido.
            auto scratch = scratch_pool.acquire();
            suite.check(opened->read(PageId{1}, scratch.get()).has_value() &&
                            scratch.get() == expected,
                        "page file reads directly into reusable storage");
            // Lê a segunda página, que nunca recebeu outro conteúdo.
            auto zero_page = opened->read(PageId{2});
            // Confere que a alocação inicializou todos os bytes com zero.
            suite.check(zero_page.has_value() && *zero_page == Page{},
                        "newly allocated pages are zero initialized");
        }
    }

    // Confere que create não apaga um banco já existente.
    suite.check_error(PageFile::create(database.path()), ErrorCode::file_already_exists,
                      "create never overwrites an existing database");
    // Gera um caminho que não receberá arquivo algum.
    TemporaryDatabase missing{"missing"};
    // Confere o erro específico para um arquivo inexistente.
    suite.check_error(PageFile::open(missing.path()), ErrorCode::file_not_found,
                      "missing database is reported");

    // Prepara um banco separado para corromper sua assinatura.
    TemporaryDatabase bad_magic{"magic"};
    // Cria e fecha o arquivo válido antes de alterá-lo diretamente.
    {
        // Cria um superbloco inicialmente correto.
        auto file = PageFile::create(bad_magic.path());
        // Confere a preparação deste cenário.
        suite.check(file.has_value(), "magic test database is created");
    }
    // Troca o primeiro caractere de MODB por X.
    overwrite_byte(bad_magic.path(), 0, 'X');
    // Confere que a abertura rejeita a assinatura alterada.
    suite.check_error(PageFile::open(bad_magic.path()), ErrorCode::invalid_file_format,
                      "invalid magic number is rejected");

    // Prepara outro banco para simular uma versão desconhecida.
    TemporaryDatabase bad_version{"version"};
    // Cria e fecha o arquivo antes da alteração.
    {
        // Cria um superbloco com a versão atual.
        auto file = PageFile::create(bad_version.path());
        // Confere a preparação deste cenário.
        suite.check(file.has_value(), "version test database is created");
    }
    // Altera o byte menos significativo da versão para dois.
    overwrite_byte(bad_version.path(), 4, 2);
    // Confere que a versão desconhecida não é interpretada.
    suite.check_error(PageFile::open(bad_version.path()),
                      ErrorCode::incompatible_format_version,
                      "unknown format version is rejected");

    // Prepara outro banco para simular truncamento físico.
    TemporaryDatabase truncated{"truncated"};
    // Cria e fecha uma página completa antes do truncamento.
    {
        // Cria um arquivo inicialmente válido.
        auto file = PageFile::create(truncated.path());
        // Confere a preparação deste cenário.
        suite.check(file.has_value(), "truncation test database is created");
    }
    // Recebe o possível erro de resize_file sem lançar exceção.
    std::error_code resize_error;
    // Remove o último byte da página zero.
    std::filesystem::resize_file(truncated.path(), page_size - 1, resize_error);
    // Confere se o sistema permitiu truncar o arquivo do teste.
    suite.check(!resize_error, "test database is truncated");
    // Confere que uma página incompleta é tratada como corrupção.
    suite.check_error(PageFile::open(truncated.path()), ErrorCode::corrupt_file,
                      "truncated database is rejected");

    // Um crash entre estender o arquivo e gravar o superbloco deixa uma página órfã.
    TemporaryDatabase orphan{"orphan-tail"};
    {
        // Cria o banco e aloca uma página de dados para termos uma cauda conhecida.
        auto file = PageFile::create(orphan.path());
        suite.check(file.has_value(), "orphan-tail test database is created");
        if (file) {
            suite.check(file->allocate_page().has_value(), "orphan-tail base page is allocated");
            suite.check(file->flush().has_value(), "orphan-tail base page is flushed");
        }
    }
    // Estende o arquivo em uma página inteira sem atualizar o superbloco.
    std::error_code grow_error;
    const auto physical_size = std::filesystem::file_size(orphan.path());
    std::filesystem::resize_file(orphan.path(), physical_size + page_size, grow_error);
    suite.check(!grow_error, "orphan-tail file is extended by one physical page");
    {
        // A abertura tolera a página órfã em vez de recusar o banco inteiro.
        auto reopened = PageFile::open(orphan.path());
        suite.check(reopened.has_value(), "database with an orphan trailing page still opens");
        if (reopened) {
            // A contagem lógica ignora a página órfã de cauda.
            suite.check(reopened->page_count() == 2, "orphan trailing page is not counted");
            // A próxima alocação reaproveita o espaço órfão e volta a ser consistente.
            auto reused = reopened->allocate_page();
            suite.check(reused == Result<PageId>{PageId{2}},
                        "next allocation reuses the orphan slot");
            suite.check(reopened->flush().has_value(), "reconciled database is flushed");
        }
    }
    {
        // Depois de reconciliar, o arquivo abre sem depender da tolerância.
        auto verified = PageFile::open(orphan.path());
        suite.check(verified.has_value() && verified->page_count() == 3,
                    "reconciled database reopens with the expected page count");
    }

    // O catalog root sobrevive às reescritas do superbloco feitas por allocate_page.
    TemporaryDatabase catalog{"catalog-root"};
    {
        auto file = PageFile::create(catalog.path());
        suite.check(file.has_value(), "catalog-root test database is created");
        if (file) {
            // Um banco novo ainda não possui raiz de catálogo.
            suite.check(!file->catalog_root().has_value(), "new database has no catalog root");
            auto page = file->allocate_page();
            suite.check(page == Result<PageId>{PageId{1}}, "catalog-root page is allocated");
            // Aponta a raiz do catálogo para a página recém-alocada.
            suite.check(file->set_catalog_root(PageId{1}).has_value(), "catalog root is set");
            // Uma nova alocação reescreve o superbloco e não pode apagar a raiz.
            suite.check(file->allocate_page() == Result<PageId>{PageId{2}},
                        "allocation after setting the catalog root succeeds");
            suite.check(file->catalog_root() == std::optional<PageId>{PageId{1}},
                        "catalog root survives further allocations in memory");
            suite.check(file->flush().has_value(), "catalog-root database is flushed");
        }
    }
    {
        // A raiz persistida precisa continuar disponível após reabrir o arquivo.
        auto reopened = PageFile::open(catalog.path());
        suite.check(reopened.has_value(), "catalog-root database reopens");
        if (reopened) {
            suite.check(reopened->catalog_root() == std::optional<PageId>{PageId{1}},
                        "catalog root survives closing and reopening");
        }
    }
    {
        // A raiz do catálogo não pode ser o superbloco nem uma página inexistente.
        auto file = PageFile::open(catalog.path());
        suite.check(file.has_value(), "catalog-root database reopens for validation");
        if (file) {
            suite.check_error(file->set_catalog_root(PageId{0}), ErrorCode::invalid_argument,
                              "catalog root cannot be the superblock page");
            suite.check_error(file->set_catalog_root(PageId{999}), ErrorCode::page_not_found,
                              "catalog root must reference an existing page");
        }
    }

    // Encerra o processo com o resultado de todos os cenários.
    return suite.finish();
}
