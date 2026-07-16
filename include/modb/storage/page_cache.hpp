#pragma once

// Importa Page, o conteúdo guardado em cache.
#include "modb/storage/page.hpp"

// Disponibiliza std::size_t.
#include <cstddef>
// Disponibiliza inteiros de largura fixa para o número de página.
#include <cstdint>
// Disponibiliza a lista de recência (LRU).
#include <list>
// Disponibiliza o índice de acesso O(1).
#include <unordered_map>
// Disponibiliza std::pair na entrada da lista.
#include <utility>

namespace modb::storage {

// Cache LRU de páginas lidas, em memória, com capacidade fixa em páginas.
//
// Só guarda páginas limpas: as escritas do PageFile são write-through e apenas
// atualizam a cópia em cache. O cache nunca segura dados sujos, então não muda
// nenhuma garantia de durabilidade — serve só para evitar syscalls e cópias em
// releituras (o motor lê a mesma página do SO repetidamente sem ele).
class PageCache {
public:
    // Capacidade mínima de uma página; zero é elevado a um para nunca degenerar.
    explicit PageCache(std::size_t capacity_pages)
        : capacity_{capacity_pages == 0 ? 1 : capacity_pages} {}

    // Retorna a página em cache, promovendo-a a mais-recente, ou nullptr no miss.
    [[nodiscard]] const Page* get(std::uint64_t page);
    // Insere ou atualiza o conteúdo de uma página (cópia), removendo a menos
    // recente quando a capacidade é excedida.
    void put(std::uint64_t page, const Page& contents);
    // Remove uma página do cache, se presente (usado quando o disco muda por um
    // caminho que não repõe o conteúdo inteiro, como a escrita parcial).
    void invalidate(std::uint64_t page);

    // Quantas páginas estão em cache no momento (útil para testes).
    [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    // Cada entrada guarda o número da página e uma cópia dos seus bytes.
    using Entry = std::pair<std::uint64_t, Page>;

    // Capacidade máxima em páginas.
    std::size_t capacity_;
    // Frente = mais recente; fim = menos recente (candidata a remoção).
    std::list<Entry> entries_;
    // Localiza a entrada de cada página em tempo constante.
    std::unordered_map<std::uint64_t, std::list<Entry>::iterator> index_;
};

} // namespace modb::storage
