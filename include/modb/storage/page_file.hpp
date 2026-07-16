#pragma once

// Importa Result para retornar erros de arquivo sem lançar exceções.
#include "modb/error.hpp"
// Importa Page e PageId.
#include "modb/storage/page.hpp"

// Disponibiliza os tipos inteiros usados no formato.
#include <cstdint>
// Disponibiliza caminhos portáveis.
#include <filesystem>
// Disponibiliza o fluxo que lê e escreve o mesmo arquivo.
#include <fstream>
// Disponibiliza a raiz de catálogo, que pode ainda não existir.
#include <optional>

namespace modb::storage {

// Define a versão binária que esta implementação consegue abrir.
inline constexpr std::uint16_t current_format_version = 1;
// Reserva a página zero para os metadados gerais do banco.
inline constexpr PageId superblock_page_id{0};

// Gerencia um arquivo dividido em páginas de tamanho fixo.
class PageFile {
public:
    // Impede a cópia porque dois objetos não devem possuir o mesmo fluxo.
    PageFile(const PageFile&) = delete;
    // Impede a atribuição por cópia pelo mesmo motivo.
    PageFile& operator=(const PageFile&) = delete;
    // Permite transferir a propriedade do arquivo para outro objeto.
    PageFile(PageFile&&) noexcept = default;
    // Permite atribuir por movimento.
    PageFile& operator=(PageFile&&) noexcept = default;
    // Fecha o fluxo automaticamente quando o objeto é destruído.
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
    // Entrega ao sistema operacional todas as escritas pendentes do fluxo.
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
    PageFile(std::filesystem::path path, std::fstream stream, std::uint64_t page_count,
             std::optional<PageId> catalog_root = std::nullopt)
        : path_{std::move(path)},
          stream_{std::move(stream)},
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
    // Mantém o arquivo aberto para leitura e escrita binária.
    std::fstream stream_;
    // Mantém em memória a quantidade validada de páginas.
    std::uint64_t page_count_{};
    // Espelha a raiz do catálogo persistida no superbloco.
    std::optional<PageId> catalog_root_;
};

} // namespace modb::storage
