// Valida a B+ tree persistente da Fase 7B isoladamente: inserção ordenada e
// embaralhada, invariantes estruturais (ordem, profundidade uniforme), chaves
// duplicadas por valor, faixa inclusiva/vazia, remoção, reabertura e ordenação
// correta por tipo via o key_codec ordenável.
#include "modb/index/btree.hpp"
#include "modb/index/key_codec.hpp"
#include "modb/object/attribute_value.hpp"
#include "modb/storage/page_file.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <system_error>
#include <vector>

using namespace modb;
using namespace modb::index;
using modb::object::AttributeValue;
using modb::storage::PageFile;

namespace {

class TemporaryFile {
public:
    explicit TemporaryFile(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-btree-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::vector<std::byte> int_key(std::int64_t value) {
    auto encoded = encode_key(AttributeValue{value});
    return encoded ? *encoded : std::vector<std::byte>{};
}

std::vector<std::byte> str_key(const std::string& value) {
    auto encoded = encode_key(AttributeValue{value});
    return encoded ? *encoded : std::vector<std::byte>{};
}

} // namespace

int main() {
    TestSuite suite;

    // === A. int64: inserção embaralhada, busca de todas, invariantes, faixa ===
    {
        TemporaryFile temp{"int"};
        auto file = PageFile::create(temp.path());
        suite.check(file.has_value(), "A: page file created");
        if (!file) {
            return suite.finish();
        }
        auto tree = BTree::create(*file);
        suite.check(tree.has_value(), "A: empty tree created");
        if (!tree) {
            return suite.finish();
        }

        constexpr int total = 2000;
        std::vector<int> order(total);
        std::iota(order.begin(), order.end(), 0);
        std::mt19937 rng{12345};
        std::shuffle(order.begin(), order.end(), rng);

        bool all_inserted = true;
        for (int v : order) {
            if (!tree->insert(int_key(v), static_cast<std::uint64_t>(v) + 1)) {
                all_inserted = false;
                break;
            }
        }
        suite.check(all_inserted, "A: 2000 shuffled keys inserted");

        auto height = tree->validate();
        suite.check(height.has_value(), "A: structure is valid (ordered, uniform depth)");
        suite.check(height.has_value() && *height >= 2, "A: the tree grew past a single leaf");

        bool all_found = true;
        for (int v = 0; v < total; ++v) {
            auto hits = tree->find(int_key(v));
            if (!hits || hits->size() != 1 || hits->front() != static_cast<std::uint64_t>(v) + 1) {
                all_found = false;
                break;
            }
        }
        suite.check(all_found, "A: every key is found with its object id");

        // Faixa [100, 199] inclusive -> exatamente 100 ids, em ordem crescente.
        auto ranged = tree->range(int_key(100), int_key(199));
        bool range_ok = ranged.has_value() && ranged->size() == 100;
        if (range_ok) {
            for (std::size_t i = 0; i < ranged->size(); ++i) {
                if ((*ranged)[i] != static_cast<std::uint64_t>(100 + i) + 1) {
                    range_ok = false;
                    break;
                }
            }
        }
        suite.check(range_ok, "A: an inclusive range returns exactly the keys in order");

        // Faixa vazia (nenhuma chave em [5000, 6000]).
        auto empty = tree->range(int_key(5000), int_key(6000));
        suite.check(empty.has_value() && empty->empty(), "A: an out-of-range window is empty");

        // Chave inexistente.
        auto missing = tree->find(int_key(999999));
        suite.check(missing.has_value() && missing->empty(), "A: a missing key returns no hits");
    }

    // === B. chaves grandes (string) forçam split interno -> altura >= 3 ===
    {
        TemporaryFile temp{"str"};
        auto file = PageFile::create(temp.path());
        auto tree = file ? BTree::create(*file)
                         : Result<BTree>{std::unexpected(Error{ErrorCode::io_error, "no file"})};
        suite.check(file.has_value() && tree.has_value(), "B: tree over long string keys created");
        if (!file || !tree) {
            return suite.finish();
        }
        constexpr int total = 3000;
        const std::string pad(180, 'x');  // chaves grandes: poucas por página
        bool ok = true;
        for (int i = 0; i < total; ++i) {
            // Prefixo zero-padded mantém a ordem lexicográfica = ordem numérica.
            std::string key = pad + "-" + std::string(6 - std::to_string(i).size(), '0') +
                              std::to_string(i);
            if (!tree->insert(str_key(key), static_cast<std::uint64_t>(i) + 1)) {
                ok = false;
                break;
            }
        }
        suite.check(ok, "B: 3000 long keys inserted");
        auto height = tree->validate();
        suite.check(height.has_value(), "B: structure valid with internal splits");
        suite.check(height.has_value() && *height >= 3,
                    "B: long keys forced internal node splits (height >= 3)");
        // Amostra de buscas.
        bool found = true;
        for (int i = 0; i < total; i += 137) {
            std::string key = pad + "-" + std::string(6 - std::to_string(i).size(), '0') +
                              std::to_string(i);
            auto hits = tree->find(str_key(key));
            if (!hits || hits->size() != 1 || hits->front() != static_cast<std::uint64_t>(i) + 1) {
                found = false;
                break;
            }
        }
        suite.check(found, "B: sampled long keys are all found");
    }

    // === C. duplicatas por valor: mesma chave, ObjectIds distintos ===
    {
        TemporaryFile temp{"dup"};
        auto file = PageFile::create(temp.path());
        auto tree = file ? BTree::create(*file)
                         : Result<BTree>{std::unexpected(Error{ErrorCode::io_error, "no file"})};
        suite.check(file.has_value() && tree.has_value(), "C: tree created");
        if (!file || !tree) {
            return suite.finish();
        }
        // 50 objetos com o MESMO valor de chave (42), ids 10..59.
        bool ok = true;
        for (std::uint64_t id = 10; id < 60; ++id) {
            ok = ok && tree->insert(int_key(42), id).has_value();
        }
        // Alguns vizinhos para provar que a faixa não vaza.
        ok = ok && tree->insert(int_key(41), 5).has_value();
        ok = ok && tree->insert(int_key(43), 100).has_value();
        suite.check(ok, "C: 50 duplicate-value keys plus neighbours inserted");

        auto hits = tree->find(int_key(42));
        bool hits_ok = hits.has_value() && hits->size() == 50;
        if (hits_ok) {
            for (std::size_t i = 0; i < hits->size(); ++i) {
                if ((*hits)[i] != 10 + i) {  // devolvidos em ordem de ObjectId
                    hits_ok = false;
                    break;
                }
            }
        }
        suite.check(hits_ok, "C: find returns all 50 ids for the duplicated value, in id order");

        // Chave composta exata duplicada é rejeitada.
        suite.check_error(tree->insert(int_key(42), 10), ErrorCode::invalid_argument,
                          "C: an exact duplicate (value + id) is rejected");
    }

    // === D. remoção: busca falha depois, vizinhos preservados, estrutura ok ===
    {
        TemporaryFile temp{"rm"};
        auto file = PageFile::create(temp.path());
        auto tree = file ? BTree::create(*file)
                         : Result<BTree>{std::unexpected(Error{ErrorCode::io_error, "no file"})};
        suite.check(file.has_value() && tree.has_value(), "D: tree created");
        if (!file || !tree) {
            return suite.finish();
        }
        for (int v = 0; v < 500; ++v) {
            static_cast<void>(tree->insert(int_key(v), static_cast<std::uint64_t>(v) + 1));
        }
        // Remove os pares.
        bool removed = true;
        for (int v = 0; v < 500; v += 2) {
            removed = removed && tree->remove(int_key(v), static_cast<std::uint64_t>(v) + 1).has_value();
        }
        suite.check(removed, "D: even keys removed");
        suite.check_error(tree->remove(int_key(0), 1), ErrorCode::record_not_found,
                          "D: removing an absent key fails cleanly");
        suite.check_error(tree->remove(int_key(100000), 1), ErrorCode::record_not_found,
                          "D: removing a key past the largest one fails cleanly");
        auto gone = tree->find(int_key(2));
        auto kept = tree->find(int_key(3));
        suite.check(gone.has_value() && gone->empty(), "D: a removed key is no longer found");
        suite.check(kept.has_value() && kept->size() == 1 && kept->front() == 4,
                    "D: a neighbour of a removed key survives");
        suite.check(tree->validate().has_value(),
                    "D: structure stays ordered and at minimum fill after removals");
    }

    // === D2. remoção em massa: borrow/merge + encolhimento da raiz ===
    {
        TemporaryFile temp{"rm-mass"};
        auto file = PageFile::create(temp.path());
        auto tree = file ? BTree::create(*file)
                         : Result<BTree>{std::unexpected(Error{ErrorCode::io_error, "no file"})};
        suite.check(file.has_value() && tree.has_value(), "D2: tree created");
        if (!file || !tree) {
            return suite.finish();
        }
        constexpr int total = 3000;
        for (int v = 0; v < total; ++v) {
            static_cast<void>(tree->insert(int_key(v), static_cast<std::uint64_t>(v) + 1));
        }
        auto tall = tree->validate();
        suite.check(tall.has_value() && *tall >= 2, "D2: tree is taller than a single leaf");
        const auto tall_root = tree->root_page();
        // Remove quase tudo, deixando 3 chaves — deve mergear folhas e encolher a raiz.
        bool removed = true;
        for (int v = 0; v < total - 3; ++v) {
            removed =
                removed && tree->remove(int_key(v), static_cast<std::uint64_t>(v) + 1).has_value();
        }
        suite.check(removed, "D2: bulk removal succeeds");
        auto short_height = tree->validate();
        suite.check(short_height.has_value(),
                    "D2: tree remains valid (min fill) after bulk removal");
        suite.check(short_height.has_value() && *short_height == 1,
                    "D2: root shrinks back to a single leaf");
        suite.check(tree->root_page().value != tall_root.value,
                    "D2: root page identity changes when height shrinks");
        for (int v = total - 3; v < total; ++v) {
            auto hits = tree->find(int_key(v));
            suite.check(hits.has_value() && hits->size() == 1 &&
                            hits->front() == static_cast<std::uint64_t>(v) + 1,
                        "D2: surviving keys remain findable");
        }
    }

    // === E. reabertura: árvore íntegra após fechar e reabrir o arquivo ===
    {
        TemporaryFile temp{"reopen"};
        std::uint64_t root_value = 0;
        constexpr int total = 1500;
        {
            auto file = PageFile::create(temp.path());
            suite.check(file.has_value(), "E: file created");
            if (!file) {
                return suite.finish();
            }
            auto tree = BTree::create(*file);
            if (!tree) {
                return suite.finish();
            }
            for (int v = 0; v < total; ++v) {
                static_cast<void>(tree->insert(int_key(v * 3), static_cast<std::uint64_t>(v) + 1));
            }
            root_value = tree->root_page().value;
            suite.check(file->flush().has_value(), "E: changes flushed");
        }
        {
            auto file = PageFile::open(temp.path());
            suite.check(file.has_value(), "E: file reopened");
            if (!file) {
                return suite.finish();
            }
            auto tree = BTree::open(*file, modb::storage::PageId{root_value});
            suite.check(tree.has_value(), "E: tree reopened from persisted root");
            if (tree) {
                suite.check(tree->validate().has_value(), "E: reopened structure is valid");
                bool all = true;
                for (int v = 0; v < total; ++v) {
                    auto hits = tree->find(int_key(v * 3));
                    if (!hits || hits->size() != 1 ||
                        hits->front() != static_cast<std::uint64_t>(v) + 1) {
                        all = false;
                        break;
                    }
                }
                suite.check(all, "E: every key survives closing and reopening");
            }
        }
    }

    // === F. ordenação por tipo: negativos, positivos, floats, strings ===
    {
        TemporaryFile temp{"order"};
        auto file = PageFile::create(temp.path());
        auto tree = file ? BTree::create(*file)
                         : Result<BTree>{std::unexpected(Error{ErrorCode::io_error, "no file"})};
        suite.check(file.has_value() && tree.has_value(), "F: tree created");
        if (!file || !tree) {
            return suite.finish();
        }
        // int64 incluindo negativos: -5, -1, 0, 3, 10. A faixa completa deve
        // devolvê-los em ordem numérica, provando o encoding com bias de sinal.
        const std::vector<std::int64_t> values{10, -1, 3, -5, 0};
        for (std::size_t i = 0; i < values.size(); ++i) {
            static_cast<void>(tree->insert(int_key(values[i]), i + 1));
        }
        auto ordered = tree->range(int_key(-100), int_key(100));
        // Ordem numérica: -5(id4), -1(id2), 0(id5), 3(id3), 10(id1).
        const std::vector<std::uint64_t> expected{4, 2, 5, 3, 1};
        suite.check(ordered.has_value() && *ordered == expected,
                    "F: signed integers come back in numeric order (sign-biased encoding)");
    }

    // === G. ordenação de floats (inclui negativos) ===
    {
        TemporaryFile temp{"float"};
        auto file = PageFile::create(temp.path());
        auto tree = file ? BTree::create(*file)
                         : Result<BTree>{std::unexpected(Error{ErrorCode::io_error, "no file"})};
        suite.check(file.has_value() && tree.has_value(), "G: tree created");
        if (!file || !tree) {
            return suite.finish();
        }
        const std::vector<double> values{2.5, -3.0, 0.0, -0.5, 1.0};
        for (std::size_t i = 0; i < values.size(); ++i) {
            static_cast<void>(tree->insert(*encode_key(AttributeValue{values[i]}), i + 1));
        }
        auto lo = encode_key(AttributeValue{-1000.0});
        auto hi = encode_key(AttributeValue{1000.0});
        auto ordered = tree->range(*lo, *hi);
        // Ordem: -3.0(id2), -0.5(id4), 0.0(id3), 1.0(id5), 2.5(id1).
        const std::vector<std::uint64_t> expected{2, 4, 3, 5, 1};
        suite.check(ordered.has_value() && *ordered == expected,
                    "G: doubles come back in numeric order (sign-flip encoding)");
    }

    // === H. remoção pelas duas pontas: sobe o rebalanceamento a nós internos e
    // exercita o irmão ESQUERDO. D2 só drena pela esquerda (remove sempre a
    // menor chave), então o nó que fica abaixo do mínimo é sempre o filho mais
    // à esquerda do seu nível — nunca tem irmão esquerdo (`has_left` nunca é
    // verdadeiro, `try_borrow_left`/merge-com-esquerda nunca rodam). Drenando
    // as duas pontas em direção ao meio, o lado direito também esvazia e força
    // borrow/merge com o irmão esquerdo — e, com uma árvore de 3+ níveis, o
    // encolhimento cascateia até nós internos (não só a raiz).
    {
        TemporaryFile temp{"rm-both-ends"};
        auto file = PageFile::create(temp.path());
        auto tree = file ? BTree::create(*file)
                         : Result<BTree>{std::unexpected(Error{ErrorCode::io_error, "no file"})};
        suite.check(file.has_value() && tree.has_value(), "H: tree created");
        if (!file || !tree) {
            return suite.finish();
        }
        constexpr int total = 4000;
        const std::string pad(180, 'x');  // chaves grandes: poucas por página, como no teste B.
        auto padded_key = [&](int i) {
            return pad + "-" + std::string(6 - std::to_string(i).size(), '0') + std::to_string(i);
        };
        bool inserted = true;
        for (int i = 0; i < total; ++i) {
            inserted = inserted &&
                      tree->insert(str_key(padded_key(i)), static_cast<std::uint64_t>(i) + 1).has_value();
        }
        suite.check(inserted, "H: 4000 long keys inserted");
        auto tall = tree->validate();
        suite.check(tall.has_value() && *tall >= 3, "H: tree grew at least 3 levels deep");

        // Drena as duas pontas em direção ao meio, deixando uma janela viva.
        constexpr int keep_lo = total / 2 - 4;
        constexpr int keep_hi = total / 2 + 4;
        bool removed = true;
        for (int i = 0; i < keep_lo; ++i) {
            removed = removed &&
                     tree->remove(str_key(padded_key(i)), static_cast<std::uint64_t>(i) + 1).has_value();
        }
        for (int i = total - 1; i >= keep_hi; --i) {
            removed = removed &&
                     tree->remove(str_key(padded_key(i)), static_cast<std::uint64_t>(i) + 1).has_value();
        }
        suite.check(removed, "H: draining from both ends toward the middle succeeds");

        auto shrunk = tree->validate();
        suite.check(shrunk.has_value(),
                    "H: structure stays valid (min fill) while shrinking from both sides");

        bool survivors_found = true;
        for (int i = keep_lo; i < keep_hi; ++i) {
            auto hits = tree->find(str_key(padded_key(i)));
            if (!hits || hits->size() != 1 || hits->front() != static_cast<std::uint64_t>(i) + 1) {
                survivors_found = false;
                break;
            }
        }
        suite.check(survivors_found, "H: the surviving middle window remains findable");

        auto gone_lo = tree->find(str_key(padded_key(0)));
        auto gone_hi = tree->find(str_key(padded_key(total - 1)));
        suite.check(gone_lo.has_value() && gone_lo->empty() && gone_hi.has_value() && gone_hi->empty(),
                    "H: keys drained from either end are no longer found");
    }

    // === I. key_codec: cobre boolean/ref/blob (tipos indexáveis ainda não
    // exercitados por nenhum teste) e o erro para os tipos não indexáveis
    // (null/bytes/embedded). Cada tipo usa sua PRÓPRIA árvore: ref e blob
    // compartilham o mesmo encoding big-endian de u64, então misturar os dois
    // na mesma árvore faria os intervalos numéricos se sobreporem.
    {
        // boolean: false ordena antes de true.
        TemporaryFile temp_bool{"key-codec-bool"};
        auto file_bool = PageFile::create(temp_bool.path());
        auto tree_bool = file_bool
                            ? BTree::create(*file_bool)
                            : Result<BTree>{std::unexpected(Error{ErrorCode::io_error, "no file"})};
        suite.check(file_bool.has_value() && tree_bool.has_value(), "I: boolean tree created");
        auto enc_false = encode_key(AttributeValue{false});
        auto enc_true = encode_key(AttributeValue{true});
        suite.check(enc_false.has_value() && enc_true.has_value(), "I: boolean values encode");
        if (tree_bool && enc_false && enc_true) {
            static_cast<void>(tree_bool->insert(*enc_true, 1));
            static_cast<void>(tree_bool->insert(*enc_false, 2));
            auto ordered = tree_bool->range(*enc_false, *enc_true);
            const std::vector<std::uint64_t> expected{2, 1};
            suite.check(ordered.has_value() && *ordered == expected, "I: false sorts before true");
        }

        // ref: ObjectId ordena numericamente (big-endian sem sinal).
        TemporaryFile temp_ref{"key-codec-ref"};
        auto file_ref = PageFile::create(temp_ref.path());
        auto tree_ref = file_ref
                            ? BTree::create(*file_ref)
                            : Result<BTree>{std::unexpected(Error{ErrorCode::io_error, "no file"})};
        suite.check(file_ref.has_value() && tree_ref.has_value(), "I: ref tree created");
        auto enc_ref_lo = encode_key(AttributeValue{modb::object::ObjectId{5}});
        auto enc_ref_hi = encode_key(AttributeValue{modb::object::ObjectId{500}});
        suite.check(enc_ref_lo.has_value() && enc_ref_hi.has_value(), "I: ref values encode");
        if (tree_ref && enc_ref_lo && enc_ref_hi) {
            static_cast<void>(tree_ref->insert(*enc_ref_hi, 10));
            static_cast<void>(tree_ref->insert(*enc_ref_lo, 20));
            auto ordered = tree_ref->range(*enc_ref_lo, *enc_ref_hi);
            const std::vector<std::uint64_t> expected{20, 10};
            suite.check(ordered.has_value() && *ordered == expected,
                        "I: ref (ObjectId) values sort numerically");
        }

        // blob: BlobId usa o mesmo encoding big-endian do ref, em árvore própria.
        TemporaryFile temp_blob{"key-codec-blob"};
        auto file_blob = PageFile::create(temp_blob.path());
        auto tree_blob = file_blob
                             ? BTree::create(*file_blob)
                             : Result<BTree>{std::unexpected(Error{ErrorCode::io_error, "no file"})};
        suite.check(file_blob.has_value() && tree_blob.has_value(), "I: blob tree created");
        auto enc_blob_lo = encode_key(AttributeValue{modb::object::BlobId{7}});
        auto enc_blob_hi = encode_key(AttributeValue{modb::object::BlobId{700}});
        suite.check(enc_blob_lo.has_value() && enc_blob_hi.has_value(), "I: blob values encode");
        if (tree_blob && enc_blob_lo && enc_blob_hi) {
            static_cast<void>(tree_blob->insert(*enc_blob_hi, 30));
            static_cast<void>(tree_blob->insert(*enc_blob_lo, 40));
            auto ordered = tree_blob->range(*enc_blob_lo, *enc_blob_hi);
            const std::vector<std::uint64_t> expected{40, 30};
            suite.check(ordered.has_value() && *ordered == expected, "I: blob values sort numerically");
        }

        // Tipos não indexáveis devolvem o erro dedicado, não um valor qualquer.
        suite.check_error(encode_key(AttributeValue{modb::object::AttributeNull{}}),
                          ErrorCode::invalid_argument, "I: null is rejected as a non-indexable type");
        suite.check_error(
            encode_key(AttributeValue{std::vector<std::byte>{std::byte{1}, std::byte{2}}}),
            ErrorCode::invalid_argument, "I: bytes is rejected as a non-indexable type");
        suite.check_error(encode_key(AttributeValue{modb::object::EmbeddedValue{}}),
                          ErrorCode::invalid_argument,
                          "I: embedded is rejected as a non-indexable type");
    }

    return suite.finish();
}
