#include "modb/storage/scratch_page_pool.hpp"

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
    free_indices_.reserve(capacity);
    for (std::size_t index = capacity; index > 0; --index) {
        free_indices_.push_back(index - 1);
    }
}

Result<std::unique_ptr<ScratchPagePool>> ScratchPagePool::create(std::size_t capacity) {
    if (capacity == 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
                                     "ScratchPagePool capacity must be greater than zero"});
    }
    // O construtor é privado; usa new porque make_unique não o alcança.
    return std::unique_ptr<ScratchPagePool>{new ScratchPagePool{capacity}};
}

std::optional<ScratchPagePool::Handle> ScratchPagePool::try_acquire() {
    if (free_indices_.empty()) {
        return std::nullopt;
    }
    const auto index = free_indices_.back();
    free_indices_.pop_back();
    return Handle{this, index, &pages_[index]};
}

void ScratchPagePool::release(std::size_t index) noexcept {
    free_indices_.push_back(index);
}

} // namespace modb::storage
