// Importa PageFile, Page e PageId.
#include "modb/storage/page_file.hpp"

// Disponibiliza algoritmos como copy e equal.
#include <algorithm>
// Disponibiliza blocos de tamanho fixo.
#include <array>
// Disponibiliza std::byte, std::size_t e to_integer.
#include <cstddef>
// Disponibiliza inteiros com largura definida.
#include <cstdint>
// Disponibiliza os limites máximos dos tipos inteiros.
#include <limits>
// Disponibiliza a raiz de catálogo opcional lida do superbloco.
#include <optional>
// Disponibiliza mensagens e conversões para texto.
#include <string>
// Disponibiliza std::error_code para erros do filesystem.
#include <system_error>

namespace modb::storage {
namespace {

// Os quatro primeiros bytes identificam um arquivo do moDb.
constexpr std::array<std::byte, 4> magic{
    // Primeiro caractere da assinatura.
    std::byte{'M'},
    // Segundo caractere da assinatura.
    std::byte{'O'},
    // Terceiro caractere da assinatura.
    std::byte{'D'},
    // Quarto caractere da assinatura.
    std::byte{'B'},
};
// A versão começa no byte 4 do superbloco.
constexpr std::size_t version_offset = 4;
// O tamanho da página começa no byte 8.
constexpr std::size_t page_size_offset = 8;
// A quantidade de páginas começa no byte 12.
constexpr std::size_t page_count_offset = 12;
// A futura raiz do catálogo começa no byte 20.
constexpr std::size_t catalog_root_offset = 20;
// O maior uint64_t representa a ausência de uma página.
constexpr std::uint64_t no_page = std::numeric_limits<std::uint64_t>::max();

// Facilita a criação de erros de entrada e saída.
Error io_error(std::string message) { return Error{ErrorCode::io_error, std::move(message)}; }

// Codifica um inteiro de 16 bits em ordem little-endian.
void encode_u16(std::span<std::byte, page_size> target, std::size_t offset,
                std::uint16_t value) {
    // Escreve um byte do valor a cada repetição.
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        // Guarda primeiro o byte menos significativo.
        target[offset + index] = std::byte{static_cast<unsigned char>(value & 0xffU)};
        // Remove o byte que acabou de ser escrito.
        value = static_cast<std::uint16_t>(value >> 8U);
    }
}

// Codifica um inteiro de 32 bits em ordem little-endian.
void encode_u32(std::span<std::byte, page_size> target, std::size_t offset,
                std::uint32_t value) {
    // Percorre os quatro bytes do inteiro.
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        // Escreve o byte menos significativo na posição atual.
        target[offset + index] = std::byte{static_cast<unsigned char>(value & 0xffU)};
        // Prepara o próximo byte.
        value >>= 8U;
    }
}

// Codifica um inteiro de 64 bits em ordem little-endian.
void encode_u64(std::span<std::byte, page_size> target, std::size_t offset,
                std::uint64_t value) {
    // Percorre os oito bytes do inteiro.
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        // Escreve o byte menos significativo na posição atual.
        target[offset + index] = std::byte{static_cast<unsigned char>(value & 0xffU)};
        // Prepara o próximo byte.
        value >>= 8U;
    }
}

// Reconstrói um inteiro de 16 bits armazenado em little-endian.
std::uint16_t decode_u16(std::span<const std::byte, page_size> source, std::size_t offset) {
    // Começa com todos os bits desligados.
    std::uint16_t value = 0;
    // Lê os dois bytes armazenados.
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        // Move o byte atual de volta para sua posição original no inteiro.
        const auto shifted = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(std::to_integer<unsigned char>(source[offset + index]))
            << (index * 8U));
        // Combina o byte atual com os bytes já reconstruídos.
        value = static_cast<std::uint16_t>(value | shifted);
    }
    // Devolve o inteiro reconstruído.
    return value;
}

// Reconstrói um inteiro de 32 bits armazenado em little-endian.
std::uint32_t decode_u32(std::span<const std::byte, page_size> source, std::size_t offset) {
    // Começa com todos os bits desligados.
    std::uint32_t value = 0;
    // Lê os quatro bytes armazenados.
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        // Converte o byte e o desloca para sua posição original.
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(source[offset + index]))
                 << (index * 8U);
    }
    // Devolve o inteiro reconstruído.
    return value;
}

// Reconstrói um inteiro de 64 bits armazenado em little-endian.
std::uint64_t decode_u64(std::span<const std::byte, page_size> source, std::size_t offset) {
    // Começa com todos os bits desligados.
    std::uint64_t value = 0;
    // Lê os oito bytes armazenados.
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        // Converte o byte e o desloca para sua posição original.
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(source[offset + index]))
                 << (index * 8U);
    }
    // Devolve o inteiro reconstruído.
    return value;
}

// Monta a página zero com os metadados atuais do arquivo.
Page make_superblock(std::uint64_t page_count, std::optional<PageId> catalog_root) {
    // Cria uma página inicialmente preenchida com zeros.
    Page page;
    // Copia a assinatura MODB para os quatro primeiros bytes.
    std::copy(magic.begin(), magic.end(), page.bytes().begin());
    // Grava a versão do formato no offset definido.
    encode_u16(page.bytes(), version_offset, current_format_version);
    // Grava o tamanho fixo das páginas.
    encode_u32(page.bytes(), page_size_offset, static_cast<std::uint32_t>(page_size));
    // Grava quantas páginas o arquivo possui.
    encode_u64(page.bytes(), page_count_offset, page_count);
    // Preserva a raiz do catálogo; o sentinela indica que ela ainda não existe.
    encode_u64(page.bytes(), catalog_root_offset, catalog_root ? catalog_root->value : no_page);
    // Retorna o superbloco pronto para escrita.
    return page;
}

// Converte o campo persistido da raiz do catálogo em um PageId opcional.
std::optional<PageId> decode_catalog_root(const Page& superblock) {
    // Reconstrói o valor armazenado no offset da raiz.
    const auto stored = decode_u64(superblock.bytes(), catalog_root_offset);
    // O sentinela representa a ausência de uma raiz de catálogo.
    if (stored == no_page) {
        return std::nullopt;
    }
    // Qualquer outro valor identifica a página raiz.
    return PageId{stored};
}

// Lê exatamente uma página do ponto atual do fluxo.
Result<void> read_exact(std::fstream& stream, Page& page) {
    // Pede ao fluxo para preencher todos os bytes da página.
    stream.read(reinterpret_cast<char*>(page.bytes().data()),
                static_cast<std::streamsize>(page_size));
    // Uma leitura menor significa arquivo truncado ou falha de I/O.
    if (stream.gcount() != static_cast<std::streamsize>(page_size)) {
        return std::unexpected(Error{ErrorCode::corrupt_file, "could not read a complete page"});
    }
    // A página inteira foi lida.
    return {};
}

// Confere os campos do superbloco e retorna a quantidade de páginas.
Result<std::uint64_t> validate_superblock(const Page& superblock, std::uintmax_t file_size) {
    // Compara os quatro primeiros bytes com a assinatura MODB.
    if (!std::equal(magic.begin(), magic.end(), superblock.bytes().begin())) {
        return std::unexpected(
            Error{ErrorCode::invalid_file_format, "file does not contain the moDb magic number"});
    }

    // Reconstrói a versão armazenada nos bytes 4 e 5.
    const auto version = decode_u16(superblock.bytes(), version_offset);
    // Impede que esta implementação interprete outro formato por engano.
    if (version != current_format_version) {
        return std::unexpected(Error{
            ErrorCode::incompatible_format_version,
            "unsupported moDb format version: " + std::to_string(version),
        });
    }

    // Reconstrói o tamanho de página gravado no arquivo.
    const auto stored_page_size = decode_u32(superblock.bytes(), page_size_offset);
    // O tamanho armazenado precisa ser igual ao tamanho usado pelo código.
    if (stored_page_size != page_size) {
        return std::unexpected(
            Error{ErrorCode::corrupt_file, "stored page size does not match the moDb page size"});
    }

    // Reconstrói a quantidade de páginas declarada pelo superbloco.
    const auto stored_page_count = decode_u64(superblock.bytes(), page_count_offset);
    // Valida zero e overflow do produto contagem × tamanho de página.
    if (stored_page_count == 0 ||
        stored_page_count > std::numeric_limits<std::uintmax_t>::max() / page_size) {
        return std::unexpected(
            Error{ErrorCode::corrupt_file, "stored page count is invalid"});
    }
    // A contagem lógica nunca pode exigir mais bytes do que o arquivo possui.
    // Um arquivo maior é tolerado: um crash entre estender o arquivo em
    // allocate_page e gravar o superbloco deixa páginas físicas órfãs além da
    // contagem, que são ignoradas aqui e reaproveitadas na próxima alocação.
    if (stored_page_count * page_size > file_size) {
        return std::unexpected(Error{ErrorCode::corrupt_file,
                                     "stored page count is larger than the database file"});
    }
    // A quantidade declarada cabe no arquivo.
    return stored_page_count;
}

} // namespace

// Cria um arquivo moDb novo sem sobrescrever arquivos existentes.
Result<PageFile> PageFile::create(const std::filesystem::path& path) {
    // Recebe possíveis erros do filesystem sem lançar exceção.
    std::error_code filesystem_error;
    // Protege um arquivo que já existe no caminho solicitado.
    if (std::filesystem::exists(path, filesystem_error)) {
        return std::unexpected(
            Error{ErrorCode::file_already_exists, "database file already exists: " + path.string()});
    }
    // Trata uma falha ocorrida durante a consulta do caminho.
    if (filesystem_error) {
        return std::unexpected(io_error("could not inspect database path: " +
                                        filesystem_error.message()));
    }

    // Limita o fluxo de criação a este bloco para fechá-lo antes da reabertura.
    {
        // Abre um arquivo binário novo para escrita.
        std::ofstream output{path, std::ios::binary | std::ios::out | std::ios::trunc};
        // Confere se o sistema operacional abriu o arquivo.
        if (!output) {
            return std::unexpected(io_error("could not create database file: " + path.string()));
        }
        // Um banco novo começa com um único superbloco e sem raiz de catálogo.
        const auto superblock = make_superblock(1, std::nullopt);
        // Escreve todos os bytes configurados da página zero.
        output.write(reinterpret_cast<const char*>(superblock.bytes().data()),
                     static_cast<std::streamsize>(page_size));
        // Envia ao sistema operacional os bytes mantidos no buffer do fluxo.
        output.flush();
        // Verifica se a escrita ou o flush falhou.
        if (!output) {
            return std::unexpected(io_error("could not initialize database file: " + path.string()));
        }
    }

    // Reabre o arquivo permitindo leituras e escritas futuras.
    std::fstream stream{path, std::ios::binary | std::ios::in | std::ios::out};
    // Confere se a reabertura funcionou.
    if (!stream) {
        return std::unexpected(io_error("could not reopen database file: " + path.string()));
    }
    // Transfere o fluxo aberto para o PageFile retornado.
    return PageFile{path, std::move(stream), 1};
}

// Abre um arquivo existente somente depois de validar seu formato.
Result<PageFile> PageFile::open(const std::filesystem::path& path) {
    // Recebe erros do filesystem sem lançar exceção.
    std::error_code filesystem_error;
    // Retorna um erro específico quando o caminho não existe.
    if (!std::filesystem::exists(path, filesystem_error)) {
        return std::unexpected(
            Error{ErrorCode::file_not_found, "database file not found: " + path.string()});
    }
    // Trata uma falha ao consultar a existência do caminho.
    if (filesystem_error) {
        return std::unexpected(io_error("could not inspect database path: " +
                                        filesystem_error.message()));
    }

    // Consulta o tamanho físico do arquivo.
    const auto size = std::filesystem::file_size(path, filesystem_error);
    // Trata uma falha ao obter o tamanho.
    if (filesystem_error) {
        return std::unexpected(io_error("could not read database file size: " +
                                        filesystem_error.message()));
    }
    // Um arquivo válido possui pelo menos uma página e não possui página parcial.
    if (size < page_size || size % page_size != 0) {
        return std::unexpected(
            Error{ErrorCode::corrupt_file, "database file is truncated or misaligned"});
    }

    // Abre o arquivo em modo binário para leitura e escrita.
    std::fstream stream{path, std::ios::binary | std::ios::in | std::ios::out};
    // Confere se o sistema operacional permitiu a abertura.
    if (!stream) {
        return std::unexpected(io_error("could not open database file: " + path.string()));
    }

    // Prepara o objeto que receberá a primeira página completa.
    Page superblock;
    // Lê a página zero completa.
    if (auto result = read_exact(stream, superblock); !result) {
        return std::unexpected(result.error());
    }
    // Valida assinatura, versão, tamanho e contagem de páginas.
    auto page_count = validate_superblock(superblock, size);
    // Propaga qualquer inconsistência encontrada.
    if (!page_count) {
        return std::unexpected(page_count.error());
    }
    // Recupera a raiz do catálogo persistida para mantê-la entre reescritas.
    const auto catalog_root = decode_catalog_root(superblock);
    // Retorna o arquivo aberto com a contagem validada e a raiz preservada.
    return PageFile{path, std::move(stream), *page_count, catalog_root};
}

// Acrescenta uma página zerada ao final do arquivo.
Result<PageId> PageFile::allocate_page() {
    // Impede que a soma da quantidade de páginas ultrapasse uint64_t.
    if (page_count_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(io_error("database reached the maximum page count"));
    }

    // O próximo identificador é igual à quantidade atual de páginas.
    const PageId id{page_count_};
    // Escreve uma nova página preenchida com zeros.
    if (auto result = write_at(id, Page{}); !result) {
        return std::unexpected(result.error());
    }
    // Calcula a nova quantidade após a alocação.
    const auto new_page_count = page_count_ + 1;
    // Atualiza apenas os 8 bytes de page_count, sem reescrever o superbloco inteiro
    // nem tocar em magic/versão/tamanho-de-página/catalog_root.
    if (auto result = write_page_count(new_page_count); !result) {
        return std::unexpected(result.error());
    }
    // Atualiza a cópia mantida em memória somente após as escritas funcionarem.
    page_count_ = new_page_count;
    // Devolve o identificador reservado para o chamador.
    return id;
}

// Lê uma página existente pelo seu identificador.
Result<Page> PageFile::read(PageId id) {
    // A API por valor mantém conveniência para chamadores sem buffer reutilizável.
    Page page;
    if (auto result = read(id, page); !result) {
        return std::unexpected(result.error());
    }
    return page;
}

// Lê uma página diretamente no armazenamento fornecido pelo chamador.
Result<void> PageFile::read(PageId id, Page& destination) {
    // Rejeita identificadores fora da quantidade conhecida.
    if (id.value >= page_count_) {
        return std::unexpected(
            Error{ErrorCode::page_not_found, "page does not exist: " + std::to_string(id.value)});
    }

    // Limpa flags deixadas por operações anteriores no fluxo.
    stream_.clear();
    // Move a posição de leitura para o início da página solicitada.
    stream_.seekg(static_cast<std::streamoff>(id.value * page_size), std::ios::beg);
    // Confere se o deslocamento foi aceito.
    if (!stream_) {
        return std::unexpected(io_error("could not seek to page: " + std::to_string(id.value)));
    }

    // Sobrescreve todos os bytes do buffer reutilizável.
    if (auto result = read_exact(stream_, destination); !result) {
        return std::unexpected(result.error());
    }
    return {};
}

// Sobrescreve uma página de dados existente.
Result<void> PageFile::write(PageId id, const Page& page) {
    // Protege os metadados do banco contra alterações externas.
    if (id == superblock_page_id) {
        return std::unexpected(
            Error{ErrorCode::reserved_page, "the superblock cannot be overwritten directly"});
    }
    // Rejeita uma página que ainda não foi alocada.
    if (id.value >= page_count_) {
        return std::unexpected(
            Error{ErrorCode::page_not_found, "page does not exist: " + std::to_string(id.value)});
    }
    // Usa a rotina interna depois de concluir todas as validações públicas.
    return write_at(id, page);
}

// Envia ao sistema operacional todas as escritas pendentes no fluxo.
Result<void> PageFile::flush() {
    // Esvazia o buffer mantido por std::fstream.
    stream_.flush();
    // O estado do fluxo informa se o flush falhou.
    if (!stream_) {
        return std::unexpected(io_error("could not flush database file: " + path_.string()));
    }
    // O fluxo não informou erro.
    return {};
}

// Reconstrói e grava a página zero com uma nova contagem, mantendo os demais campos.
Result<void> PageFile::write_superblock(std::uint64_t page_count) {
    return write_at(superblock_page_id, make_superblock(page_count, catalog_root_));
}

// Atualiza somente os 8 bytes de page_count no superbloco, sem reescrever a página.
Result<void> PageFile::write_page_count(std::uint64_t page_count) {
    // Codifica a contagem em little-endian, no mesmo layout de make_superblock.
    std::array<std::byte, sizeof(std::uint64_t)> buffer{};
    for (std::size_t index = 0; index < sizeof(page_count); ++index) {
        buffer[index] = std::byte{static_cast<unsigned char>(page_count & 0xffU)};
        page_count >>= 8U;
    }
    // Limpa flags de operações anteriores no fluxo.
    stream_.clear();
    // Posiciona a escrita no campo page_count dentro do superbloco.
    stream_.seekp(static_cast<std::streamoff>(
        superblock_page_id.value * page_size + page_count_offset), std::ios::beg);
    if (!stream_) {
        return std::unexpected(io_error("could not seek to the superblock page count"));
    }
    // Escreve apenas os oito bytes do campo.
    stream_.write(reinterpret_cast<const char*>(buffer.data()),
                  static_cast<std::streamsize>(buffer.size()));
    if (!stream_) {
        return std::unexpected(io_error("could not update the superblock page count"));
    }
    return {};
}

// Persiste a raiz do catálogo sem apagar os demais metadados do superbloco.
Result<void> PageFile::set_catalog_root(std::optional<PageId> root) {
    // Uma raiz precisa apontar para uma página de dados existente.
    if (root) {
        // A página zero pertence ao superbloco e nunca é uma raiz de catálogo.
        if (root->value == superblock_page_id.value) {
            return std::unexpected(Error{ErrorCode::invalid_argument,
                                         "the catalog root cannot be the superblock page"});
        }
        // A página precisa ter sido alocada antes de virar raiz.
        if (root->value >= page_count_) {
            return std::unexpected(Error{
                ErrorCode::page_not_found,
                "catalog root page does not exist: " + std::to_string(root->value)});
        }
    }
    // Guarda o valor anterior para reverter se a escrita falhar.
    const auto previous = catalog_root_;
    // Atualiza o espelho em memória antes de reescrever o superbloco.
    catalog_root_ = root;
    // Persiste o superbloco preservando a contagem atual de páginas.
    if (auto result = write_superblock(page_count_); !result) {
        // Restaura o valor anterior para não divergir do que está em disco.
        catalog_root_ = previous;
        return std::unexpected(result.error());
    }
    // A raiz foi persistida com sucesso.
    return {};
}

// Executa a escrita binária sem bloquear páginas reservadas.
Result<void> PageFile::write_at(PageId id, const Page& page) {
    // Limpa flags deixadas por operações anteriores no fluxo.
    stream_.clear();
    // Move a posição de escrita para o início da página.
    stream_.seekp(static_cast<std::streamoff>(id.value * page_size), std::ios::beg);
    // Confere se o deslocamento foi aceito.
    if (!stream_) {
        return std::unexpected(io_error("could not seek to page: " + std::to_string(id.value)));
    }
    // Escreve todos os bytes da página na posição calculada.
    stream_.write(reinterpret_cast<const char*>(page.bytes().data()),
                  static_cast<std::streamsize>(page_size));
    // Verifica se a escrita completa foi aceita pelo fluxo.
    if (!stream_) {
        return std::unexpected(io_error("could not write page: " + std::to_string(id.value)));
    }
    // A operação terminou sem erro.
    return {};
}

} // namespace modb::storage
