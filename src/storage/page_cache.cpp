// Importa a interface do cache de páginas.
#include "modb/storage/page_cache.hpp"

namespace modb::storage {

const Page* PageCache::get(std::uint64_t page) {
    const auto it = index_.find(page);
    if (it == index_.end()) {
        return nullptr;
    }
    // Move a entrada para a frente (mais recente) sem copiar o conteúdo.
    entries_.splice(entries_.begin(), entries_, it->second);
    return &it->second->second;
}

void PageCache::put(std::uint64_t page, const Page& contents) {
    if (const auto it = index_.find(page); it != index_.end()) {
        // Já presente: atualiza o conteúdo e promove a mais recente.
        it->second->second = contents;
        entries_.splice(entries_.begin(), entries_, it->second);
        return;
    }
    // Nova: insere na frente e indexa.
    entries_.emplace_front(page, contents);
    index_.emplace(page, entries_.begin());
    // Remove a menos recente enquanto exceder a capacidade.
    while (index_.size() > capacity_) {
        index_.erase(entries_.back().first);
        entries_.pop_back();
    }
}

void PageCache::invalidate(std::uint64_t page) {
    const auto it = index_.find(page);
    if (it == index_.end()) {
        return;
    }
    entries_.erase(it->second);
    index_.erase(it);
}

} // namespace modb::storage
