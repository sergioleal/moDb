// Importa PageFile, Page e PageId.
#include "modb/storage/page_file.hpp"
// Importa store_le/load_le, a implementação única de little-endian.
#include "modb/storage/endian.hpp"

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
    // Delega à implementação única de little-endian a partir do offset dado.
    store_le(target.subspan(offset, sizeof(value)), value);
}

// Codifica um inteiro de 32 bits em ordem little-endian.
void encode_u32(std::span<std::byte, page_size> target, std::size_t offset,
                std::uint32_t value) {
    // Delega à implementação única de little-endian a partir do offset dado.
    store_le(target.subspan(offset, sizeof(value)), value);
}

// Codifica um inteiro de 64 bits em ordem little-endian.
void encode_u64(std::span<std::byte, page_size> target, std::size_t offset,
                std::uint64_t value) {
    // Delega à implementação única de little-endian a partir do offset dado.
    store_le(target.subspan(offset, sizeof(value)), value);
}

// Reconstrói um inteiro de 16 bits armazenado em little-endian.
std::uint16_t decode_u16(std::span<const std::byte, page_size> source, std::size_t offset) {
    // Delega à implementação única de little-endian a partir do offset dado.
    return load_le<std::uint16_t>(source.subspan(offset, sizeof(std::uint16_t)));
}

// Reconstrói um inteiro de 32 bits armazenado em little-endian.
std::uint32_t decode_u32(std::span<const std::byte, page_size> source, std::size_t offset) {
    // Delega à implementação única de little-endian a partir do offset dado.
    return load_le<std::uint32_t>(source.subspan(offset, sizeof(std::uint32_t)));
}

// Reconstrói um inteiro de 64 bits armazenado em little-endian.
std::uint64_t decode_u64(std::span<const std::byte, page_size> source, std::size_t offset) {
    // Delega à implementação única de little-endian a partir do offset dado.
    return load_le<std::uint64_t>(source.subspan(offset, sizeof(std::uint64_t)));
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

// Calcula o offset em bytes onde a página id começa no arquivo.
std::uint64_t page_offset(PageId id) {
    return id.value * static_cast<std::uint64_t>(page_size);
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

    // Cria o arquivo com um descritor de leitura e escrita já pronto para uso.
    auto file = NativeFile::open(path, NativeFile::Mode::create_new);
    if (!file) {
        return std::unexpected(file.error());
    }
    // Um banco novo começa com um único superbloco e sem raiz de catálogo.
    const auto superblock = make_superblock(1, std::nullopt);
    // Grava a página zero no início do arquivo.
    if (auto written = file->write_at(page_offset(superblock_page_id), superblock.bytes());
        !written) {
        return std::unexpected(written.error());
    }
    // Garante que o superbloco recém-criado sobrevive a uma queda de energia.
    if (auto synced = file->sync(); !synced) {
        return std::unexpected(synced.error());
    }
    // Transfere o descritor aberto para o PageFile retornado.
    return PageFile{path, std::move(*file), 1};
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

    // Abre o arquivo existente para leitura e escrita.
    auto file = NativeFile::open(path, NativeFile::Mode::open_existing);
    if (!file) {
        return std::unexpected(file.error());
    }

    // Prepara o objeto que receberá a primeira página completa.
    Page superblock;
    // Lê a página zero completa a partir do início do arquivo.
    if (auto result = file->read_at(page_offset(superblock_page_id), superblock.bytes());
        !result) {
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
    return PageFile{path, std::move(*file), *page_count, catalog_root};
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

    // Acerto de cache: copia a página já residente e evita a syscall.
    if (const Page* cached = cache_->get(id.value)) {
        std::copy(cached->bytes().begin(), cached->bytes().end(), destination.bytes().begin());
        return {};
    }

    // Erro de cache: traz a página pedida e as seguintes numa única leitura
    // maior (read-ahead), limitada ao fim lógico do arquivo. Favorece varreduras
    // sequenciais e o acesso repetido à mesma página (ex.: IDMP compartilhada).
    const std::uint64_t first = id.value;
    const std::uint64_t available = page_count_ - first;
    const std::size_t count =
        static_cast<std::size_t>(available < page_readahead ? available : page_readahead);
    // Buffer contíguo para a leitura única; só a parte preenchida é usada.
    std::array<std::byte, page_readahead * page_size> buffer;
    const std::span<std::byte> block{buffer.data(), count * page_size};
    if (auto read = file_.read_at(page_offset(id), block); !read) {
        return std::unexpected(read.error());
    }

    // Copia a página pedida para o destino e popula o cache com todas as trazidas.
    const auto requested = block.subspan(0, page_size);
    std::copy(requested.begin(), requested.end(), destination.bytes().begin());
    for (std::size_t index = 0; index < count; ++index) {
        Page page;
        const auto slice = block.subspan(index * page_size, page_size);
        std::copy(slice.begin(), slice.end(), page.bytes().begin());
        cache_->put(first + index, page);
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

// Persiste no dispositivo todas as escritas já aceitas (durabilidade real).
Result<void> PageFile::flush() {
    // Delega ao descritor nativo, que chama FlushFileBuffers/fsync.
    return file_.sync();
}

// Reconstrói e grava a página zero com uma nova contagem, mantendo os demais campos.
Result<void> PageFile::write_superblock(std::uint64_t page_count) {
    return write_at(superblock_page_id, make_superblock(page_count, catalog_root_));
}

// Atualiza somente os 8 bytes de page_count no superbloco, sem reescrever a página.
Result<void> PageFile::write_page_count(std::uint64_t page_count) {
    // Codifica a contagem em little-endian, no mesmo layout de make_superblock.
    std::array<std::byte, sizeof(std::uint64_t)> buffer{};
    store_le(std::span<std::byte>{buffer}, page_count);
    // Grava apenas os oito bytes do campo, na sua posição dentro do superbloco.
    if (auto written =
            file_.write_at(page_offset(superblock_page_id) + page_count_offset, buffer);
        !written) {
        return std::unexpected(written.error());
    }
    // A escrita parcial alterou 8 bytes da página zero; a cópia em cache (se
    // houver) ficou desatualizada, então é descartada.
    cache_->invalidate(superblock_page_id.value);
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
    // Grava a página inteira na sua posição, sem cursor compartilhado.
    if (auto written = file_.write_at(page_offset(id), page.bytes()); !written) {
        return std::unexpected(written.error());
    }
    // Write-through: a cópia em cache passa a refletir exatamente o disco, sem
    // nunca segurar dados sujos (a durabilidade continua a mesma).
    cache_->put(id.value, page);
    return {};
}

} // namespace modb::storage
