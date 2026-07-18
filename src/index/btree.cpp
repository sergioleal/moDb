// Implementação da B+ tree persistente (Fase 7B).
#include "modb/index/btree.hpp"

// Importa store_le/load_le para os campos internos das páginas (não são
// ordenáveis por si — a ordenação vem dos bytes de chave do key_codec).
#include "modb/storage/endian.hpp"

// Disponibiliza std::min e comparações.
#include <algorithm>
// Disponibiliza as assinaturas das páginas.
#include <array>
// Disponibiliza std::byte / std::size_t.
#include <cstddef>
// Disponibiliza std::memcpy.
#include <cstring>
// Disponibiliza std::string nas mensagens de erro.
#include <string>
// Disponibiliza std::move.
#include <utility>

namespace modb::index {
namespace {

using storage::load_le;
using storage::Page;
using storage::PageId;
using storage::store_le;

constexpr std::array<std::byte, 4> leaf_magic{std::byte{'B'}, std::byte{'T'}, std::byte{'L'},
                                              std::byte{'F'}};
constexpr std::array<std::byte, 4> internal_magic{std::byte{'B'}, std::byte{'T'}, std::byte{'I'},
                                                  std::byte{'N'}};
constexpr std::uint16_t btree_version = 1;

// Layout do header (32 bytes): magic(4) version(2) level(2) key_count(2)
// cell_top(2) reservado(4) link(8) reservado(8). `link` é next_leaf numa folha
// e leftmost_child numa interna.
constexpr std::size_t header_size = 32;
constexpr std::size_t off_level = 6;
constexpr std::size_t off_key_count = 8;
constexpr std::size_t off_cell_top = 10;
constexpr std::size_t off_link = 16;
constexpr std::size_t slot_base = header_size;
constexpr std::size_t slot_size = 2;

// Uma entrada lógica: bytes de valor, ObjectId de desempate e, em nós internos,
// o filho à direita deste separador.
struct Entry {
    std::vector<std::byte> key;
    std::uint64_t objid{};
    std::uint64_t child{};
};

bool node_is_leaf(const Page& page) {
    return std::equal(leaf_magic.begin(), leaf_magic.end(), page.bytes().begin());
}
std::uint16_t node_level(const Page& page) {
    return load_le<std::uint16_t>(page.bytes().subspan(off_level, 2));
}
std::uint16_t node_key_count(const Page& page) {
    return load_le<std::uint16_t>(page.bytes().subspan(off_key_count, 2));
}
std::uint64_t node_link(const Page& page) {
    return load_le<std::uint64_t>(page.bytes().subspan(off_link, 8));
}
std::uint16_t slot_at(const Page& page, std::size_t index) {
    return load_le<std::uint16_t>(page.bytes().subspan(slot_base + index * slot_size, slot_size));
}

std::size_t cell_bytes(std::size_t key_len, bool leaf) {
    return 2 + key_len + 8 + (leaf ? 0U : 8U);
}

// Leitura de uma célula em `offset`.
std::span<const std::byte> cell_key(const Page& page, std::size_t offset) {
    const auto key_len = load_le<std::uint16_t>(page.bytes().subspan(offset, 2));
    return page.bytes().subspan(offset + 2, key_len);
}
std::uint64_t cell_objid(const Page& page, std::size_t offset) {
    const auto key_len = load_le<std::uint16_t>(page.bytes().subspan(offset, 2));
    return load_le<std::uint64_t>(page.bytes().subspan(offset + 2 + key_len, 8));
}
std::uint64_t cell_child(const Page& page, std::size_t offset) {
    const auto key_len = load_le<std::uint16_t>(page.bytes().subspan(offset, 2));
    return load_le<std::uint64_t>(page.bytes().subspan(offset + 2 + key_len + 8, 8));
}

// Compara só os bytes de valor (lexicográfico, byte não sinalizado).
int cmp_keybytes(std::span<const std::byte> a, std::span<const std::byte> b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto ai = std::to_integer<std::uint8_t>(a[i]);
        const auto bi = std::to_integer<std::uint8_t>(b[i]);
        if (ai != bi) {
            return ai < bi ? -1 : 1;
        }
    }
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    return 0;
}

// Compara a chave composta (valor, ObjectId).
int cmp_composite(std::span<const std::byte> ka, std::uint64_t ida, std::span<const std::byte> kb,
                  std::uint64_t idb) {
    const int by_key = cmp_keybytes(ka, kb);
    if (by_key != 0) {
        return by_key;
    }
    if (ida != idb) {
        return ida < idb ? -1 : 1;
    }
    return 0;
}

// Primeiro índice de slot cuja composta é >= (key, objid).
std::size_t lower_bound_index(const Page& page, std::span<const std::byte> key,
                              std::uint64_t objid) {
    std::size_t lo = 0;
    std::size_t hi = node_key_count(page);
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        const auto off = slot_at(page, mid);
        if (cmp_composite(cell_key(page, off), cell_objid(page, off), key, objid) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// Primeiro índice de slot cuja composta é > (key, objid).
std::size_t upper_bound_index(const Page& page, std::span<const std::byte> key,
                              std::uint64_t objid) {
    std::size_t lo = 0;
    std::size_t hi = node_key_count(page);
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        const auto off = slot_at(page, mid);
        if (cmp_composite(cell_key(page, off), cell_objid(page, off), key, objid) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// Lê todas as entradas de um nó, em ordem.
std::vector<Entry> read_all(const Page& page) {
    const bool leaf = node_is_leaf(page);
    const auto count = node_key_count(page);
    std::vector<Entry> entries;
    entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto off = slot_at(page, i);
        const auto key = cell_key(page, off);
        Entry entry;
        entry.key.assign(key.begin(), key.end());
        entry.objid = cell_objid(page, off);
        entry.child = leaf ? 0 : cell_child(page, off);
        entries.push_back(std::move(entry));
    }
    return entries;
}

// Reescreve um nó do zero com as entradas dadas (assumidas ordenadas).
void write_node(Page& page, bool leaf, std::uint16_t level, std::uint64_t link,
                const std::vector<Entry>& entries) {
    page = Page{};  // zera tudo
    const auto& magic = leaf ? leaf_magic : internal_magic;
    std::copy(magic.begin(), magic.end(), page.bytes().begin());
    store_le<std::uint16_t>(page.bytes().subspan(4, 2), btree_version);
    store_le<std::uint16_t>(page.bytes().subspan(off_level, 2), level);
    store_le<std::uint16_t>(page.bytes().subspan(off_key_count, 2),
                            static_cast<std::uint16_t>(entries.size()));
    store_le<std::uint64_t>(page.bytes().subspan(off_link, 8), link);

    std::size_t cell_top = storage::page_size;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const std::size_t size = cell_bytes(entry.key.size(), leaf);
        cell_top -= size;
        auto bytes = page.bytes();
        store_le<std::uint16_t>(bytes.subspan(cell_top, 2),
                                static_cast<std::uint16_t>(entry.key.size()));
        std::memcpy(&bytes[cell_top + 2], entry.key.data(), entry.key.size());
        store_le<std::uint64_t>(bytes.subspan(cell_top + 2 + entry.key.size(), 8), entry.objid);
        if (!leaf) {
            store_le<std::uint64_t>(bytes.subspan(cell_top + 2 + entry.key.size() + 8, 8),
                                    entry.child);
        }
        store_le<std::uint16_t>(bytes.subspan(slot_base + i * slot_size, slot_size),
                                static_cast<std::uint16_t>(cell_top));
    }
    store_le<std::uint16_t>(page.bytes().subspan(off_cell_top, 2),
                            static_cast<std::uint16_t>(cell_top));
}

// Bytes totais que um conjunto de entradas ocupa (células + slots).
std::size_t total_bytes(const std::vector<Entry>& entries, std::size_t begin, std::size_t end,
                        bool leaf) {
    std::size_t total = 0;
    for (std::size_t i = begin; i < end; ++i) {
        total += cell_bytes(entries[i].key.size(), leaf) + slot_size;
    }
    return total;
}

constexpr std::size_t node_capacity = storage::page_size - header_size;

bool fits(const std::vector<Entry>& entries, bool leaf) {
    return total_bytes(entries, 0, entries.size(), leaf) <= node_capacity;
}

// Ponto de divisão: menor índice em que a metade esquerda passa de ~metade da
// capacidade, garantindo ambos os lados não vazios e cabendo em uma página.
std::size_t split_point(const std::vector<Entry>& entries, bool leaf) {
    const std::size_t half = node_capacity / 2;
    std::size_t used = 0;
    std::size_t mid = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        used += cell_bytes(entries[i].key.size(), leaf) + slot_size;
        if (used > half && i > 0) {
            mid = i;
            break;
        }
    }
    if (mid == 0) {
        mid = entries.size() / 2;
    }
    if (mid == 0) {
        mid = 1;
    }
    if (mid >= entries.size()) {
        mid = entries.size() - 1;
    }
    return mid;
}

// Desce da raiz até a folha que conteria (key, objid).
Result<PageId> descend(storage::PageFile& file, PageId root, std::span<const std::byte> key,
                       std::uint64_t objid) {
    PageId current = root;
    while (true) {
        auto page = file.read(current);
        if (!page) {
            return std::unexpected(page.error());
        }
        if (node_is_leaf(*page)) {
            return current;
        }
        const auto idx = upper_bound_index(*page, key, objid);
        current = (idx == 0) ? PageId{node_link(*page)}
                             : PageId{cell_child(*page, slot_at(*page, idx - 1))};
    }
}

// Sobe um split pela cadeia de pais, criando uma nova raiz se preciso.
// Atualiza `root` quando a raiz cresce em altura.
Result<void> propagate(storage::PageFile& file, PageId& root, Entry separator, PageId left_child,
                       std::vector<PageId>& path) {
    std::uint16_t child_level = 0;
    {
        auto child = file.read(left_child);
        if (!child) {
            return std::unexpected(child.error());
        }
        child_level = node_level(*child);
    }

    while (true) {
        if (path.empty()) {
            auto new_root = file.allocate_page();
            if (!new_root) {
                return std::unexpected(new_root.error());
            }
            std::vector<Entry> entries;
            entries.push_back(separator);
            Page page;
            write_node(page, /*leaf=*/false, static_cast<std::uint16_t>(child_level + 1),
                       /*link=*/left_child.value, entries);
            if (auto w = file.write(*new_root, page); !w) {
                return std::unexpected(w.error());
            }
            root = *new_root;
            return {};
        }

        const PageId parent_id = path.back();
        path.pop_back();
        auto parent = file.read(parent_id);
        if (!parent) {
            return std::unexpected(parent.error());
        }
        const std::uint16_t parent_level = node_level(*parent);
        const std::uint64_t leftmost = node_link(*parent);
        auto entries = read_all(*parent);
        const auto idx = lower_bound_index(*parent, separator.key, separator.objid);
        entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(idx), separator);

        if (fits(entries, /*leaf=*/false)) {
            Page page;
            write_node(page, false, parent_level, leftmost, entries);
            return file.write(parent_id, page);
        }

        const std::size_t mid = split_point(entries, false);
        Entry middle = entries[mid];
        std::vector<Entry> left_entries(entries.begin(),
                                        entries.begin() + static_cast<std::ptrdiff_t>(mid));
        std::vector<Entry> right_entries(entries.begin() + static_cast<std::ptrdiff_t>(mid) + 1,
                                         entries.end());
        auto right_id = file.allocate_page();
        if (!right_id) {
            return std::unexpected(right_id.error());
        }
        Page left_page;
        write_node(left_page, false, parent_level, leftmost, left_entries);
        Page right_page;
        write_node(right_page, false, parent_level, middle.child, right_entries);
        if (auto w = file.write(parent_id, left_page); !w) {
            return std::unexpected(w.error());
        }
        if (auto w = file.write(*right_id, right_page); !w) {
            return std::unexpected(w.error());
        }
        separator.key = middle.key;
        separator.objid = middle.objid;
        separator.child = right_id->value;
        child_level = parent_level;
        left_child = parent_id;
    }
}

} // namespace

Result<BTree> BTree::create(storage::PageFile& file) {
    auto root = file.allocate_page();
    if (!root) {
        return std::unexpected(root.error());
    }
    Page page;
    write_node(page, /*leaf=*/true, /*level=*/0, /*link=*/0, {});
    if (auto written = file.write(*root, page); !written) {
        return std::unexpected(written.error());
    }
    return BTree{file, *root};
}

Result<BTree> BTree::open(storage::PageFile& file, storage::PageId root) {
    auto page = file.read(root);
    if (!page) {
        return std::unexpected(page.error());
    }
    if (!node_is_leaf(*page) &&
        !std::equal(internal_magic.begin(), internal_magic.end(), page->bytes().begin())) {
        return std::unexpected(Error{ErrorCode::invalid_page_format, "not a B+ tree root page"});
    }
    return BTree{file, root};
}

Result<void> BTree::insert(std::span<const std::byte> key, std::uint64_t object_id) {
    if (cell_bytes(key.size(), /*leaf=*/false) + slot_size > node_capacity / 2) {
        return std::unexpected(
            Error{ErrorCode::value_too_large, "index key is too large for a B+ tree node"});
    }

    // Desce até a folha guardando o caminho de nós internos.
    std::vector<PageId> path;
    PageId current = root_;
    while (true) {
        auto page = file_->read(current);
        if (!page) {
            return std::unexpected(page.error());
        }
        if (node_is_leaf(*page)) {
            break;
        }
        path.push_back(current);
        const auto idx = upper_bound_index(*page, key, object_id);
        current = (idx == 0) ? PageId{node_link(*page)} : PageId{cell_child(*page, slot_at(*page, idx - 1))};
    }

    // Insere na folha.
    auto leaf = file_->read(current);
    if (!leaf) {
        return std::unexpected(leaf.error());
    }
    const auto idx = lower_bound_index(*leaf, key, object_id);
    if (idx < node_key_count(*leaf)) {
        const auto off = slot_at(*leaf, idx);
        if (cmp_composite(cell_key(*leaf, off), cell_objid(*leaf, off), key, object_id) == 0) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "duplicate index entry"});
        }
    }
    auto entries = read_all(*leaf);
    Entry fresh;
    fresh.key.assign(key.begin(), key.end());
    fresh.objid = object_id;
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(idx), std::move(fresh));
    const std::uint64_t old_link = node_link(*leaf);

    if (fits(entries, /*leaf=*/true)) {
        Page page;
        write_node(page, true, 0, old_link, entries);
        return file_->write(current, page);
    }

    // Split de folha.
    const std::size_t mid = split_point(entries, true);
    auto right_id = file_->allocate_page();
    if (!right_id) {
        return std::unexpected(right_id.error());
    }
    std::vector<Entry> left_entries(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(mid));
    std::vector<Entry> right_entries(entries.begin() + static_cast<std::ptrdiff_t>(mid), entries.end());
    Page left_page;
    write_node(left_page, true, 0, right_id->value, left_entries);
    Page right_page;
    write_node(right_page, true, 0, old_link, right_entries);
    if (auto w = file_->write(current, left_page); !w) {
        return std::unexpected(w.error());
    }
    if (auto w = file_->write(*right_id, right_page); !w) {
        return std::unexpected(w.error());
    }

    // Sobe o separador (menor chave da folha direita) e o filho direito.
    Entry separator;
    separator.key = right_entries.front().key;
    separator.objid = right_entries.front().objid;
    separator.child = right_id->value;
    return propagate(*file_, root_, std::move(separator), current, path);
}

Result<void> BTree::remove(std::span<const std::byte> key, std::uint64_t object_id) {
    // Remoção de folha, sem rebalance: desce direto à folha.
    auto current_id = descend(*file_, root_, key, object_id);
    if (!current_id) {
        return std::unexpected(current_id.error());
    }
    const PageId current = *current_id;
    auto leaf = file_->read(current);
    if (!leaf) {
        return std::unexpected(leaf.error());
    }
    const auto idx = lower_bound_index(*leaf, key, object_id);
    if (idx >= node_key_count(*leaf)) {
        return std::unexpected(Error{ErrorCode::record_not_found, "index entry not found"});
    }
    const auto off = slot_at(*leaf, idx);
    if (cmp_composite(cell_key(*leaf, off), cell_objid(*leaf, off), key, object_id) != 0) {
        return std::unexpected(Error{ErrorCode::record_not_found, "index entry not found"});
    }
    auto entries = read_all(*leaf);
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(idx));
    Page page;
    // Remoção simples de folha: sem merge/borrow (a folha pode ficar abaixo do
    // mínimo). find/range seguem corretos; a compactação plena é pós-MVP.
    write_node(page, true, 0, node_link(*leaf), entries);
    return file_->write(current, page);
}

Result<std::vector<std::uint64_t>> BTree::range(std::span<const std::byte> lo,
                                                std::span<const std::byte> hi) const {
    std::vector<std::uint64_t> out;
    auto leaf_id = descend(*file_, root_, lo, 0);
    if (!leaf_id) {
        return std::unexpected(leaf_id.error());
    }
    PageId current = *leaf_id;
    bool first = true;
    while (true) {
        auto page = file_->read(current);
        if (!page) {
            return std::unexpected(page.error());
        }
        const auto count = node_key_count(*page);
        std::size_t start = 0;
        if (first) {
            start = lower_bound_index(*page, lo, 0);
            first = false;
        }
        for (std::size_t i = start; i < count; ++i) {
            const auto off = slot_at(*page, i);
            if (cmp_keybytes(cell_key(*page, off), hi) > 0) {
                return out;  // ultrapassou o limite superior
            }
            out.push_back(cell_objid(*page, off));
        }
        const auto next = node_link(*page);
        if (next == 0) {
            return out;
        }
        current = PageId{next};
    }
}

Result<std::vector<std::uint64_t>> BTree::find(std::span<const std::byte> key) const {
    return range(key, key);
}

Result<std::uint32_t> BTree::validate() const {
    // Percorre a árvore conferindo: ordenação estrita dentro de cada nó,
    // profundidade uniforme das folhas e consistência separador == menor chave
    // do filho direito. Devolve a altura (folha = 1).
    struct Frame {
        PageId id;
        std::uint32_t depth;
    };
    std::vector<Frame> stack{{root_, 1}};
    std::uint32_t leaf_depth = 0;
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        auto page = file_->read(frame.id);
        if (!page) {
            return std::unexpected(page.error());
        }
        const auto count = node_key_count(*page);
        // Ordenação estrita.
        for (std::size_t i = 1; i < count; ++i) {
            const auto a = slot_at(*page, i - 1);
            const auto b = slot_at(*page, i);
            if (cmp_composite(cell_key(*page, a), cell_objid(*page, a), cell_key(*page, b),
                              cell_objid(*page, b)) >= 0) {
                return std::unexpected(Error{ErrorCode::corrupt_page, "B+ node keys out of order"});
            }
        }
        if (node_is_leaf(*page)) {
            if (leaf_depth == 0) {
                leaf_depth = frame.depth;
            } else if (leaf_depth != frame.depth) {
                return std::unexpected(
                    Error{ErrorCode::corrupt_page, "B+ leaves at differing depths"});
            }
            continue;
        }
        // Interno: empilha filhos.
        stack.push_back({PageId{node_link(*page)}, frame.depth + 1});
        for (std::size_t i = 0; i < count; ++i) {
            stack.push_back({PageId{cell_child(*page, slot_at(*page, i))}, frame.depth + 1});
        }
    }
    return leaf_depth;
}

} // namespace modb::index
