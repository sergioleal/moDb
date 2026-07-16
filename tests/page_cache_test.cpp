// Importa o cache de páginas exercitado neste teste.
#include "modb/storage/page_cache.hpp"

// Importa as funções simples de verificação dos testes.
#include "test_support.hpp"

// Disponibiliza std::byte.
#include <cstddef>

using namespace modb;
using namespace modb::storage;

namespace {

// Cria uma página cujo primeiro byte marca a identidade, para checar conteúdo.
Page tagged_page(std::uint8_t tag) {
    Page page;
    page[0] = std::byte{tag};
    return page;
}

std::uint8_t first_byte(const Page& page) {
    return std::to_integer<std::uint8_t>(page.bytes()[0]);
}

} // namespace

int main() {
    TestSuite suite;

    // --- básico: miss, put, hit, update ---
    {
        PageCache cache{4};
        suite.check(cache.get(1) == nullptr, "a miss returns nullptr");
        cache.put(1, tagged_page(11));
        const Page* got = cache.get(1);
        suite.check(got != nullptr && first_byte(*got) == 11, "a put is retrievable with content");
        // Reescrever a mesma página atualiza o conteúdo, não duplica.
        cache.put(1, tagged_page(99));
        got = cache.get(1);
        suite.check(got != nullptr && first_byte(*got) == 99, "a re-put updates the content");
        suite.check(cache.size() == 1, "updating keeps a single entry");
    }

    // --- capacidade e remoção LRU ---
    {
        PageCache cache{3};
        cache.put(1, tagged_page(1));
        cache.put(2, tagged_page(2));
        cache.put(3, tagged_page(3));
        suite.check(cache.size() == 3, "cache fills to capacity");

        // Toca a página 1, tornando a 2 a menos recente.
        suite.check(cache.get(1) != nullptr, "page 1 is still present");
        // Inserir a 4 remove a menos recente (2), não a 1.
        cache.put(4, tagged_page(4));
        suite.check(cache.size() == 3, "cache never exceeds capacity");
        suite.check(cache.get(2) == nullptr, "the least-recently-used page is evicted");
        suite.check(cache.get(1) != nullptr, "a recently-used page survives eviction");
        suite.check(cache.get(3) != nullptr, "page 3 survives");
        suite.check(cache.get(4) != nullptr, "the newly inserted page is present");
    }

    // --- invalidate ---
    {
        PageCache cache{4};
        cache.put(7, tagged_page(7));
        suite.check(cache.get(7) != nullptr, "page 7 is present before invalidate");
        cache.invalidate(7);
        suite.check(cache.get(7) == nullptr, "invalidate removes the page");
        suite.check(cache.size() == 0, "invalidate shrinks the cache");
        // Invalidar uma página ausente é um no-op seguro.
        cache.invalidate(7);
        suite.check(cache.size() == 0, "invalidating a missing page is a safe no-op");
    }

    // --- capacidade zero é elevada a um ---
    {
        PageCache cache{0};
        suite.check(cache.capacity() == 1, "a zero capacity is raised to one");
        cache.put(1, tagged_page(1));
        cache.put(2, tagged_page(2));
        suite.check(cache.size() == 1, "capacity one keeps a single page");
        suite.check(cache.get(1) == nullptr && cache.get(2) != nullptr,
                    "the older page is evicted at capacity one");
    }

    return suite.finish();
}
