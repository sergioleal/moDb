#pragma once

#include "modb/error.hpp"
#include "modb/storage/page.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <utility>

namespace modb::storage {

// Buffer pool LRU com capacidade configurável, pin/unpin, páginas sujas e
// métricas. Páginas pinadas nunca são evictadas. Páginas dirty só saem do pool
// via write-back explícito (após o WAL estar durável) ou discard (rollback).
class BufferPool {
public:
    struct Metrics {
        std::uint64_t hits{0};
        std::uint64_t misses{0};
        std::uint64_t evictions{0};
        std::uint64_t dirty_flushes{0};
        std::uint64_t pinned{0};
    };

    explicit BufferPool(std::size_t capacity_pages);

    // --- API de cache limpo (compatível com o PageCache da Fase 5) ---
    [[nodiscard]] const Page* get(std::uint64_t page);
    void put(std::uint64_t page, const Page& contents);
    void invalidate(std::uint64_t page);

    // --- Pin / unpin ---
    // Incrementa o pin de uma página já residente; miss → page_not_found.
    [[nodiscard]] Result<void> pin(std::uint64_t page);
    // Decrementa o pin; no-op seguro se ausente ou já em zero.
    void unpin(std::uint64_t page);

    // --- Dirty / write-back ---
    // Insere ou atualiza como dirty (não evictável até flush/discard).
    void put_dirty(std::uint64_t page, const Page& contents);
    [[nodiscard]] bool is_dirty(std::uint64_t page) const noexcept;
    // Escritor chamado para cada página dirty (ordem de inserção aproximada).
    // Após sucesso de todas, limpa dirty e conta dirty_flushes.
    [[nodiscard]] Result<void> flush_dirty(
        const std::function<Result<void>(std::uint64_t page, const Page& contents)>& writer);
    // Descarta todas as páginas dirty (rollback); remove frames dirty sem pin.
    void discard_dirty() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t pinned_count() const noexcept;
    [[nodiscard]] std::size_t dirty_count() const noexcept;
    [[nodiscard]] Metrics metrics() const noexcept;
    void reset_metrics() noexcept;
    // Contabiliza write-backs feitos pelo PageFile após o WAL (apply).
    void record_dirty_flushes(std::uint64_t count) noexcept { metrics_.dirty_flushes += count; }

private:
    struct Frame {
        std::uint64_t page_id{};
        Page page{};
        std::uint32_t pin_count{0};
        bool dirty{false};
        // Em qual lista o frame está: `held_` (dirty ou pinado) ou `entries_`.
        bool held{false};
    };

    using Entry = Frame;
    using List = std::list<Entry>;

    void touch(List::iterator it);
    // Recoloca o frame na lista que corresponde ao estado atual dele (O(1),
    // por splice). Chamado depois de toda mudança de dirty/pin.
    void place(List::iterator it);
    // Remove vítimas limpas e sem pin até caber `max_size` (ou não houver).
    void evict_until(std::size_t max_size);
    [[nodiscard]] bool can_evict(const Frame& frame) const noexcept;

    std::size_t capacity_;
    // Duas listas (T33.3): `entries_` guarda só frames evictáveis, em ordem
    // LRU; `held_`, os dirty ou pinados, que não podem sair. Antes era uma lista
    // só, e `evict_until` a percorria pulando os não evictáveis -- com mais
    // frames sujos do que a capacidade, cada inserção no cache varria todos
    // eles: O(páginas sujas) por operação, O(n²) numa transação grande
    // (collect_garbage a 100k levava 30-64 s). Agora despejar é tirar do fim de
    // `entries_`.
    List entries_;
    List held_;
    std::unordered_map<std::uint64_t, List::iterator> index_;
    Metrics metrics_{};
};

// Nome histórico da Fase 5: o embrião evoluiu para BufferPool.
using PageCache = BufferPool;

} // namespace modb::storage
