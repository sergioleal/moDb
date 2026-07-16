#include "modb/storage/scratch_page_pool.hpp"

#include <stdexcept>
#include <utility>

namespace modb::storage {

ScratchPagePool::Handle::Handle(ScratchPagePool* pool, std::size_t index, Page* page) noexcept
    : pool_{pool}, index_{index}, page_{page} {}

ScratchPagePool::Handle::Handle(Handle&& other) noexcept
    : pool_{std::exchange(other.pool_, nullptr)},
      index_{other.index_},
      page_{std::exchange(other.page_, nullptr)} {}

ScratchPagePool::Handle& ScratchPagePool::Handle::operator=(Handle&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = std::exchange(other.pool_, nullptr);
        index_ = other.index_;
        page_ = std::exchange(other.page_, nullptr);
    }
    return *this;
}

ScratchPagePool::Handle::~Handle() {
    release();
}

void ScratchPagePool::Handle::release() noexcept {
    if (pool_ != nullptr) {
        pool_->release(index_);
        pool_ = nullptr;
        page_ = nullptr;
    }
}

ScratchPagePool::ScratchPagePool(std::size_t capacity) : pages_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument{"ScratchPagePool capacity must be greater than zero"};
    }
    free_indices_.reserve(capacity);
    for (std::size_t index = capacity; index > 0; --index) {
        free_indices_.push_back(index - 1);
    }
}

ScratchPagePool::Handle ScratchPagePool::acquire() {
    std::unique_lock lock{mutex_};
    available_.wait(lock, [this] { return !free_indices_.empty(); });
    const auto index = free_indices_.back();
    free_indices_.pop_back();
    return Handle{this, index, &pages_[index]};
}

std::optional<ScratchPagePool::Handle> ScratchPagePool::try_acquire() {
    std::lock_guard lock{mutex_};
    if (free_indices_.empty()) {
        return std::nullopt;
    }
    const auto index = free_indices_.back();
    free_indices_.pop_back();
    return Handle{this, index, &pages_[index]};
}

std::size_t ScratchPagePool::available() const {
    std::lock_guard lock{mutex_};
    return free_indices_.size();
}

void ScratchPagePool::release(std::size_t index) noexcept {
    {
        std::lock_guard lock{mutex_};
        free_indices_.push_back(index);
    }
    available_.notify_one();
}

} // namespace modb::storage
