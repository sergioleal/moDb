#pragma once

// Importa Result para retornar erros de arquivo sem lançar exceções.
#include "modb/error.hpp"
// Importa o descritor nativo com I/O posicional e sincronização real.
#include "modb/storage/native_file.hpp"
// Importa Page e PageId.
#include "modb/storage/page.hpp"
// Importa o buffer pool (evolução do PageCache).
#include "modb/storage/buffer_pool.hpp"

// Disponibiliza os tipos inteiros usados no formato.
#include <cstddef>
#include <cstdint>
// Disponibiliza caminhos portáveis.
#include <filesystem>
// Disponibiliza a posse movível do cache.
#include <memory>
// Disponibiliza a raiz de catálogo, que pode ainda não existir.
#include <optional>
// Disponibiliza o buffer de páginas sujas da transação.
#include <unordered_map>

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
    [[nodiscard]] static Result<PageFile> create(
        const std::filesystem::path& path,
        std::size_t cache_capacity = page_cache_capacity);
    // Abre e valida um arquivo moDb existente.
    [[nodiscard]] static Result<PageFile> open(
        const std::filesystem::path& path,
        std::size_t cache_capacity = page_cache_capacity);

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

    // --- Transações (Fase 5): WAL redo-only com after-images de página ---
    // Durante uma transação, write() bufferiza a página modificada em vez de
    // tocar o disco; read() enxerga o buffer (read-your-writes). No commit as
    // páginas são aplicadas de uma vez. allocate_page continua imediato (páginas
    // não referenciadas de uma transação abortada ficam órfãs — limitação de MVP).
    void begin_transaction();
    // Descarta as páginas pendentes (rollback): nada foi aplicado ao disco.
    void discard_transaction() noexcept;
    [[nodiscard]] bool in_transaction() const noexcept { return in_transaction_; }
    // Páginas sujas acumuladas na transação corrente (lidas pelo WAL no commit).
    [[nodiscard]] const std::unordered_map<std::uint64_t, Page>& transaction_pages()
        const noexcept {
        return tx_pages_;
    }
    // Aplica as páginas sujas ao disco (após o WAL estar durável) e encerra a
    // transação. O flush é responsabilidade do chamador (após aplicar).
    [[nodiscard]] Result<void> apply_transaction();

    // Test-only (Fase 5, failpoints): faz apply_transaction falhar após aplicar
    // exatamente `pages_before_failure` páginas, simulando uma queda no meio da
    // aplicação — as primeiras ficam no disco, o resto e o WAL ficam para a
    // recuperação. O padrão (~0) aplica todas as páginas normalmente.
    void set_apply_failpoint(std::size_t pages_before_failure) noexcept {
        apply_failpoint_ = pages_before_failure;
    }

    // Recuperação: grava uma página reaplicada do WAL, estendendo o arquivo e a
    // contagem de páginas se necessário. Ignora o buffer de transação.
    [[nodiscard]] Result<void> write_recovered_page(PageId id, const Page& page);

    // Retorna a página raiz do catálogo ou vazio quando ainda não existe.
    [[nodiscard]] std::optional<PageId> catalog_root() const noexcept { return catalog_root_; }
    // Persiste a raiz do catálogo no superbloco, preservando os demais campos.
    [[nodiscard]] Result<void> set_catalog_root(std::optional<PageId> root);

    // Retorna a quantidade total de páginas, incluindo o superbloco.
    [[nodiscard]] std::uint64_t page_count() const noexcept { return page_count_; }
    // Retorna o caminho do arquivo sem copiá-lo.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    // Acesso ao buffer pool (capacidade, pin/unpin, métricas) — Fase 10B.
    [[nodiscard]] BufferPool& buffer_pool() noexcept { return *cache_; }
    [[nodiscard]] const BufferPool& buffer_pool() const noexcept { return *cache_; }

private:
    // Constrói um PageFile somente depois que create ou open validou o arquivo.
    PageFile(std::filesystem::path path, NativeFile file, std::uint64_t page_count,
             std::optional<PageId> catalog_root, std::size_t cache_capacity)
        : path_{std::move(path)},
          file_{std::move(file)},
          cache_{std::make_unique<BufferPool>(cache_capacity)},
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
    // Cache/buffer pool de páginas (LRU + pin + dirty); unique_ptr mantém o
    // PageFile movível sem exigir que o pool seja movível.
    std::unique_ptr<BufferPool> cache_;
    // Mantém em memória a quantidade validada de páginas.
    std::uint64_t page_count_{};
    // Espelha a raiz do catálogo persistida no superbloco.
    std::optional<PageId> catalog_root_;
    // Indica se há uma transação em andamento bufferizando escritas.
    bool in_transaction_{false};
    // Páginas sujas da transação corrente (PageId → conteúdo), aplicadas no commit.
    std::unordered_map<std::uint64_t, Page> tx_pages_;
    // Test-only: teto de páginas aplicadas antes de simular queda (~0 = todas).
    std::size_t apply_failpoint_{~std::size_t{0}};
};

} // namespace modb::storage
