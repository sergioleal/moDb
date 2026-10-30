// Registros de qualquer tamanho (ADR-030): páginas de overflow no TableHeap.
//
// - no heap: tamanhos na borda do inline, de várias páginas e de 1 MiB, de ida e
//   volta por read, scan_records e read_page_records; update entre inline e
//   overflow; erase devolve as páginas à lista livre (o arquivo não cresce ao
//   gravar de novo); reabertura; heap antigo (sem a flag) mantém o limite;
// - no banco: objeto grande numa transação, snapshot vendo a versão anterior
//   depois de um update, GC liberando a versão velha, rollback, db check limpo.

#include "modb/object/database.hpp"
#include "modb/storage/database_check.hpp"
#include "modb/storage/page_file.hpp"
#include "modb/storage/table_heap.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

using namespace modb;
using namespace modb::storage;

namespace {

std::filesystem::path temp_path(const char* tag) {
    return std::filesystem::temp_directory_path() /
           ("modb-overflow-" + std::string{tag} + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".modb");
}

void apagar(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);
}

// Conteúdo que muda byte a byte, para pegar pedaços trocados ou deslocados.
std::vector<std::byte> padrao(std::size_t size, std::uint8_t seed) {
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((i * 31 + seed) & 0xFF);
    }
    return out;
}

bool check_limpo(const std::filesystem::path& path) {
    auto report = check_database(path);
    return report && report->ok();
}

struct Doc {
    std::string texto;
};

object::BindingBuilder<Doc> doc_binding() {
    object::BindingBuilder<Doc> b{"Doc"};
    b.field<1>("texto", &Doc::texto);
    return b;
}

} // namespace

int main() {
    TestSuite suite;
    const std::size_t inline_max = slotted_page_max_record_size - 1;  // 1 byte de tipo

    // --- TableHeap ---
    {
        const auto path = temp_path("heap");
        PageId root{};
        std::vector<std::pair<RecordId, std::vector<std::byte>>> gravados;
        {
            auto file = PageFile::create(path);
            auto heap = file ? TableHeap::create(*file) : Result<TableHeap>{std::unexpected(file.error())};
            suite.check(heap.has_value() && heap->tagged(), "um heap novo marca os registros");
            if (!heap) {
                return suite.finish();
            }
            root = heap->root_page();
            const std::vector<std::size_t> tamanhos{1,          100,        inline_max,         inline_max + 1,
                                                    page_size, 3 * page_size + 7, std::size_t{1} << 20};
            std::uint8_t seed = 1;
            for (const auto size : tamanhos) {
                auto bytes = padrao(size, seed++);
                auto id = heap->insert(bytes);
                suite.check(id.has_value(), "insere um registro de " + std::to_string(size) + " bytes");
                if (id) {
                    auto back = heap->read(*id);
                    suite.check(back && *back == bytes, "lê de volta o registro de " + std::to_string(size) + " bytes");
                    gravados.emplace_back(*id, std::move(bytes));
                }
            }
            auto todos = heap->scan_records();
            bool scan_ok = todos && todos->size() == gravados.size();
            for (std::size_t i = 0; scan_ok && i < gravados.size(); ++i) {
                bool achou = false;
                for (const auto& r : *todos) {
                    achou = achou || (r.id == gravados[i].first && r.bytes == gravados[i].second);
                }
                scan_ok = achou;
            }
            suite.check(scan_ok, "scan_records devolve os registros inteiros, inclusive os de overflow");
            std::size_t por_pagina = 0;
            for (auto page = heap->first_page(); page;) {
                auto slice = heap->read_page_records(*page);
                if (!slice) {
                    break;
                }
                for (const auto& r : slice->records) {
                    for (const auto& [id, bytes] : gravados) {
                        por_pagina += (r.id == id && r.bytes == bytes) ? 1 : 0;
                    }
                }
                page = slice->next;
            }
            suite.check(por_pagina == gravados.size(), "read_page_records devolve os registros inteiros");
            auto cadeias = heap->overflow_chains();
            suite.check(cadeias && cadeias->size() == 4, "quatro registros foram para overflow");

            // update entre inline e overflow, nos dois sentidos.
            const auto alvo = gravados[1].first;
            auto grande = padrao(5 * page_size, 99);
            auto u1 = heap->update(alvo, grande);
            auto r1 = u1 ? heap->read(*u1) : Result<std::vector<std::byte>>{std::unexpected(u1.error())};
            suite.check(r1 && *r1 == grande, "update de inline para overflow");
            auto pequeno = padrao(10, 7);
            auto u2 = u1 ? heap->update(*u1, pequeno) : Result<RecordId>{std::unexpected(u1.error())};
            auto r2 = u2 ? heap->read(*u2) : Result<std::vector<std::byte>>{std::unexpected(u2.error())};
            suite.check(r2 && *r2 == pequeno, "update de overflow de volta para inline");
            if (u2) {
                gravados[1] = {*u2, pequeno};
            }

            // erase devolve a cadeia à lista livre: gravar de novo não cresce o arquivo.
            const auto megabyte = gravados.back();
            suite.check(heap->erase(megabyte.first).has_value(), "apaga o registro de 1 MiB");
            gravados.pop_back();
            const auto paginas = file->page_count();
            auto de_novo = heap->insert(padrao(std::size_t{1} << 20, 42));
            suite.check(de_novo.has_value() && file->page_count() == paginas,
                        "regravar 1 MiB reusa as páginas liberadas, sem crescer o arquivo");
            if (de_novo) {
                gravados.emplace_back(*de_novo, padrao(std::size_t{1} << 20, 42));
            }
            suite.check(file->flush().has_value(), "o heap é gravado no disco");
        }
        suite.check(check_limpo(path), "db check limpo num arquivo com overflow");
        {
            auto file = PageFile::open(path);
            auto heap = file ? TableHeap::open(*file, root) : Result<TableHeap>{std::unexpected(file.error())};
            bool iguais = heap.has_value() && heap->tagged();
            for (const auto& [id, bytes] : gravados) {
                auto back = heap ? heap->read(id) : Result<std::vector<std::byte>>{std::unexpected(Error{})};
                iguais = iguais && back && *back == bytes;
            }
            suite.check(iguais, "depois de reabrir, todos os registros voltam iguais");
        }
        apagar(path);
    }

    // --- heap de antes da ADR-030 (raiz sem a flag) ---
    {
        const auto path = temp_path("legacy");
        {
            auto file = PageFile::create(path);
            auto heap = file ? TableHeap::create(*file) : Result<TableHeap>{std::unexpected(file.error())};
            if (heap) {
                // Desliga a flag de registros marcados (bytes 6-7 da raiz THRP).
                auto page = file->read(heap->root_page());
                if (page) {
                    page->bytes()[6] = std::byte{0};
                    page->bytes()[7] = std::byte{0};
                    (void)file->write(heap->root_page(), *page);
                }
                auto reopened = TableHeap::open(*file, heap->root_page());
                suite.check(reopened && !reopened->tagged(), "uma raiz sem a flag abre como heap antigo");
                if (reopened) {
                    auto small = reopened->insert(padrao(50, 3));
                    auto back = small ? reopened->read(*small) : Result<std::vector<std::byte>>{std::unexpected(small.error())};
                    suite.check(back && *back == padrao(50, 3), "o heap antigo grava e lê registros pequenos");
                    auto big = reopened->insert(padrao(page_size, 4));
                    suite.check(!big && big.error().code == ErrorCode::record_too_large,
                                "o heap antigo mantém o limite de uma página");
                }
            }
        }
        apagar(path);
    }

    // --- banco de objetos: transação, snapshot, GC, rollback ---
    {
        const auto path = temp_path("db");
        auto created = object::Database::create(path);
        auto db = created ? std::make_shared<object::Database>(std::move(*created)) : nullptr;
        auto attached = db ? object::DatabaseRegistry::instance().attach(db) : Result<object::DatabaseId>{};
        suite.check(db && attached && db->bind(doc_binding()).has_value(), "banco com o tipo Doc");
        if (db && attached) {
            const std::string v1(100 * 1024, 'a');
            const std::string v2(200 * 1024, 'b');
            object::ObjectId id{};
            {
                auto tx = db->begin();
                auto doc = tx ? db->create(*tx, Doc{v1}) : Result<object::Handle<Doc>>{std::unexpected(tx.error())};
                suite.check(doc && tx->commit(), "cria um objeto de 100 KB");
                if (doc) {
                    id = doc->id();
                }
            }
            std::optional<object::Snapshot> antes;
            if (auto aberto = db->snapshot()) {
                antes.emplace(std::move(*aberto));
            }
            {
                auto tx = db->begin();
                auto handle = db->get<Doc>(id);
                suite.check(handle && db->update(*tx, *handle, Doc{v2}) && tx->commit(), "troca por 200 KB");
            }
            auto atual = db->get<Doc>(id);
            auto agora = atual ? db->materialize(*atual) : Result<Doc>{std::unexpected(atual.error())};
            suite.check(agora && agora->texto == v2, "a leitura atual vê os 200 KB");
            if (antes) {
                auto velho = db->get<Doc>(id, *antes);
                suite.check(velho && velho->texto == v1, "o snapshot de antes ainda vê os 100 KB");
            }
            {
                auto tx = db->begin();
                auto doc = tx ? db->create(*tx, Doc{std::string(300 * 1024, 'r')})
                              : Result<object::Handle<Doc>>{std::unexpected(tx.error())};
                suite.check(doc.has_value(), "um objeto de 300 KB entra numa transação");
                if (tx) {
                    (void)tx->rollback();
                }
            }
            std::size_t vistos = 0;
            for (auto& row : db->query<Doc>().stream()) {
                vistos += row ? 1 : 0;
            }
            suite.check(vistos == 1, "depois do rollback, só o objeto confirmado existe");
            antes.reset();
            auto coletados = db->collect_garbage();
            suite.check(coletados && *coletados >= 1, "o GC libera a versão de 100 KB");
            object::DatabaseRegistry::instance().detach(*attached);
        }
        db.reset();
        suite.check(check_limpo(path), "db check limpo depois de update, rollback e GC com overflow");
        apagar(path);
    }

    return suite.finish();
}
