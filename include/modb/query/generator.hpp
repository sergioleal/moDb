#pragma once

// Gerador preguiçoso próprio sobre coroutines C++20 (Fase 7A). Sem depender de
// std::generator (C++23), para compilar em MinGW GCC, MSVC e Clang. Erros não
// atravessam a fronteira como exceções: eles fluem como valores, tipando o
// stream como Generator<Result<T>>. Uma exceção genuína (ex.: bad_alloc dentro
// da coroutine) é capturada e relançada no consumidor — nunca perdida.

// Disponibiliza o suporte de coroutines.
#include <coroutine>
// Disponibiliza std::exception_ptr.
#include <exception>
// Disponibiliza std::input_iterator_tag e std::default_sentinel_t.
#include <iterator>
// Disponibiliza o valor corrente sem exigir T default-construtível.
#include <optional>
// Disponibiliza std::exchange/std::move.
#include <utility>

namespace modb::query {

// Stream preguiçoso de valores T produzidos por `co_yield`. Move-only: possui a
// coroutine e a destrói (liberando toda a cadeia de upstreams) no destrutor —
// abandonar o gerador no meio é seguro e não vaza.
template <typename T>
class Generator {
public:
    struct promise_type {
        // Guarda o último valor cedido; optional evita exigir T() de T.
        std::optional<T> current_;
        // Guarda uma exceção genuína escapada da coroutine (não deveria ocorrer
        // no uso normal, em que erros são valores Result).
        std::exception_ptr exception_;

        Generator get_return_object() {
            return Generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        // Começa suspenso: nada roda até o primeiro `begin()`/`++` (preguiça).
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(T value) {
            current_.emplace(std::move(value));
            return {};
        }
        void return_void() noexcept {}
        void unhandled_exception() { exception_ = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    Generator() noexcept = default;
    explicit Generator(handle_type handle) noexcept : handle_{handle} {}
    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;
    Generator(Generator&& other) noexcept : handle_{std::exchange(other.handle_, {})} {}
    Generator& operator=(Generator&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    ~Generator() { destroy(); }

    // Iterador de input de passagem única. Comparar com o sentinela é "acabou?".
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T&;

        iterator() noexcept = default;
        explicit iterator(handle_type handle) noexcept : handle_{handle} {}

        iterator& operator++() {
            handle_.resume();
            rethrow_if_failed();
            return *this;
        }
        void operator++(int) { ++*this; }

        [[nodiscard]] T& operator*() const { return *handle_.promise().current_; }
        [[nodiscard]] T* operator->() const { return &*handle_.promise().current_; }

        [[nodiscard]] bool operator==(std::default_sentinel_t) const noexcept {
            return !handle_ || handle_.done();
        }

    private:
        void rethrow_if_failed() const {
            if (handle_.done() && handle_.promise().exception_) {
                std::rethrow_exception(handle_.promise().exception_);
            }
        }
        handle_type handle_{};
    };

    // Avança até o primeiro valor (a coroutine começa suspensa).
    [[nodiscard]] iterator begin() {
        if (handle_) {
            handle_.resume();
            if (handle_.done() && handle_.promise().exception_) {
                std::rethrow_exception(handle_.promise().exception_);
            }
        }
        return iterator{handle_};
    }
    [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }

    // Verdadeiro quando há uma coroutine associada (não foi movido para fora).
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(handle_); }

private:
    void destroy() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }
    handle_type handle_{};
};

} // namespace modb::query
