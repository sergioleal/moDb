#pragma once

// B+ tree persistente (Fase 7B). Chaves são bytes já codificados de forma
// ordenável (ver key_codec.hpp) mais um ObjectId de desempate, então chaves de
// valor duplicado são permitidas e ordenadas de forma estável. Valor = ObjectId.
// Páginas BTLF (folha) e BTIN (interna) com header de 32 bytes; folhas
// encadeadas por `next_leaf` para varredura por faixa. Split de folha e interna
// propaga até a raiz, que pode crescer em altura. A raiz é devolvida por
// `root_page()` para ser persistida no catálogo do índice.

// Importa Result e códigos de erro.
#include "modb/error.hpp"
// Importa PageFile e PageId.
#include "modb/storage/page_file.hpp"

// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza inteiros de largura fixa.
#include <cstdint>
// Disponibiliza a visão das chaves recebidas.
#include <span>
// Disponibiliza os vetores de resultado.
#include <vector>

namespace modb::index {

class BTree {
public:
    // Cria uma árvore vazia (uma folha raiz) e devolve o handle.
    [[nodiscard]] static Result<BTree> create(storage::PageFile& file);
    // Reabre uma árvore a partir da página raiz persistida.
    [[nodiscard]] static Result<BTree> open(storage::PageFile& file, storage::PageId root);

    // Página raiz atual — muda quando a raiz sofre split; o dono deve persistir.
    [[nodiscard]] storage::PageId root_page() const noexcept { return root_; }

    // Insere a chave composta (bytes de valor + ObjectId). Duplicatas de valor
    // são permitidas; a mesma chave composta exata é rejeitada.
    [[nodiscard]] Result<void> insert(std::span<const std::byte> key, std::uint64_t object_id);
    // Remove a chave composta exata; ausente é `record_not_found`.
    [[nodiscard]] Result<void> remove(std::span<const std::byte> key, std::uint64_t object_id);
    // Todos os ObjectIds cujo valor de chave é exatamente `key` (igualdade),
    // em ordem crescente de ObjectId.
    [[nodiscard]] Result<std::vector<std::uint64_t>> find(std::span<const std::byte> key) const;
    // Todos os ObjectIds cujo valor de chave está em [lo, hi] inclusive, na
    // ordem da árvore (valor crescente e, para valores iguais, id crescente).
    [[nodiscard]] Result<std::vector<std::uint64_t>> range(std::span<const std::byte> lo,
                                                           std::span<const std::byte> hi) const;

    // Verificação estrutural para testes: profundidade uniforme das folhas,
    // ordenação interna e (nas não-raiz) preenchimento mínimo. Devolve a altura.
    [[nodiscard]] Result<std::uint32_t> validate() const;

private:
    BTree(storage::PageFile& file, storage::PageId root) noexcept : file_{&file}, root_{root} {}

    storage::PageFile* file_;
    storage::PageId root_;
};

} // namespace modb::index
