#pragma once

// Operadores de stream das Fases 7A/7C/7D.
//
// Streaming (O(1)): limit, filter, project, compute, cancellable, merge
//   (merge exige entradas já ordenadas).
// Parcialmente bloqueante: top_k (heap de no máximo k elementos).
// Bloqueante: sort, distinct, aggregate (materializam a entrada).

#include "modb/error.hpp"
#include "modb/query/generator.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace modb::query {

enum class OperatorNature : std::uint8_t {
    streaming = 0,
    partially_blocking = 1,
    blocking = 2,
};

[[nodiscard]] constexpr OperatorNature nature_of_limit() noexcept {
    return OperatorNature::streaming;
}
[[nodiscard]] constexpr OperatorNature nature_of_filter() noexcept {
    return OperatorNature::streaming;
}
[[nodiscard]] constexpr OperatorNature nature_of_project() noexcept {
    return OperatorNature::streaming;
}
[[nodiscard]] constexpr OperatorNature nature_of_compute() noexcept {
    return OperatorNature::streaming;
}
[[nodiscard]] constexpr OperatorNature nature_of_merge() noexcept {
    return OperatorNature::streaming;
}
[[nodiscard]] constexpr OperatorNature nature_of_top_k() noexcept {
    return OperatorNature::partially_blocking;
}
[[nodiscard]] constexpr OperatorNature nature_of_sort() noexcept {
    return OperatorNature::blocking;
}
[[nodiscard]] constexpr OperatorNature nature_of_distinct() noexcept {
    return OperatorNature::blocking;
}
[[nodiscard]] constexpr OperatorNature nature_of_aggregate() noexcept {
    return OperatorNature::blocking;
}

[[nodiscard]] constexpr const char* nature_name(OperatorNature nature) noexcept {
    switch (nature) {
    case OperatorNature::streaming:
        return "streaming";
    case OperatorNature::partially_blocking:
        return "partially_blocking";
    case OperatorNature::blocking:
        return "blocking";
    }
    return "unknown";
}

class CancellationToken {
public:
    CancellationToken() : flag_{std::make_shared<std::atomic<bool>>(false)} {}

    void cancel() noexcept { flag_->store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool cancelled() const noexcept {
        return flag_->load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

inline void observe_peak(std::size_t* peak, std::size_t live) noexcept {
    if (peak != nullptr && live > *peak) {
        *peak = live;
    }
}

template <typename T>
Generator<Result<T>> limit(Generator<Result<T>> source, std::size_t n) {
    if (n == 0) {
        for (auto& item : source) {
            co_yield std::move(item);
        }
        co_return;
    }
    std::size_t emitted = 0;
    for (auto& item : source) {
        co_yield std::move(item);
        if (++emitted >= n) {
            co_return;
        }
    }
}

template <typename T, typename Predicate>
Generator<Result<T>> filter(Generator<Result<T>> source, Predicate predicate) {
    for (auto& item : source) {
        if (!item) {
            co_yield std::move(item);
            co_return;
        }
        if (predicate(*item)) {
            co_yield std::move(item);
        }
    }
}

template <typename In, typename Out, typename Projector>
Generator<Result<Out>> project(Generator<Result<In>> source, Projector projector) {
    for (auto& item : source) {
        if (!item) {
            co_yield Result<Out>{std::unexpected(item.error())};
            co_return;
        }
        co_yield projector(*item);
    }
}

template <typename In, typename Out, typename Function>
Generator<Result<Out>> compute(Generator<Result<In>> source, Function function) {
    for (auto& item : source) {
        if (!item) {
            co_yield Result<Out>{std::unexpected(item.error())};
            co_return;
        }
        co_yield function(*item);
    }
}

template <typename T>
Generator<Result<T>> cancellable(Generator<Result<T>> source, CancellationToken token) {
    for (auto& item : source) {
        if (token.cancelled()) {
            co_return;
        }
        co_yield std::move(item);
    }
}

// sort (Fase 7D, BLOQUEANTE): materializa todos os Ok, ordena e cede. `less`
// compara dois T; `peak` (opcional) recebe o pico de elementos retidos.
template <typename T, typename Less = std::less<T>>
Generator<Result<T>> sort(Generator<Result<T>> source, Less less = {},
                          std::size_t* peak = nullptr) {
    std::vector<T> rows;
    for (auto& item : source) {
        if (!item) {
            co_yield Result<T>{std::unexpected(item.error())};
            co_return;
        }
        rows.push_back(std::move(*item));
        observe_peak(peak, rows.size());
    }
    std::stable_sort(rows.begin(), rows.end(), less);
    for (auto& row : rows) {
        co_yield Result<T>{std::move(row)};
    }
}

// top_k (Fase 7D, PARCIALMENTE BLOQUEANTE): mantém no máximo k elementos no
// heap. Por padrão devolve os k "maiores" segundo `less` (os que ficariam no
// fim de um sort crescente). Pico ≤ k.
template <typename T, typename Less = std::less<T>>
Generator<Result<T>> top_k(Generator<Result<T>> source, std::size_t k, Less less = {},
                           std::size_t* peak = nullptr) {
    if (k == 0) {
        co_return;
    }
    // Heap mínimo dos k melhores: heap[0] é o pior entre os selecionados.
    auto worse = [&](const T& a, const T& b) { return less(b, a); };
    std::vector<T> heap;
    heap.reserve(k);
    for (auto& item : source) {
        if (!item) {
            co_yield Result<T>{std::unexpected(item.error())};
            co_return;
        }
        if (heap.size() < k) {
            heap.push_back(std::move(*item));
            std::push_heap(heap.begin(), heap.end(), worse);
        } else if (less(heap.front(), *item)) {
            std::pop_heap(heap.begin(), heap.end(), worse);
            heap.back() = std::move(*item);
            std::push_heap(heap.begin(), heap.end(), worse);
        }
        observe_peak(peak, heap.size());
    }
    // Comp=worse (maior segundo less) → heap mínimo dos k melhores; front = pior.
    // sort_heap com o mesmo Comp deixa o melhor primeiro (ordem descrescente).
    std::sort_heap(heap.begin(), heap.end(), worse);
    for (auto& row : heap) {
        co_yield Result<T>{std::move(row)};
    }
}

// distinct (Fase 7D, BLOQUEANTE): cede a primeira ocorrência de cada chave.
template <typename T, typename KeyFn>
Generator<Result<T>> distinct(Generator<Result<T>> source, KeyFn key_fn,
                              std::size_t* peak = nullptr) {
    using Key = std::decay_t<decltype(key_fn(std::declval<const T&>()))>;
    std::set<Key> seen;
    for (auto& item : source) {
        if (!item) {
            co_yield Result<T>{std::unexpected(item.error())};
            co_return;
        }
        const Key key = key_fn(*item);
        if (seen.insert(key).second) {
            observe_peak(peak, seen.size());
            co_yield Result<T>{std::move(*item)};
        } else {
            observe_peak(peak, seen.size());
        }
    }
}

// aggregate (Fase 7D, BLOQUEANTE): reduz a entrada a um único Acc.
template <typename T, typename Acc, typename Fold>
Generator<Result<Acc>> aggregate(Generator<Result<T>> source, Acc initial, Fold fold) {
    Acc acc = std::move(initial);
    for (auto& item : source) {
        if (!item) {
            co_yield Result<Acc>{std::unexpected(item.error())};
            co_return;
        }
        acc = fold(std::move(acc), *item);
    }
    co_yield Result<Acc>{std::move(acc)};
}

// merge (Fase 7D, STREAMING): funde duas sequências já ordenadas por `less`.
template <typename T, typename Less = std::less<T>>
Generator<Result<T>> merge(Generator<Result<T>> left, Generator<Result<T>> right,
                           Less less = {}) {
    auto left_it = left.begin();
    auto right_it = right.begin();
    const auto left_end = left.end();
    const auto right_end = right.end();

    auto take_left = [&]() -> Result<T> {
        Result<T> value = std::move(*left_it);
        ++left_it;
        return value;
    };
    auto take_right = [&]() -> Result<T> {
        Result<T> value = std::move(*right_it);
        ++right_it;
        return value;
    };

    while (left_it != left_end && right_it != right_end) {
        if (!*left_it) {
            co_yield Result<T>{std::unexpected((*left_it).error())};
            co_return;
        }
        if (!*right_it) {
            co_yield Result<T>{std::unexpected((*right_it).error())};
            co_return;
        }
        if (!less(**right_it, **left_it)) {
            co_yield take_left();
        } else {
            co_yield take_right();
        }
    }
    while (left_it != left_end) {
        if (!*left_it) {
            co_yield Result<T>{std::unexpected((*left_it).error())};
            co_return;
        }
        co_yield take_left();
    }
    while (right_it != right_end) {
        if (!*right_it) {
            co_yield Result<T>{std::unexpected((*right_it).error())};
            co_return;
        }
        co_yield take_right();
    }
}

} // namespace modb::query
