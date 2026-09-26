#include "modb/storage/buffer_pool.hpp"

namespace modb::storage {

BufferPool::BufferPool(std::size_t capacity_pages)
    : capacity_{capacity_pages == 0 ? 1 : capacity_pages} {}

const Page* BufferPool::get(std::uint64_t page) {
    const auto it = index_.find(page);
    if (it == index_.end()) {
        ++metrics_.misses;
        return nullptr;
    }
    ++metrics_.hits;
    touch(it->second);
    return &it->second->page;
}

void BufferPool::put(std::uint64_t page, const Page& contents) {
    if (const auto it = index_.find(page); it != index_.end()) {
        it->second->page = contents;
        // put limpo sobre dirty limpa o bit: o conteúdo agora bate com o disco
        // (write-through) ou substitui a versão suja pela limpa fornecida.
        it->second->dirty = false;
        place(it->second);
        touch(it->second);
        return;
    }
    evict_until(capacity_ - 1);
    entries_.emplace_front(Frame{.page_id = page, .page = contents});
    index_.emplace(page, entries_.begin());
    // Se ainda exceder (só dirty/pinned no pool), aceita overflow temporário.
}

void BufferPool::invalidate(std::uint64_t page) {
    const auto it = index_.find(page);
    if (it == index_.end()) {
        return;
    }
    // Não remove pinada: o titular ainda referencia o frame.
    if (it->second->pin_count > 0) {
        return;
    }
    (it->second->held ? held_ : entries_).erase(it->second);
    index_.erase(it);
}

Result<void> BufferPool::pin(std::uint64_t page) {
    const auto it = index_.find(page);
    if (it == index_.end()) {
        return std::unexpected(
            Error{ErrorCode::page_not_found, "cannot pin a page that is not in the buffer pool"});
    }
    ++it->second->pin_count;
    place(it->second);
    touch(it->second);
    return {};
}

void BufferPool::unpin(std::uint64_t page) {
    const auto it = index_.find(page);
    if (it == index_.end() || it->second->pin_count == 0) {
        return;
    }
    --it->second->pin_count;
    place(it->second);
}

void BufferPool::put_dirty(std::uint64_t page, const Page& contents) {
    if (const auto it = index_.find(page); it != index_.end()) {
        it->second->page = contents;
        it->second->dirty = true;
        place(it->second);
        touch(it->second);
        return;
    }
    // Dirty não é evictável; pode temporariamente exceder a capacidade.
    held_.emplace_front(Frame{.page_id = page, .page = contents, .dirty = true, .held = true});
    index_.emplace(page, held_.begin());
}

bool BufferPool::is_dirty(std::uint64_t page) const noexcept {
    const auto it = index_.find(page);
    return it != index_.end() && it->second->dirty;
}

Result<void> BufferPool::flush_dirty(
    const std::function<Result<void>(std::uint64_t, const Page&)>& writer) {
    // Só `held_` pode ter frames sujos. `place` pode mover o frame para
    // `entries_`, então o próximo é guardado antes.
    for (auto it = held_.begin(); it != held_.end();) {
        const auto current = it++;
        if (!current->dirty) {
            continue;
        }
        if (auto written = writer(current->page_id, current->page); !written) {
            return std::unexpected(written.error());
        }
        current->dirty = false;
        ++metrics_.dirty_flushes;
        place(current);
    }
    // Após write-back, encolhe spill de dirty para a capacidade.
    evict_until(capacity_);
    return {};
}

void BufferPool::discard_dirty() noexcept {
    for (auto it = held_.begin(); it != held_.end();) {
        if (!it->dirty) {
            ++it;
            continue;
        }
        it->dirty = false;
        if (it->pin_count == 0) {
            index_.erase(it->page_id);
            it = held_.erase(it);
        } else {
            ++it;
        }
    }
    evict_until(capacity_);
}

std::size_t BufferPool::pinned_count() const noexcept {
    std::size_t count = 0;
    for (const auto& frame : held_) {
        if (frame.pin_count > 0) {
            ++count;
        }
    }
    return count;
}

std::size_t BufferPool::dirty_count() const noexcept {
    std::size_t count = 0;
    for (const auto& frame : held_) {
        if (frame.dirty) {
            ++count;
        }
    }
    return count;
}

void BufferPool::reset_metrics() noexcept {
    metrics_ = Metrics{};
}

BufferPool::Metrics BufferPool::metrics() const noexcept {
    Metrics copy = metrics_;
    copy.pinned = pinned_count();
    return copy;
}

void BufferPool::touch(List::iterator it) {
    auto& list = it->held ? held_ : entries_;
    list.splice(list.begin(), list, it);
}

void BufferPool::place(List::iterator it) {
    const bool should_hold = !can_evict(*it);
    if (should_hold == it->held) {
        return;
    }
    // splice move o nó sem invalidar o iterador guardado em `index_`.
    if (should_hold) {
        held_.splice(held_.begin(), entries_, it);
    } else {
        entries_.splice(entries_.begin(), held_, it);
    }
    it->held = should_hold;
}

bool BufferPool::can_evict(const Frame& frame) const noexcept {
    return frame.pin_count == 0 && !frame.dirty;
}

void BufferPool::evict_until(std::size_t max_size) {
    // `entries_` só tem evictáveis: a vítima é sempre a cauda (menos recente).
    while (index_.size() > max_size && !entries_.empty()) {
        index_.erase(entries_.back().page_id);
        entries_.pop_back();
        ++metrics_.evictions;
    }
}

} // namespace modb::storage
