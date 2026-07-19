#pragma once

// Travessias BFS/DFS lazy sob um Snapshot (Fase 12C / ADR-015).

#include "modb/error.hpp"
#include "modb/object/ids.hpp"
#include "modb/query/generator.hpp"
#include "modb/query/operators.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace modb::graph {

enum class DanglingPolicy : std::uint8_t {
    fail = 0,        // primeira órfã aborta a travessia com erro
    skip = 1,        // ignora o vizinho órfão e segue
    yield_error = 2, // cede Result falho e continua (se possível)
};

struct GraphVisit {
    object::ObjectId id{};
    std::uint32_t depth{0};

    friend bool operator==(const GraphVisit&, const GraphVisit&) = default;
};

struct TraversalOptions {
    // 0 = sem limite de profundidade.
    std::uint32_t max_depth{0};
    // 0 = sem limite de vértices visitados (cedidos).
    std::uint64_t max_vertices{0};
    DanglingPolicy dangling{DanglingPolicy::fail};
    query::CancellationToken cancel{};
    bool has_cancel{false};
};

// Vizinhos de saída de um vértice, em ordem determinística.
using AdjacencyFn =
    std::function<Result<std::vector<object::ObjectId>>(object::ObjectId from)>;

// Política para um alvo que não resolve sob o snapshot.
using ResolveFn = std::function<Result<bool>(object::ObjectId target)>;

namespace detail {

[[nodiscard]] inline bool depth_exceeded(std::uint32_t depth,
                                         const TraversalOptions& options) noexcept {
    return options.max_depth != 0 && depth > options.max_depth;
}

[[nodiscard]] inline bool vertex_limit_reached(std::uint64_t visited,
                                               const TraversalOptions& options) noexcept {
    return options.max_vertices != 0 && visited >= options.max_vertices;
}

[[nodiscard]] inline bool cancelled(const TraversalOptions& options) noexcept {
    return options.has_cancel && options.cancel.cancelled();
}

// true = expandir; false = pular; unexpected = abortar (fail) ou ceder erro.
[[nodiscard]] inline Result<bool> accept_neighbor(object::ObjectId neighbor,
                                                  const ResolveFn& resolve,
                                                  DanglingPolicy policy) {
    if (!resolve) {
        return true;
    }
    auto ok = resolve(neighbor);
    if (ok) {
        return *ok ? true : false;
    }
    if (ok.error().code != ErrorCode::edge_target_not_found &&
        ok.error().code != ErrorCode::record_not_found) {
        return std::unexpected(ok.error());
    }
    switch (policy) {
    case DanglingPolicy::fail:
        return std::unexpected(Error{ErrorCode::edge_target_not_found, ok.error().message});
    case DanglingPolicy::skip:
        return false;
    case DanglingPolicy::yield_error:
        return std::unexpected(Error{ErrorCode::edge_target_not_found, ok.error().message});
    }
    return false;
}

} // namespace detail

// BFS: ordem por níveis; na mesma profundidade, ordem dos vizinhos do provedor.
[[nodiscard]] inline query::Generator<Result<GraphVisit>> bfs(object::ObjectId start,
                                                              AdjacencyFn adjacency,
                                                              TraversalOptions options = {},
                                                              ResolveFn resolve = {}) {
    if (!adjacency) {
        co_yield Result<GraphVisit>{
            std::unexpected(Error{ErrorCode::invalid_argument, "adjacency function is empty"})};
        co_return;
    }

    std::unordered_set<std::uint64_t> seen;
    std::deque<GraphVisit> queue;
    queue.push_back(GraphVisit{.id = start, .depth = 0});
    seen.insert(start.value);
    std::uint64_t yielded = 0;

    while (!queue.empty()) {
        if (detail::cancelled(options)) {
            co_return;
        }
        if (detail::vertex_limit_reached(yielded, options)) {
            co_yield Result<GraphVisit>{std::unexpected(
                Error{ErrorCode::graph_limit_exceeded, "BFS exceeded max_vertices"})};
            co_return;
        }

        const GraphVisit current = queue.front();
        queue.pop_front();
        co_yield Result<GraphVisit>{current};
        ++yielded;

        if (detail::depth_exceeded(current.depth + 1, options)) {
            continue;
        }

        auto neighbors = adjacency(current.id);
        if (!neighbors) {
            co_yield Result<GraphVisit>{std::unexpected(neighbors.error())};
            co_return;
        }
        for (const object::ObjectId neighbor : *neighbors) {
            if (detail::cancelled(options)) {
                co_return;
            }
            auto accept = detail::accept_neighbor(neighbor, resolve, options.dangling);
            if (!accept) {
                if (options.dangling == DanglingPolicy::yield_error) {
                    co_yield Result<GraphVisit>{std::unexpected(accept.error())};
                    continue;
                }
                co_yield Result<GraphVisit>{std::unexpected(accept.error())};
                co_return;
            }
            if (!*accept) {
                continue;
            }
            if (!seen.insert(neighbor.value).second) {
                continue;
            }
            queue.push_back(GraphVisit{.id = neighbor, .depth = current.depth + 1});
        }
    }
}

// DFS pré-ordem: desce no primeiro vizinho antes dos irmãos (ordem do provedor).
[[nodiscard]] inline query::Generator<Result<GraphVisit>> dfs(object::ObjectId start,
                                                              AdjacencyFn adjacency,
                                                              TraversalOptions options = {},
                                                              ResolveFn resolve = {}) {
    if (!adjacency) {
        co_yield Result<GraphVisit>{
            std::unexpected(Error{ErrorCode::invalid_argument, "adjacency function is empty"})};
        co_return;
    }

    std::unordered_set<std::uint64_t> seen;
    // Pilha de (vértice, próximo índice de vizinho a expandir). -1 = ainda não cedido.
    struct Frame {
        object::ObjectId id{};
        std::uint32_t depth{0};
        std::vector<object::ObjectId> neighbors{};
        std::size_t next{0};
        bool yielded{false};
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{.id = start, .depth = 0});
    seen.insert(start.value);
    std::uint64_t yielded_count = 0;

    while (!stack.empty()) {
        if (detail::cancelled(options)) {
            co_return;
        }
        Frame& frame = stack.back();
        if (!frame.yielded) {
            if (detail::vertex_limit_reached(yielded_count, options)) {
                co_yield Result<GraphVisit>{std::unexpected(
                    Error{ErrorCode::graph_limit_exceeded, "DFS exceeded max_vertices"})};
                co_return;
            }
            co_yield Result<GraphVisit>{GraphVisit{.id = frame.id, .depth = frame.depth}};
            ++yielded_count;
            frame.yielded = true;

            if (!detail::depth_exceeded(frame.depth + 1, options)) {
                auto neighbors = adjacency(frame.id);
                if (!neighbors) {
                    co_yield Result<GraphVisit>{std::unexpected(neighbors.error())};
                    co_return;
                }
                frame.neighbors = std::move(*neighbors);
            }
        }

        bool pushed = false;
        while (frame.next < frame.neighbors.size()) {
            if (detail::cancelled(options)) {
                co_return;
            }
            const object::ObjectId neighbor = frame.neighbors[frame.next++];
            auto accept = detail::accept_neighbor(neighbor, resolve, options.dangling);
            if (!accept) {
                if (options.dangling == DanglingPolicy::yield_error) {
                    co_yield Result<GraphVisit>{std::unexpected(accept.error())};
                    continue;
                }
                co_yield Result<GraphVisit>{std::unexpected(accept.error())};
                co_return;
            }
            if (!*accept) {
                continue;
            }
            if (!seen.insert(neighbor.value).second) {
                continue;
            }
            stack.push_back(Frame{.id = neighbor, .depth = frame.depth + 1});
            pushed = true;
            break;
        }
        if (!pushed) {
            stack.pop_back();
        }
    }
}

} // namespace modb::graph
