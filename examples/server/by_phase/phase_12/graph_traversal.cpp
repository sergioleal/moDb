#include "modb/graph/traversal.hpp"

#include <iostream>
#include <unordered_map>
#include <vector>

int main() {
    using modb::object::ObjectId;
    std::unordered_map<std::uint64_t, std::vector<ObjectId>> edges{
        {1, {ObjectId{2}, ObjectId{3}}},
        {2, {ObjectId{4}}},
        {3, {}},
        {4, {}},
    };

    int visited = 0;
    auto adjacency = [&edges](ObjectId from) -> modb::Result<std::vector<ObjectId>> {
        return edges[from.value];
    };
    for (auto& item : modb::graph::bfs(ObjectId{1}, adjacency)) {
        if (!item) {
            std::cerr << item.error().message << '\n';
            return 1;
        }
        std::cout << "visit " << item->id.value << " depth=" << item->depth << '\n';
        ++visited;
    }
    return visited == 4 ? 0 : 1;
}
