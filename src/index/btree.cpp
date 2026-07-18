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

bool underfull(const std::vector<Entry>& entries, bool leaf) {
    // Com chaves de tamanho variável o split não garante 50/50; 1/3 da
    // capacidade é o mínimo exigido das não-raiz após remoção.
    return total_bytes(entries, 0, entries.size(), leaf) < node_capacity / 3;
}

// Índice do filho em `parent`: -1 = leftmost (`link`), senão o slot cujo
// `child` aponta para `child_id`.
int child_slot(const Page& parent, PageId child_id) {
    if (PageId{node_link(parent)} == child_id) {
        return -1;
    }
    const auto count = node_key_count(parent);
    for (std::size_t i = 0; i < count; ++i) {
        if (PageId{cell_child(parent, slot_at(parent, i))} == child_id) {
            return static_cast<int>(i);
        }
    }
    return -2;
}

PageId sibling_at(const Page& parent, int slot) {
    if (slot < 0) {
        return PageId{node_link(parent)};
    }
    return PageId{cell_child(parent, slot_at(parent, static_cast<std::size_t>(slot)))};
}

Result<void> write_orphan(storage::PageFile& file, PageId id) {
    return file.write(id, Page{});
}

// Sobe o rebalanceamento após uma remoção: borrow do irmão ou merge. `node_id`
// é o nó já reescrito em memória nas `entries`/`link`/`leaf`/`level` passados.
Result<void> rebalance(storage::PageFile& file, PageId& root, PageId node_id, bool leaf,
                       std::uint16_t level, std::uint64_t link, std::vector<Entry> entries,
                       std::vector<PageId>& path) {
    while (true) {
        if (path.empty()) {
            // Raiz: pode ficar abaixo do mínimo. Se uma interna ficou sem
            // separadores, o único filho vira a nova raiz (altura −1).
            if (!leaf && entries.empty()) {
                const PageId child{link};
                if (auto orphaned = write_orphan(file, node_id); !orphaned) {
                    return orphaned;
                }
                root = child;
                return {};
            }
            Page page;
            write_node(page, leaf, level, link, entries);
            return file.write(node_id, page);
        }

        if (!underfull(entries, leaf)) {
            Page page;
            write_node(page, leaf, level, link, entries);
            return file.write(node_id, page);
        }

        const PageId parent_id = path.back();
        path.pop_back();
        auto parent = file.read(parent_id);
        if (!parent) {
            return std::unexpected(parent.error());
        }
        const std::uint16_t parent_level = node_level(*parent);
        std::uint64_t parent_leftmost = node_link(*parent);
        auto parent_entries = read_all(*parent);
        const int slot = child_slot(*parent, node_id);
        if (slot < -1) {
            return std::unexpected(Error{ErrorCode::corrupt_page, "B+ child missing from parent"});
        }

        const int right_slot = slot + 1;
        const bool has_right = right_slot < static_cast<int>(parent_entries.size());
        const bool has_left = slot >= 0;

        auto try_borrow_right = [&]() -> Result<bool> {
            if (!has_right) {
                return false;
            }
            const PageId right_id = PageId{parent_entries[static_cast<std::size_t>(right_slot)].child};
            auto right = file.read(right_id);
            if (!right) {
                return std::unexpected(right.error());
            }
            auto right_entries = read_all(*right);
            const std::uint64_t right_link = node_link(*right);
            if (leaf) {
                std::size_t take = 0;
                while (take < right_entries.size()) {
                    std::vector<Entry> donor(right_entries.begin() + static_cast<std::ptrdiff_t>(take + 1),
                                             right_entries.end());
                    if (underfull(donor, true)) {
                        break;
                    }
                    ++take;
                    std::vector<Entry> trial = entries;
                    trial.insert(trial.end(), right_entries.begin(),
                                 right_entries.begin() + static_cast<std::ptrdiff_t>(take));
                    if (!underfull(trial, true) && fits(trial, true)) {
                        entries = std::move(trial);
                        right_entries.erase(right_entries.begin(),
                                            right_entries.begin() + static_cast<std::ptrdiff_t>(take));
                        parent_entries[static_cast<std::size_t>(right_slot)].key =
                            right_entries.front().key;
                        parent_entries[static_cast<std::size_t>(right_slot)].objid =
                            right_entries.front().objid;
                        Page left_page;
                        write_node(left_page, true, level, link, entries);
                        Page right_page;
                        write_node(right_page, true, level, right_link, right_entries);
                        Page parent_page;
                        write_node(parent_page, false, parent_level, parent_leftmost, parent_entries);
                        if (auto w = file.write(node_id, left_page); !w) {
                            return std::unexpected(w.error());
                        }
                        if (auto w = file.write(right_id, right_page); !w) {
                            return std::unexpected(w.error());
                        }
                        if (auto w = file.write(parent_id, parent_page); !w) {
                            return std::unexpected(w.error());
                        }
                        return true;
                    }
                }
                return false;
            }
            if (right_entries.empty()) {
                return false;
            }
            std::vector<Entry> donor(right_entries.begin() + 1, right_entries.end());
            if (underfull(donor, false)) {
                return false;
            }
            Entry down = parent_entries[static_cast<std::size_t>(right_slot)];
            down.child = right_link;
            Entry up = right_entries.front();
            const std::uint64_t new_right_link = up.child;
            std::vector<Entry> trial = entries;
            trial.push_back(std::move(down));
            if (underfull(trial, false) || !fits(trial, false)) {
                return false;
            }
            parent_entries[static_cast<std::size_t>(right_slot)].key = up.key;
            parent_entries[static_cast<std::size_t>(right_slot)].objid = up.objid;
            entries = std::move(trial);
            right_entries.erase(right_entries.begin());
            Page left_page;
            write_node(left_page, false, level, link, entries);
            Page right_page;
            write_node(right_page, false, level, new_right_link, right_entries);
            Page parent_page;
            write_node(parent_page, false, parent_level, parent_leftmost, parent_entries);
            if (auto w = file.write(node_id, left_page); !w) {
                return std::unexpected(w.error());
            }
            if (auto w = file.write(right_id, right_page); !w) {
                return std::unexpected(w.error());
            }
            if (auto w = file.write(parent_id, parent_page); !w) {
                return std::unexpected(w.error());
            }
            return true;
        };

        auto try_borrow_left = [&]() -> Result<bool> {
            if (!has_left) {
                return false;
            }
            const PageId left_id = sibling_at(*parent, slot - 1);
            auto left = file.read(left_id);
            if (!left) {
                return std::unexpected(left.error());
            }
            auto left_entries = read_all(*left);
            const std::uint64_t left_link = node_link(*left);
            if (leaf) {
                std::size_t take = 0;
                while (take < left_entries.size()) {
                    std::vector<Entry> donor(
                        left_entries.begin(),
                        left_entries.end() - static_cast<std::ptrdiff_t>(take + 1));
                    if (underfull(donor, true)) {
                        break;
                    }
                    ++take;
                    std::vector<Entry> trial;
                    trial.insert(trial.end(), left_entries.end() - static_cast<std::ptrdiff_t>(take),
                                 left_entries.end());
                    trial.insert(trial.end(), entries.begin(), entries.end());
                    if (!underfull(trial, true) && fits(trial, true)) {
                        entries = std::move(trial);
                        left_entries.erase(left_entries.end() - static_cast<std::ptrdiff_t>(take),
                                           left_entries.end());
                        parent_entries[static_cast<std::size_t>(slot)].key = entries.front().key;
                        parent_entries[static_cast<std::size_t>(slot)].objid = entries.front().objid;
                        Page left_page;
                        write_node(left_page, true, level, left_link, left_entries);
                        Page right_page;
                        write_node(right_page, true, level, link, entries);
                        Page parent_page;
                        write_node(parent_page, false, parent_level, parent_leftmost, parent_entries);
                        if (auto w = file.write(left_id, left_page); !w) {
                            return std::unexpected(w.error());
                        }
                        if (auto w = file.write(node_id, right_page); !w) {
                            return std::unexpected(w.error());
                        }
                        if (auto w = file.write(parent_id, parent_page); !w) {
                            return std::unexpected(w.error());
                        }
                        return true;
                    }
                }
                return false;
            }
            // Interna: empresta uma entrada (o separador do pai desce; o último
            // do irmão sobe e vira o novo separador).
            if (left_entries.empty()) {
                return false;
            }
            std::vector<Entry> donor(left_entries.begin(), left_entries.end() - 1);
            if (underfull(donor, false)) {
                return false;
            }
            Entry up = left_entries.back();
            Entry old_sep = parent_entries[static_cast<std::size_t>(slot)];
            old_sep.child = link;
            std::vector<Entry> trial;
            trial.push_back(std::move(old_sep));
            trial.insert(trial.end(), entries.begin(), entries.end());
            if (underfull(trial, false) || !fits(trial, false)) {
                return false;
            }
            parent_entries[static_cast<std::size_t>(slot)].key = up.key;
            parent_entries[static_cast<std::size_t>(slot)].objid = up.objid;
            left_entries.pop_back();
            entries = std::move(trial);
            const std::uint64_t new_link = up.child;
            Page left_page;
            write_node(left_page, false, level, left_link, left_entries);
            Page right_page;
            write_node(right_page, false, level, new_link, entries);
            Page parent_page;
            write_node(parent_page, false, parent_level, parent_leftmost, parent_entries);
            if (auto w = file.write(left_id, left_page); !w) {
                return std::unexpected(w.error());
            }
            if (auto w = file.write(node_id, right_page); !w) {
                return std::unexpected(w.error());
            }
            if (auto w = file.write(parent_id, parent_page); !w) {
                return std::unexpected(w.error());
            }
            return true;
        };

        if (auto borrowed = try_borrow_right(); !borrowed) {
            return std::unexpected(borrowed.error());
        } else if (*borrowed) {
            return {};
        }
        if (auto borrowed = try_borrow_left(); !borrowed) {
            return std::unexpected(borrowed.error());
        } else if (*borrowed) {
            return {};
        }

        // Merge com o irmão direito (preferido) ou esquerdo.
        PageId keep_id = node_id;
        PageId drop_id{};
        std::vector<Entry> merged = entries;
        std::uint64_t merged_link = link;
        int drop_parent_slot = -1;

        if (has_right) {
            drop_id = PageId{parent_entries[static_cast<std::size_t>(right_slot)].child};
            auto right = file.read(drop_id);
            if (!right) {
                return std::unexpected(right.error());
            }
            auto right_entries = read_all(*right);
            const std::uint64_t right_link = node_link(*right);
            if (!leaf) {
                Entry sep = parent_entries[static_cast<std::size_t>(right_slot)];
                sep.child = right_link;
                merged.push_back(std::move(sep));
            }
            merged.insert(merged.end(), right_entries.begin(), right_entries.end());
            if (leaf) {
                merged_link = right_link;  // herda o next_leaf do direito
            }
            drop_parent_slot = right_slot;
        } else if (has_left) {
            keep_id = sibling_at(*parent, slot - 1);
            drop_id = node_id;
            auto left = file.read(keep_id);
            if (!left) {
                return std::unexpected(left.error());
            }
            auto left_entries = read_all(*left);
            merged_link = node_link(*left);
            merged = std::move(left_entries);
            if (!leaf) {
                Entry sep = parent_entries[static_cast<std::size_t>(slot)];
                sep.child = link;
                merged.push_back(std::move(sep));
            }
            merged.insert(merged.end(), entries.begin(), entries.end());
            if (leaf) {
                merged_link = link;  // next_leaf do nó removido (direito no par)
            }
            drop_parent_slot = slot;
        } else {
            // Sem irmãos: é o único filho — grava e sobe (a raiz tratará).
            Page page;
            write_node(page, leaf, level, link, entries);
            if (auto w = file.write(node_id, page); !w) {
                return std::unexpected(w.error());
            }
            node_id = parent_id;
            leaf = false;
            level = parent_level;
            link = parent_leftmost;
            entries = std::move(parent_entries);
            continue;
        }

        if (!fits(merged, leaf)) {
            // Chaves variáveis impediram o merge; deixa o nó abaixo do mínimo
            // (ainda correto para find/range) em vez de corromper a página.
            Page page;
            write_node(page, leaf, level, link, entries);
            return file.write(node_id, page);
        }

        parent_entries.erase(parent_entries.begin() + drop_parent_slot);
        Page keep_page;
        write_node(keep_page, leaf, level, merged_link, merged);
        if (auto w = file.write(keep_id, keep_page); !w) {
            return std::unexpected(w.error());
        }
        if (auto orphaned = write_orphan(file, drop_id); !orphaned) {
            return orphaned;
        }

        // Continua o rebalanceamento no pai (já sem o separador removido).
        node_id = parent_id;
        leaf = false;
        level = parent_level;
        link = parent_leftmost;
        entries = std::move(parent_entries);
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
    // Desce até a folha guardando o caminho (necessário para borrow/merge).
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
        current = (idx == 0) ? PageId{node_link(*page)}
                             : PageId{cell_child(*page, slot_at(*page, idx - 1))};
    }

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
    return rebalance(*file_, root_, current, /*leaf=*/true, /*level=*/0, node_link(*leaf),
                     std::move(entries), path);
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
    // profundidade uniforme das folhas, preenchimento mínimo nas não-raiz e
    // consistência estrutural. Devolve a altura (folha = 1).
    struct Frame {
        PageId id;
        std::uint32_t depth;
        bool is_root;
    };
    std::vector<Frame> stack{{root_, 1, true}};
    std::uint32_t leaf_depth = 0;
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        auto page = file_->read(frame.id);
        if (!page) {
            return std::unexpected(page.error());
        }
        const auto count = node_key_count(*page);
        const bool leaf = node_is_leaf(*page);
        // Ordenação estrita.
        for (std::size_t i = 1; i < count; ++i) {
            const auto a = slot_at(*page, i - 1);
            const auto b = slot_at(*page, i);
            if (cmp_composite(cell_key(*page, a), cell_objid(*page, a), cell_key(*page, b),
                              cell_objid(*page, b)) >= 0) {
                return std::unexpected(Error{ErrorCode::corrupt_page, "B+ node keys out of order"});
            }
        }
        if (!frame.is_root) {
            auto entries = read_all(*page);
            if (underfull(entries, leaf)) {
                return std::unexpected(
                    Error{ErrorCode::corrupt_page, "B+ non-root node is underfull"});
            }
        }
        if (leaf) {
            if (leaf_depth == 0) {
                leaf_depth = frame.depth;
            } else if (leaf_depth != frame.depth) {
                return std::unexpected(
                    Error{ErrorCode::corrupt_page, "B+ leaves at differing depths"});
            }
            continue;
        }
        stack.push_back({PageId{node_link(*page)}, frame.depth + 1, false});
        for (std::size_t i = 0; i < count; ++i) {
            stack.push_back({PageId{cell_child(*page, slot_at(*page, i))}, frame.depth + 1, false});
        }
    }
    return leaf_depth;
}

} // namespace modb::index
