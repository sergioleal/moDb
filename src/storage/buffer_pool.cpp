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
    entries_.erase(it->second);
    index_.erase(it);
}

Result<void> BufferPool::pin(std::uint64_t page) {
    const auto it = index_.find(page);
    if (it == index_.end()) {
        return std::unexpected(
            Error{ErrorCode::page_not_found, "cannot pin a page that is not in the buffer pool"});
    }
    ++it->second->pin_count;
    touch(it->second);
    return {};
}

void BufferPool::unpin(std::uint64_t page) {
    const auto it = index_.find(page);
    if (it == index_.end() || it->second->pin_count == 0) {
        return;
    }
    --it->second->pin_count;
}

void BufferPool::put_dirty(std::uint64_t page, const Page& contents) {
    if (const auto it = index_.find(page); it != index_.end()) {
        it->second->page = contents;
        it->second->dirty = true;
        touch(it->second);
        return;
    }
    // Dirty não é evictável; pode temporariamente exceder a capacidade.
    entries_.emplace_front(Frame{.page_id = page, .page = contents, .dirty = true});
    index_.emplace(page, entries_.begin());
}

bool BufferPool::is_dirty(std::uint64_t page) const noexcept {
    const auto it = index_.find(page);
    return it != index_.end() && it->second->dirty;
}

Result<void> BufferPool::flush_dirty(
    const std::function<Result<void>(std::uint64_t, const Page&)>& writer) {
    for (auto& frame : entries_) {
        if (!frame.dirty) {
            continue;
        }
        if (auto written = writer(frame.page_id, frame.page); !written) {
            return std::unexpected(written.error());
        }
        frame.dirty = false;
        ++metrics_.dirty_flushes;
    }
    // Após write-back, encolhe spill de dirty para a capacidade.
    evict_until(capacity_);
    return {};
}

void BufferPool::discard_dirty() noexcept {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!it->dirty) {
            ++it;
            continue;
        }
        it->dirty = false;
        if (it->pin_count == 0) {
            index_.erase(it->page_id);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    evict_until(capacity_);
}

std::size_t BufferPool::pinned_count() const noexcept {
    std::size_t count = 0;
    for (const auto& frame : entries_) {
        if (frame.pin_count > 0) {
            ++count;
        }
    }
    return count;
}

std::size_t BufferPool::dirty_count() const noexcept {
    std::size_t count = 0;
    for (const auto& frame : entries_) {
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
    entries_.splice(entries_.begin(), entries_, it);
}

bool BufferPool::can_evict(const Frame& frame) const noexcept {
    return frame.pin_count == 0 && !frame.dirty;
}

void BufferPool::evict_until(std::size_t max_size) {
    while (index_.size() > max_size) {
        bool evicted = false;
        for (auto it = entries_.end(); it != entries_.begin();) {
            --it;
            if (!can_evict(*it)) {
                continue;
            }
            index_.erase(it->page_id);
            it = entries_.erase(it);
            ++metrics_.evictions;
            evicted = true;
            break;
        }
        if (!evicted) {
            break;
        }
    }
}

} // namespace modb::storage
