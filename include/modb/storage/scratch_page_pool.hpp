#pragma once

#include "modb/error.hpp"
#include "modb/storage/page.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace modb::storage {

// Mantém um conjunto fixo de buffers Page reutilizáveis.
//
// O motor é single-thread e single-process por decisão de escopo (ESCOPO_MVP),
// então o pool não usa primitivas de sincronização: elas seriam peso morto e a
// espera bloqueante sem timeout de uma versão anterior podia travar sem
// diagnóstico. A indisponibilidade de um buffer é reportada por try_acquire.
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

    // Cria um pool validando a capacidade sem lançar exceções: zero é inválido.
    // Concentra num único ponto a checagem antes feita por cada chamador.
    [[nodiscard]] static Result<std::unique_ptr<ScratchPagePool>> create(std::size_t capacity);

    ScratchPagePool(const ScratchPagePool&) = delete;
    ScratchPagePool& operator=(const ScratchPagePool&) = delete;

    // Empresta um buffer livre, ou std::nullopt quando todos estão em uso.
    [[nodiscard]] std::optional<Handle> try_acquire();
    [[nodiscard]] std::size_t capacity() const noexcept { return pages_.size(); }
    [[nodiscard]] std::size_t available() const noexcept { return free_indices_.size(); }

private:
    // Aloca todos os buffers e índices uma única vez; só create pode construir.
    explicit ScratchPagePool(std::size_t capacity);
    void release(std::size_t index) noexcept;

    std::vector<Page> pages_;
    // A capacidade reservada impede alocações durante acquire/release.
    std::vector<std::size_t> free_indices_;
};

} // namespace modb::storage
