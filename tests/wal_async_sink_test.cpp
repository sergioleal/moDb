#include "modb/object/binding.hpp"
#include "modb/object/database.hpp"
#include "modb/tx/wal.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <system_error>

using namespace modb;
using namespace modb::object;

namespace {

struct Emp {
    std::string name;
    std::int64_t salary{0};
};

BindingBuilder<Emp> emp_builder() {
    BindingBuilder<Emp> builder{"Employee"};
    builder.field<1>("name", &Emp::name);
    builder.field<2>("salary", &Emp::salary);
    return builder;
}

class TempPath {
public:
    explicit TempPath(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-13-4-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TempPath() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + ".wal", ignored);
        std::filesystem::remove(path_.string() + ".scratch", ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

int main() {
    TestSuite suite;
    TempPath path{"async-wal"};

    DatabaseOptions opts;
    opts.wal_io = WalIoMode::async;

    ObjectId first_id{};
    std::uint64_t last_checkpoint = 0;
    {
        auto created = Database::create(path.path(), opts);
        suite.check(created.has_value(), "create with async wal_io");
        if (!created) {
            return suite.finish();
        }
        auto db = std::make_shared<Database>(std::move(*created));
        auto id = DatabaseRegistry::instance().attach(db);
        suite.check(db->wal_io() == WalIoMode::async, "wal_io reports async");
        suite.check(db->bind(emp_builder()).has_value(), "bind");

        // Uma transação com várias imagens de página exercita o batching:
        // vários write_at enfileirados (begin + N page-images) drenados
        // juntos no primeiro wal->sync().
        {
            auto tx = db->begin();
            suite.check(tx.has_value(), "begin batch tx");
            if (tx) {
                for (int i = 0; i < 5; ++i) {
                    auto created_obj =
                        db->create(*tx, Emp{"batch-" + std::to_string(i), 1000 + i});
                    suite.check(created_obj.has_value(), "create in batch tx");
                    if (i == 0 && created_obj) {
                        first_id = created_obj->id();
                    }
                }
                suite.check(tx->commit().has_value(), "commit batch tx");
            }
        }

        // Mais alguns commits de uma página cada, para exercitar o caminho
        // comum de commit_transaction (begin+1 page-image+commit) também.
        for (int i = 0; i < 3; ++i) {
            auto tx = db->begin();
            suite.check(tx.has_value(), "begin single tx");
            if (tx) {
                suite.check(db->create(*tx, Emp{"single-" + std::to_string(i), 1}).has_value(),
                            "create single tx");
                suite.check(tx->commit().has_value(), "commit single tx");
            }
        }

        auto got = db->get<Emp>(first_id);
        suite.check(got.has_value(), "read back handle after async commits");
        if (got) {
            auto name = got->get<&Emp::name>();
            suite.check(name.has_value() && *name == "batch-0", "read back value after async commits");
        }
        last_checkpoint = db->checkpoint_lsn();

        if (id) {
            DatabaseRegistry::instance().detach(*id);
        }
    }

    // Reabre com o mesmo modo e confirma que o WAL gravado via AsyncFile é
    // lido corretamente na recuperação (mesmo formato, mesmo parser de
    // tx::Wal::read_all — só o backend de escrita mudou).
    {
        auto reopened = Database::open(path.path(), opts);
        suite.check(reopened.has_value(), "reopen with async wal_io");
        if (!reopened) {
            return suite.finish();
        }
        auto db = std::make_shared<Database>(std::move(*reopened));
        auto id = DatabaseRegistry::instance().attach(db);
        suite.check(db->bind(emp_builder()).has_value(), "bind after reopen");
        suite.check(db->checkpoint_lsn() >= last_checkpoint, "checkpoint restored");

        auto got = db->get<Emp>(first_id);
        suite.check(got.has_value(), "read back handle after reopen");
        if (got) {
            auto name = got->get<&Emp::name>();
            auto salary = got->get<&Emp::salary>();
            suite.check(name.has_value() && *name == "batch-0" && salary.has_value() &&
                            *salary == 1000,
                        "read back value after reopen");
        }

        auto records = tx::Wal::read_all(db->wal_path());
        suite.check(records.has_value() && records->size() >= 8,
                    "wal has begin/page-image/commit records for all transactions");

        if (id) {
            DatabaseRegistry::instance().detach(*id);
        }
    }

    return suite.finish();
}
