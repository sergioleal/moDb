#pragma once

// Importa Result para retornar erros de arquivo sem lançar exceções.
#include "modb/error.hpp"
// Importa o descritor nativo com I/O posicional e sincronização real.
#include "modb/storage/native_file.hpp"
// Importa Page e PageId.
#include "modb/storage/page.hpp"
// Importa o cache de leitura de páginas.
#include "modb/storage/page_cache.hpp"

// Disponibiliza os tipos inteiros usados no formato.
#include <cstddef>
#include <cstdint>
// Disponibiliza caminhos portáveis.
#include <filesystem>
// Disponibiliza a posse movível do cache.
#include <memory>
// Disponibiliza a raiz de catálogo, que pode ainda não existir.
#include <optional>

namespace modb::storage {

// Define a versão binária que esta implementação consegue abrir.
inline constexpr std::uint16_t current_format_version = 1;
// Reserva a página zero para os metadados gerais do banco.
inline constexpr PageId superblock_page_id{0};

// Quantas páginas contíguas uma leitura de miss traz de uma vez (a pedida + as
// seguintes), numa única leitura maior — read-ahead para varreduras sequenciais.
inline constexpr std::size_t page_readahead = 4;
// Capacidade do cache de leitura, em páginas (padrão ~4 MiB com página de 4 KiB).
inline constexpr std::size_t page_cache_capacity = 1024;

// Gerencia um arquivo dividido em páginas de tamanho fixo.
class PageFile {
public:
    // Impede a cópia porque dois objetos não devem possuir o mesmo descritor.
    PageFile(const PageFile&) = delete;
    // Impede a atribuição por cópia pelo mesmo motivo.
    PageFile& operator=(const PageFile&) = delete;
    // Permite transferir a propriedade do arquivo para outro objeto.
    PageFile(PageFile&&) noexcept = default;
    // Permite atribuir por movimento.
    PageFile& operator=(PageFile&&) noexcept = default;
    // Fecha o descritor automaticamente quando o objeto é destruído.
    ~PageFile() = default;

    // Cria um arquivo novo contendo somente o superbloco.
    [[nodiscard]] static Result<PageFile> create(const std::filesystem::path& path);
    // Abre e valida um arquivo moDb existente.
    [[nodiscard]] static Result<PageFile> open(const std::filesystem::path& path);

    // Acrescenta uma página zerada e retorna seu identificador.
    [[nodiscard]] Result<PageId> allocate_page();
    // Lê uma página existente.
    [[nodiscard]] Result<Page> read(PageId id);
    // Lê diretamente sobre um buffer Page fornecido pelo chamador.
    [[nodiscard]] Result<void> read(PageId id, Page& destination);
    // Sobrescreve uma página de dados existente.
    [[nodiscard]] Result<void> write(PageId id, const Page& page);
    // Persiste no dispositivo todas as escritas já aceitas (durabilidade real).
    // Depois de um flush bem-sucedido, os dados sobrevivem a uma queda de energia.
    [[nodiscard]] Result<void> flush();

    // Retorna a página raiz do catálogo ou vazio quando ainda não existe.
    [[nodiscard]] std::optional<PageId> catalog_root() const noexcept { return catalog_root_; }
    // Persiste a raiz do catálogo no superbloco, preservando os demais campos.
    [[nodiscard]] Result<void> set_catalog_root(std::optional<PageId> root);

    // Retorna a quantidade total de páginas, incluindo o superbloco.
    [[nodiscard]] std::uint64_t page_count() const noexcept { return page_count_; }
    // Retorna o caminho do arquivo sem copiá-lo.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    // Constrói um PageFile somente depois que create ou open validou o arquivo.
    PageFile(std::filesystem::path path, NativeFile file, std::uint64_t page_count,
             std::optional<PageId> catalog_root = std::nullopt)
        : path_{std::move(path)},
          file_{std::move(file)},
          cache_{std::make_unique<PageCache>(page_cache_capacity)},
          page_count_{page_count},
          catalog_root_{catalog_root} {}

    // Atualiza internamente os metadados da página zero.
    [[nodiscard]] Result<void> write_superblock(std::uint64_t page_count);
    // Atualiza apenas os 8 bytes de page_count no superbloco (caminho de alocação).
    [[nodiscard]] Result<void> write_page_count(std::uint64_t page_count);
    // Escreve qualquer página, inclusive páginas reservadas, para uso interno.
    [[nodiscard]] Result<void> write_at(PageId id, const Page& page);

    // Guarda o caminho para mensagens e consultas.
    std::filesystem::path path_;
    // Mantém o arquivo aberto com I/O posicional e sincronização ao dispositivo.
    NativeFile file_;
    // Cache de leitura (write-through); unique_ptr mantém o PageFile movível
    // sem exigir que o cache seja movível.
    std::unique_ptr<PageCache> cache_;
    // Mantém em memória a quantidade validada de páginas.
    std::uint64_t page_count_{};
    // Espelha a raiz do catálogo persistida no superbloco.
    std::optional<PageId> catalog_root_;
};

} // namespace modb::storage
