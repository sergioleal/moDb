// Importa o BlobStore exercitado aqui.
#include "modb/object/blob_store.hpp"
#include "modb/object/database.hpp"
// Importa PageFile para criar o arquivo de teste.
#include "modb/storage/page_file.hpp"
// Importa as verificações mínimas.
#include "test_support.hpp"

// Disponibiliza o relógio do nome único.
#include <chrono>
// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza caminhos temporários.
#include <filesystem>
// Disponibiliza shared_ptr para anexar o Database ao registro.
#include <memory>
// Disponibiliza std::error_code na limpeza.
#include <system_error>
// Disponibiliza std::vector.
#include <vector>

using namespace modb;
using namespace modb::object;

namespace {

class TemporaryFile {
public:
    explicit TemporaryFile(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-blob-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Gera um padrão determinístico de n bytes para comparar round-trips.
std::vector<std::byte> pattern(std::size_t n) {
    std::vector<std::byte> data;
    data.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        data.push_back(static_cast<std::byte>((i * 31 + 7) & 0xFF));
    }
    return data;
}

} // namespace

int main() {
    TestSuite suite;

    // --- BlobStore exposto por Database exige a transação do banco ---
    {
        TemporaryFile file{"database-owner"};
        auto created = Database::create(file.path());
        suite.check(created.has_value(), "database for owned blob store is created");
        if (!created) {
            return suite.finish();
        }
        auto database = std::make_shared<Database>(std::move(*created));
        auto database_id = DatabaseRegistry::instance().attach(database);
        suite.check(database_id.has_value(), "database for owned blob store is attached");
        if (!database_id) {
            return suite.finish();
        }
        auto blobs = database->blobs();
        const auto data = pattern(100);
        suite.check_error(blobs.create(data), ErrorCode::transaction_required,
                          "database blob store rejects direct writes without a transaction");
        auto transaction = database->begin();
        suite.check(transaction.has_value(), "owned blob transaction begins");
        if (transaction) {
            suite.check(blobs.create(data).has_value(),
                        "database blob store accepts a write during its transaction");
            suite.check(transaction->commit().has_value(), "owned blob transaction commits");
        }
        DatabaseRegistry::instance().detach(*database_id);
    }

    // --- round-trip pequeno (< 1 página) ---
    {
        TemporaryFile file{"small"};
        auto page_file = storage::PageFile::create(file.path());
        suite.check(page_file.has_value(), "page file is created");
        if (!page_file) {
            return suite.finish();
        }
        BlobStore blobs{*page_file};
        const auto data = pattern(100);
        auto id = blobs.create(data);
        suite.check(id.has_value() && id->value != 0, "small blob is created");
        if (id) {
            auto read = blobs.read(*id);
            suite.check(read.has_value() && *read == data, "small blob round-trips");
        }
    }

    // --- round-trip grande (100 KiB, várias páginas) byte a byte ---
    {
        TemporaryFile file{"large"};
        const auto data = pattern(100 * 1024);
        BlobId id{};
        // Escopo interno: fecha o arquivo antes de reabrir (o SO não permite
        // duas aberturas simultâneas do mesmo caminho).
        {
            auto page_file = storage::PageFile::create(file.path());
            if (!page_file) {
                return suite.finish();
            }
            BlobStore blobs{*page_file};
            auto created = blobs.create(data);
            suite.check(created.has_value(), "large blob is created");
            if (!created) {
                return suite.finish();
            }
            id = *created;
            auto read = blobs.read(id);
            suite.check(read.has_value() && *read == data, "large blob round-trips byte for byte");

            // read_chunks concatena para o mesmo conteúdo.
            std::vector<std::byte> concatenated;
            auto chunked = blobs.read_chunks(
                id, [&](std::span<const std::byte> chunk) -> Result<void> {
                    concatenated.insert(concatenated.end(), chunk.begin(), chunk.end());
                    return {};
                });
            suite.check(chunked.has_value() && concatenated == data,
                        "read_chunks concatenation equals the content");
            suite.check(page_file->flush().has_value(), "large blob is flushed");
        }

        // Sobrevive à reabertura do arquivo.
        auto reopened = storage::PageFile::open(file.path());
        suite.check(reopened.has_value(), "page file reopens");
        if (reopened) {
            BlobStore reopened_blobs{*reopened};
            auto read = reopened_blobs.read(id);
            suite.check(read.has_value() && *read == data, "large blob survives reopening");
        }
    }

    // --- rewrite maior e menor, mantendo BlobId estável ---
    {
        TemporaryFile file{"rewrite"};
        auto page_file = storage::PageFile::create(file.path());
        if (!page_file) {
            return suite.finish();
        }
        BlobStore blobs{*page_file};
        const auto original = pattern(5000); // ~2 páginas
        auto id = blobs.create(original);
        suite.check(id.has_value(), "rewrite base blob is created");
        if (id) {
            const auto larger = pattern(20000); // ~5 páginas
            auto grown = blobs.rewrite(*id, larger);
            suite.check(grown.has_value() && *grown == *id, "rewrite grows and keeps BlobId");
            auto read_larger = blobs.read(*id);
            suite.check(read_larger.has_value() && *read_larger == larger,
                        "grown blob reads the new content");

            const auto smaller = pattern(300); // < 1 página
            auto shrunk = blobs.rewrite(*id, smaller);
            suite.check(shrunk.has_value() && *shrunk == *id, "rewrite shrinks and keeps BlobId");
            auto read_smaller = blobs.read(*id);
            suite.check(read_smaller.has_value() && *read_smaller == smaller,
                        "shrunk blob reads the new content");
        }
    }

    // --- ciclo na cadeia detectado ---
    {
        TemporaryFile file{"cycle"};
        auto page_file = storage::PageFile::create(file.path());
        if (!page_file) {
            return suite.finish();
        }
        BlobStore blobs{*page_file};
        const auto data = pattern(9000); // >= 3 páginas
        auto id = blobs.create(data);
        suite.check(id.has_value(), "cycle base blob is created");
        if (id) {
            // Corrompe o next da primeira página para apontar de volta a ela mesma.
            auto first = page_file->read(storage::PageId{id->value});
            suite.check(first.has_value(), "first blob page is read");
            if (first) {
                // Escreve o próprio id no campo next (offset 8 do cabeçalho).
                const std::uint64_t self = id->value;
                for (std::size_t b = 0; b < 8; ++b) {
                    (*first)[8 + b] = static_cast<std::byte>((self >> (8 * b)) & 0xFF);
                }
                suite.check(page_file->write(storage::PageId{id->value}, *first).has_value(),
                            "corrupted next is written");
                auto read = blobs.read(*id);
                suite.check_error(read, ErrorCode::page_chain_cycle,
                                  "a cyclic chain is rejected");
            }
        }
    }

    // --- comprimento corrompido rejeitado ---
    {
        TemporaryFile file{"length"};
        auto page_file = storage::PageFile::create(file.path());
        if (!page_file) {
            return suite.finish();
        }
        BlobStore blobs{*page_file};
        auto id = blobs.create(pattern(50));
        suite.check(id.has_value(), "length base blob is created");
        if (id) {
            auto page = page_file->read(storage::PageId{id->value});
            if (page) {
                // payload_length está no offset 16; grava um valor impossível.
                const std::uint32_t huge = 0xFFFFFFFF;
                for (std::size_t b = 0; b < 4; ++b) {
                    (*page)[16 + b] = static_cast<std::byte>((huge >> (8 * b)) & 0xFF);
                }
                suite.check(page_file->write(storage::PageId{id->value}, *page).has_value(),
                            "corrupted length is written");
                auto read = blobs.read(*id);
                suite.check_error(read, ErrorCode::corrupt_page,
                                  "an impossible payload length is rejected");
            }
        }
    }

    return suite.finish();
}
