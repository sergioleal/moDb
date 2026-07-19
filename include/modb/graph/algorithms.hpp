#pragma once

// Algoritmos de grafo sob Snapshot (Fase 12D / ADR-015): caminho mínimo,
// ciclo, ordenação topológica e componentes conexos (view não direcionada).

#include "modb/error.hpp"
#include "modb/graph/traversal.hpp"
#include "modb/object/ids.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace modb::graph {

namespace detail {

[[nodiscard]] inline Result<void> check_adjacency(const AdjacencyFn& adjacency) {
    if (!adjacency) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "adjacency function is empty"});
    }
    return {};
}

[[nodiscard]] inline Result<bool> neighbor_ok(object::ObjectId neighbor,
                                              const ResolveFn& resolve,
                                              DanglingPolicy policy) {
    return accept_neighbor(neighbor, resolve, policy);
}

} // namespace detail

// Caminho mínimo sem peso (BFS) de `start` a `goal`, inclusive.
// Inalcançável → record_not_found. Respeita limites/cancel de TraversalOptions.
[[nodiscard]] inline Result<std::vector<object::ObjectId>>
shortest_path(object::ObjectId start, object::ObjectId goal, AdjacencyFn adjacency,
              TraversalOptions options = {}, ResolveFn resolve = {}) {
    if (auto status = detail::check_adjacency(adjacency); !status) {
        return std::unexpected(status.error());
    }
    if (start == goal) {
        return std::vector<object::ObjectId>{start};
    }

    std::unordered_map<std::uint64_t, object::ObjectId> parent;
    std::unordered_set<std::uint64_t> seen;
    std::deque<GraphVisit> queue;
    queue.push_back(GraphVisit{.id = start, .depth = 0});
    seen.insert(start.value);
    std::uint64_t expanded = 0;

    while (!queue.empty()) {
        if (detail::cancelled(options)) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "shortest_path cancelled"});
        }
        if (detail::vertex_limit_reached(expanded, options)) {
            return std::unexpected(
                Error{ErrorCode::graph_limit_exceeded, "shortest_path exceeded max_vertices"});
        }

        const GraphVisit current = queue.front();
        queue.pop_front();
        ++expanded;

        if (detail::depth_exceeded(current.depth + 1, options)) {
            continue;
        }

        auto neighbors = adjacency(current.id);
        if (!neighbors) {
            return std::unexpected(neighbors.error());
        }
        for (const object::ObjectId neighbor : *neighbors) {
            if (detail::cancelled(options)) {
                return std::unexpected(
                    Error{ErrorCode::invalid_argument, "shortest_path cancelled"});
            }
            auto accept = detail::neighbor_ok(neighbor, resolve, options.dangling);
            if (!accept) {
                return std::unexpected(accept.error());
            }
            if (!*accept) {
                continue;
            }
            if (!seen.insert(neighbor.value).second) {
                continue;
            }
            parent.emplace(neighbor.value, current.id);
            if (neighbor == goal) {
                std::vector<object::ObjectId> path;
                object::ObjectId cursor = goal;
                path.push_back(cursor);
                while (cursor != start) {
                    auto it = parent.find(cursor.value);
                    if (it == parent.end()) {
                        return std::unexpected(Error{ErrorCode::invalid_argument,
                                                     "shortest_path parent chain broken"});
                    }
                    cursor = it->second;
                    path.push_back(cursor);
                }
                std::reverse(path.begin(), path.end());
                return path;
            }
            queue.push_back(GraphVisit{.id = neighbor, .depth = current.depth + 1});
        }
    }

    return std::unexpected(
        Error{ErrorCode::record_not_found, "no path between start and goal"});
}

// true se existe ciclo dirigido no subgrafo induzido por `vertices`.
[[nodiscard]] inline Result<bool> has_cycle(const std::vector<object::ObjectId>& vertices,
                                            AdjacencyFn adjacency,
                                            TraversalOptions options = {},
                                            ResolveFn resolve = {}) {
    if (auto status = detail::check_adjacency(adjacency); !status) {
        return std::unexpected(status.error());
    }

    std::unordered_set<std::uint64_t> universe;
    universe.reserve(vertices.size());
    for (const auto id : vertices) {
        universe.insert(id.value);
    }

    enum class Color : std::uint8_t { white = 0, gray = 1, black = 2 };
    std::unordered_map<std::uint64_t, Color> color;
    color.reserve(vertices.size());
    for (const auto id : vertices) {
        color[id.value] = Color::white;
    }

    for (const object::ObjectId start : vertices) {
        if (color[start.value] != Color::white) {
            continue;
        }
        if (detail::cancelled(options)) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "has_cycle cancelled"});
        }

        struct Frame {
            object::ObjectId id{};
            std::vector<object::ObjectId> neighbors{};
            std::size_t next{0};
        };
        std::vector<Frame> stack;
        color[start.value] = Color::gray;
        {
            auto neighbors = adjacency(start);
            if (!neighbors) {
                return std::unexpected(neighbors.error());
            }
            stack.push_back(Frame{.id = start, .neighbors = std::move(*neighbors)});
        }

        while (!stack.empty()) {
            if (detail::cancelled(options)) {
                return std::unexpected(
                    Error{ErrorCode::invalid_argument, "has_cycle cancelled"});
            }
            Frame& frame = stack.back();
            bool pushed = false;
            while (frame.next < frame.neighbors.size()) {
                const object::ObjectId neighbor = frame.neighbors[frame.next++];
                if (!universe.contains(neighbor.value)) {
                    continue;
                }
                auto accept = detail::neighbor_ok(neighbor, resolve, options.dangling);
                if (!accept) {
                    return std::unexpected(accept.error());
                }
                if (!*accept) {
                    continue;
                }
                const Color state = color[neighbor.value];
                if (state == Color::gray) {
                    return true;
                }
                if (state == Color::white) {
                    color[neighbor.value] = Color::gray;
                    auto next_neighbors = adjacency(neighbor);
                    if (!next_neighbors) {
                        return std::unexpected(next_neighbors.error());
                    }
                    stack.push_back(
                        Frame{.id = neighbor, .neighbors = std::move(*next_neighbors)});
                    pushed = true;
                    break;
                }
            }
            if (!pushed) {
                color[frame.id.value] = Color::black;
                stack.pop_back();
            }
        }
    }
    return false;
}

// Ordenação topológica (Kahn). Grafo cíclico → graph_cycle.
[[nodiscard]] inline Result<std::vector<object::ObjectId>>
topological_sort(const std::vector<object::ObjectId>& vertices, AdjacencyFn adjacency,
                 TraversalOptions options = {}, ResolveFn resolve = {}) {
    if (auto status = detail::check_adjacency(adjacency); !status) {
        return std::unexpected(status.error());
    }

    std::unordered_set<std::uint64_t> universe;
    universe.reserve(vertices.size());
    for (const auto id : vertices) {
        universe.insert(id.value);
    }

    std::unordered_map<std::uint64_t, std::uint32_t> indegree;
    std::unordered_map<std::uint64_t, std::vector<object::ObjectId>> outgoing;
    indegree.reserve(vertices.size());
    outgoing.reserve(vertices.size());
    for (const auto id : vertices) {
        indegree[id.value] = 0;
    }

    for (const object::ObjectId from : vertices) {
        if (detail::cancelled(options)) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "topological_sort cancelled"});
        }
        auto neighbors = adjacency(from);
        if (!neighbors) {
            return std::unexpected(neighbors.error());
        }
        std::vector<object::ObjectId> filtered;
        filtered.reserve(neighbors->size());
        for (const object::ObjectId neighbor : *neighbors) {
            if (!universe.contains(neighbor.value)) {
                continue;
            }
            auto accept = detail::neighbor_ok(neighbor, resolve, options.dangling);
            if (!accept) {
                return std::unexpected(accept.error());
            }
            if (!*accept) {
                continue;
            }
            filtered.push_back(neighbor);
            ++indegree[neighbor.value];
        }
        outgoing.emplace(from.value, std::move(filtered));
    }

    std::deque<object::ObjectId> ready;
    for (const object::ObjectId id : vertices) {
        if (indegree[id.value] == 0) {
            ready.push_back(id);
        }
    }

    std::vector<object::ObjectId> order;
    order.reserve(vertices.size());
    while (!ready.empty()) {
        if (detail::cancelled(options)) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "topological_sort cancelled"});
        }
        const object::ObjectId current = ready.front();
        ready.pop_front();
        order.push_back(current);
        for (const object::ObjectId neighbor : outgoing[current.value]) {
            auto& degree = indegree[neighbor.value];
            if (degree > 0) {
                --degree;
            }
            if (degree == 0) {
                ready.push_back(neighbor);
            }
        }
    }

    if (order.size() != vertices.size()) {
        return std::unexpected(
            Error{ErrorCode::graph_cycle, "topological_sort requires a DAG"});
    }
    return order;
}

// Componentes conexos. `adjacency` deve ser a view não direcionada explícita
// (vizinhos em ambas as direções). Componentes ordenados pelo menor ObjectId;
// vértices dentro de cada componente em ordem crescente de id.
[[nodiscard]] inline Result<std::vector<std::vector<object::ObjectId>>>
connected_components(const std::vector<object::ObjectId>& vertices, AdjacencyFn adjacency,
                     TraversalOptions options = {}, ResolveFn resolve = {}) {
    if (auto status = detail::check_adjacency(adjacency); !status) {
        return std::unexpected(status.error());
    }

    std::unordered_set<std::uint64_t> universe;
    universe.reserve(vertices.size());
    for (const auto id : vertices) {
        universe.insert(id.value);
    }

    std::unordered_set<std::uint64_t> seen;
    std::vector<std::vector<object::ObjectId>> components;

    for (const object::ObjectId start : vertices) {
        if (!seen.insert(start.value).second) {
            continue;
        }
        if (detail::cancelled(options)) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "connected_components cancelled"});
        }

        std::vector<object::ObjectId> component;
        std::deque<object::ObjectId> queue;
        queue.push_back(start);
        while (!queue.empty()) {
            if (detail::cancelled(options)) {
                return std::unexpected(
                    Error{ErrorCode::invalid_argument, "connected_components cancelled"});
            }
            const object::ObjectId current = queue.front();
            queue.pop_front();
            component.push_back(current);

            auto neighbors = adjacency(current);
            if (!neighbors) {
                return std::unexpected(neighbors.error());
            }
            for (const object::ObjectId neighbor : *neighbors) {
                if (!universe.contains(neighbor.value)) {
                    continue;
                }
                auto accept = detail::neighbor_ok(neighbor, resolve, options.dangling);
                if (!accept) {
                    return std::unexpected(accept.error());
                }
                if (!*accept) {
                    continue;
                }
                if (seen.insert(neighbor.value).second) {
                    queue.push_back(neighbor);
                }
            }
        }

        std::sort(component.begin(), component.end(),
                  [](object::ObjectId a, object::ObjectId b) { return a.value < b.value; });
        components.push_back(std::move(component));
    }

    std::sort(components.begin(), components.end(),
              [](const std::vector<object::ObjectId>& a,
                 const std::vector<object::ObjectId>& b) {
                  if (a.empty()) {
                      return !b.empty();
                  }
                  if (b.empty()) {
                      return false;
                  }
                  return a.front().value < b.front().value;
              });
    return components;
}

} // namespace modb::graph
