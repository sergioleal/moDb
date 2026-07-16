#pragma once

#include "modb/storage/page.hpp"

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace modb::storage {

// Mantém um conjunto fixo de buffers Page reutilizáveis.
class ScratchPagePool {
public:
    class Handle {
    public:
        Handle() noexcept = default;
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        Handle(Handle&& other) noexcept;
        Handle& operator=(Handle&& other) noexcept;
        ~Handle();

        [[nodiscard]] Page& get() noexcept { return *page_; }
        [[nodiscard]] const Page& get() const noexcept { return *page_; }
        [[nodiscard]] Page& operator*() noexcept { return get(); }
        [[nodiscard]] const Page& operator*() const noexcept { return get(); }
        [[nodiscard]] Page* operator->() noexcept { return page_; }
        [[nodiscard]] const Page* operator->() const noexcept { return page_; }
        [[nodiscard]] explicit operator bool() const noexcept { return page_ != nullptr; }

    private:
        friend class ScratchPagePool;
        Handle(ScratchPagePool* pool, std::size_t index, Page* page) noexcept;
        void release() noexcept;

        ScratchPagePool* pool_ = nullptr;
        std::size_t index_ = 0;
        Page* page_ = nullptr;
    };

    // Aloca todos os buffers e índices uma única vez; zero é inválido.
    explicit ScratchPagePool(std::size_t capacity);
    ScratchPagePool(const ScratchPagePool&) = delete;
    ScratchPagePool& operator=(const ScratchPagePool&) = delete;

    [[nodiscard]] Handle acquire();
    [[nodiscard]] std::optional<Handle> try_acquire();
    [[nodiscard]] std::size_t capacity() const noexcept { return pages_.size(); }
    [[nodiscard]] std::size_t available() const;

private:
    void release(std::size_t index) noexcept;

    std::vector<Page> pages_;
    // A capacidade reservada impede alocações durante acquire/release.
    std::vector<std::size_t> free_indices_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
};

} // namespace modb::storage
