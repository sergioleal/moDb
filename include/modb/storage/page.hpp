#pragma once

// Disponibiliza o bloco de bytes com tamanho fixo.
#include <array>
// Disponibiliza assert para a precondição de índice de operator[].
#include <cassert>
// Disponibiliza std::byte e std::size_t.
#include <cstddef>
// Disponibiliza o inteiro de 64 bits do identificador.
#include <cstdint>
// Disponibiliza uma visão segura dos bytes da página.
#include <span>

namespace modb::storage {

// Permite builds experimentais sem alterar o tamanho padrão do formato.
#ifndef MODB_PAGE_SIZE
#define MODB_PAGE_SIZE 4096
#endif

// Todas as páginas de um build possuem o tamanho gravado em seu superbloco.
inline constexpr std::size_t page_size = MODB_PAGE_SIZE;
// Os offsets da slotted page são persistidos em inteiros de dezesseis bits.
static_assert(page_size > 28 && page_size <= 65535,
              "MODB_PAGE_SIZE must be between 29 and 65535 bytes");

// Identifica a posição lógica de uma página dentro do arquivo.
struct PageId {
    // A página zero começa no início do arquivo, a página um vem depois dela etc.
    std::uint64_t value{};

    // Permite comparar identificadores diretamente.
    friend bool operator==(PageId, PageId) = default;
};

// Possui todos os bytes de uma página lidos ou escritos de uma vez.
class Page {
public:
    // Dá um nome simples ao tipo do bloco interno.
    using Data = std::array<std::byte, page_size>;

    // Cria uma página preenchida com zeros.
    Page() = default;

    // Permite alterar os bytes da página sem copiar o bloco.
    [[nodiscard]] std::span<std::byte, page_size> bytes() noexcept { return data_; }
    // Permite ler os bytes de uma página constante sem copiá-los.
    [[nodiscard]] std::span<const std::byte, page_size> bytes() const noexcept { return data_; }

    // Retorna um byte modificável; o índice válido é precondição do chamador.
    // Os offsets vêm de campos já validados por validate_page, então indexamos
    // direto (como std::span) em vez de lançar exceção fora da política de erros.
    [[nodiscard]] std::byte& operator[](std::size_t index) {
        assert(index < page_size);
        return data_[index];
    }
    // Versão somente leitura com a mesma precondição.
    [[nodiscard]] const std::byte& operator[](std::size_t index) const {
        assert(index < page_size);
        return data_[index];
    }

    // Compara todos os bytes de duas páginas.
    friend bool operator==(const Page&, const Page&) = default;

private:
    // Armazena os bytes e os inicializa com zero.
    Data data_{};
};

} // namespace modb::storage
