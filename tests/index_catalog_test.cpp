// Valida o diretório de índices multipágina (Fase 7B): empacotamento em cadeia
// IXDR, reabertura e atualização de raiz preservando todos os registros.
#include "modb/object/index_catalog.hpp"
#include "modb/storage/endian.hpp"
#include "modb/storage/page_file.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace modb;
using namespace modb::object;
using modb::storage::PageFile;
using modb::storage::PageId;

namespace {

class TemporaryFile {
public:
    explicit TemporaryFile(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-ixdr-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

int main() {
    TestSuite suite;
    TemporaryFile temp{"multi"};

    auto file = PageFile::create(temp.path());
    suite.check(file.has_value(), "page file is created");
    if (!file) {
        return suite.finish();
    }

    auto catalog = IndexCatalog::create(*file);
    suite.check(catalog.has_value(), "empty index directory is created");
    if (!catalog) {
        return suite.finish();
    }
    const PageId directory = catalog->directory();

    // Nomes longos forçam várias páginas IXDR (cada entrada ~2+nome+2+8 bytes).
    constexpr int total = 80;
    const std::string prefix(60, 'T');
    bool added = true;
    for (int i = 0; i < total; ++i) {
        IndexInfo info;
        info.type_name = prefix + std::to_string(i);
        info.field_id = static_cast<std::uint16_t>(i + 1);
        info.root = PageId{static_cast<std::uint64_t>(1000 + i)};
        added = added && catalog->add(std::move(info)).has_value();
    }
    suite.check(added, "many long-named indexes are added across directory pages");
    suite.check(catalog->indexes().size() == static_cast<std::size_t>(total),
                "in-memory catalog lists every index");

    // Confirma que a primeira página aponta para uma continuação (multipágina).
    {
        auto page = file->read(directory);
        suite.check(page.has_value(), "directory head page is readable");
        if (page) {
            const auto next = modb::storage::load_le<std::uint64_t>(page->bytes().subspan(8, 8));
            suite.check(next != 0, "directory overflows onto a second page");
        }
    }

    suite.check(catalog->set_root(0, PageId{4242}).has_value(), "set_root persists on a multipage directory");

    // Reabre a partir do DBRT-equivalent (só o PageId da cabeça).
    auto reopened = IndexCatalog::open(*file, directory);
    suite.check(reopened.has_value(), "multipage directory reopens");
    if (!reopened) {
        return suite.finish();
    }
    suite.check(reopened->indexes().size() == static_cast<std::size_t>(total),
                "reopened catalog retains every index entry");
    suite.check(reopened->indexes().front().root.value == 4242,
                "updated root survives reopen across directory pages");
    suite.check(reopened->indexes().back().field_id == static_cast<std::uint16_t>(total),
                "last overflow-page entry survives reopen");
    suite.check(reopened->find(prefix + "10", 11) == 10, "find resolves an entry on a later page");

    return suite.finish();
}
