#include "modb/object/binding.hpp"
#include "modb/object/database.hpp"
#include "modb/object/primary_storage.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <system_error>

using namespace modb;
using namespace modb::object;

namespace {

class TempPath {
public:
    explicit TempPath(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-15a-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
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

struct Note {
    std::string text;
};

BindingBuilder<Note> note_builder() {
    BindingBuilder<Note> builder{"Note"};
    builder.field<1>("text", &Note::text);
    return builder;
}

} // namespace

int main() {
    TestSuite suite;

    suite.check(parse_primary_storage("full").value_or(PrimaryStorage::wal_only) ==
                    PrimaryStorage::full,
                "parse full");
    suite.check(parse_primary_storage("wal_only").value_or(PrimaryStorage::full) ==
                    PrimaryStorage::wal_only,
                "parse wal_only");
    suite.check(!parse_primary_storage("nope"), "parse invalid");

    {
        TempPath path{"default"};
        auto created = Database::create(path.path());
        suite.check(created.has_value() && created->primary_storage() == PrimaryStorage::full &&
                        created->has_durable_data_files(),
                    "default create is full");
    }

    {
        TempPath path{"wal"};
        DatabaseOptions opts;
        opts.primary_storage = PrimaryStorage::wal_only;
        opts.commit_ack = CommitAckPolicy::local_wal;
        auto created = Database::create(path.path(), opts);
        suite.check(created.has_value() && created->primary_storage() == PrimaryStorage::wal_only &&
                        !created->has_durable_data_files(),
                    "wal_only create");
        if (created) {
            auto ro = created->set_read_only_replica(true);
            suite.check(!ro && ro.error().code == ErrorCode::invalid_instance_config,
                        "follower+wal_only rejected");
        }
    }

    {
        TempPath path{"follower"};
        auto created = Database::create(path.path());
        suite.check(created.has_value(), "follower create full");
        if (created) {
            auto db = std::make_shared<Database>(std::move(*created));
            suite.check(db->set_read_only_replica(true).has_value(), "follower read-only ok");
            DatabaseOptions bad;
            bad.primary_storage = PrimaryStorage::wal_only;
            // Reabrir o mesmo path full como wal_only deve falhar (não é MCTL).
            auto reopened = Database::open(path.path(), bad);
            suite.check(!reopened, "full file cannot open as wal_only");
        }
    }

    // Durability::disabled_diagnostic (só medição, T5 do plano de desempenho):
    // o commit segue a mesma sequência sem fsync. Sem queda, o SO grava as
    // páginas de qualquer forma, então o dado reaparece numa reabertura normal.
    {
        TempPath path{"nosync"};
        DatabaseOptions opts;
        opts.durability = Durability::disabled_diagnostic;
        ObjectId id{};
        {
            auto created = Database::create(path.path(), opts);
            suite.check(created.has_value(), "create with durability=disabled_diagnostic");
            if (created) {
                auto db = std::make_shared<Database>(std::move(*created));
                auto attached = DatabaseRegistry::instance().attach(db);
                suite.check(attached.has_value() && db->bind(note_builder()).has_value(), "bind Note");
                auto tx = db->begin();
                suite.check(tx.has_value(), "begin without fsync");
                if (tx) {
                    auto handle = db->create(*tx, Note{"sem fsync"});
                    suite.check(handle.has_value() && tx->commit().has_value(),
                                "commit without fsync succeeds");
                    if (handle) {
                        id = handle->id();
                    }
                }
                if (attached) {
                    DatabaseRegistry::instance().detach(*attached);
                }
            }
        }
        auto opened = Database::open(path.path());
        suite.check(opened.has_value(), "reopen after a no-fsync session (no crash)");
        if (opened) {
            auto db = std::make_shared<Database>(std::move(*opened));
            auto attached = DatabaseRegistry::instance().attach(db);
            suite.check(attached.has_value() && db->bind(note_builder()).has_value(), "rebind Note");
            auto handle = db->get<Note>(id);
            suite.check(handle.has_value(), "the object id is found after reopen");
            if (handle) {
                auto note = db->materialize(*handle);
                suite.check(note.has_value() && note->text == "sem fsync",
                            "the committed object is there after a clean reopen");
            }
            if (attached) {
                DatabaseRegistry::instance().detach(*attached);
            }
        }
    }

    return suite.finish();
}
